/* smb_client.c - the SMB connection behind storage=smb.

   One libsmb2 context, logged on to the share named in Pi1MHz.cfg:

       smb_server=192.168.0.10     (or a name the DNS knows)
       smb_share=Beeb
       smb_user=beeb
       smb_password=...
       smb_domain=...              (optional)
       smb_encrypt=1               (optional: seal every message)

   libsmb2 is asynchronous; the firmware's file calls are not.  fsroute.c
   starts an operation and then waits here, running the WiFi stack and
   libsmb2's event pump until the callback fires - the pattern of the USB
   drive's command_wait(), with the same 10 s limit, and the watchdog fed.
   A timeout or a dropped connection marks the link lost: every later call
   fails at once (the Beeb sees a disc error, never a hang) until the next
   BBC reset reconnects.  Main loop only, never FIQ. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>                      /* smb2.h needs time_t */

#include "lwip/ip_addr.h"
#include "lwip/dns.h"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include "smb_client.h"
#include "smb_socket.h"
#include "../config.h"
#include "../net_dns.h"
#include "../watchdog.h"
#include "../rpi/systimer.h"
#include "../wifi/wifi_lwip.h"

#define SMB_COMMAND_TIMEOUT_US  10000000u   /* as the USB drive's command limit */
#define SMB_NETWORK_WAIT_US     20000000u   /* power-on: WiFi join + DHCP */

static struct smb2_context *ctx;
static uint32_t session;                    /* bumped by every logon */
static bool     link_lost;
static bool     network_waited;             /* the power-on wait is used once */
static char     last_error[96];
static char     server_ip[24];          /* dotted, and ":port" if given */

static void drop(void);

static void note_error(const char *what)
{
   snprintf(last_error, sizeof last_error, "%s%s%s", what,
            ctx ? ": " : "", ctx ? smb2_get_error(ctx) : "");
}

bool smb_client_configured(void)
{
   const char *server = config_get("smb_server");
   const char *share  = config_get("smb_share");
   return server != NULL && server[0] != '\0' && share != NULL && share[0] != '\0';
}

struct smb2_context *smb_client_context(void)
{
   return (ctx != NULL && !link_lost) ? ctx : NULL;
}

bool smb_client_can_wait(void)
{
   return !wifi_lwip_in_poll();
}

/* One step of libsmb2: hand it the events that are ready. */
static void pump(void)
{
   if (ctx == NULL)
      return;
   int fd = smb2_get_fd(ctx);
   if (fd < 0)
      return;
   int rev = smbp_revents(fd, smb2_which_events(ctx));
   if (rev != 0 && smb2_service(ctx, rev) < 0) {
      note_error("connection");
      link_lost = true;
      drop();
   }
}

static bool wait_until(volatile bool *done, uint32_t limit_us)
{
   uint32_t t0 = RPI_GetSystemTime();
   while (!*done) {
      pump();
      if (*done)
         break;
      if (link_lost)
         return false;
      if (RPI_GetSystemTime() - t0 > limit_us)
         return false;
      wifi_lwip_poll();
      watchdog_feed();
   }
   return true;
}

bool smb_client_wait(volatile bool *done)
{
   if (!smb_client_can_wait())
      return false;
   if (!wait_until(done, SMB_COMMAND_TIMEOUT_US)) {
      if (!link_lost)
         snprintf(last_error, sizeof last_error, "no answer in %u s",
                  (unsigned)(SMB_COMMAND_TIMEOUT_US / 1000000u));
      link_lost = true;
      /* Now, not at the reset: a reply arriving later would be written
         into the caller's buffer, which is gone once we return.  Destroying
         the context cancels everything outstanding. */
      drop();
      return false;
   }
   return true;
}

uint32_t smb_client_session(void)
{
   return (ctx != NULL && !link_lost) ? session : 0u;
}

/* ---- connecting --------------------------------------------------------- */

static volatile bool dns_done;
static ip_addr_t     dns_addr;
static bool          dns_ok;

static void dns_found(const char *name, const ip_addr_t *addr, void *arg)
{
   (void)name; (void)arg;
   dns_ok = addr != NULL;
   if (dns_ok)
      ip_addr_copy(dns_addr, *addr);
   dns_done = true;
}

/* The server as a dotted address, ":port" kept: libsmb2's getaddrinfo is
   numeric only (smb_socket.c), and the firmware's DNS is asynchronous. */
static bool resolve(const char *server_port)
{
   char server[64];
   unsigned port = 0;
   snprintf(server, sizeof server, "%s", server_port);
   char *colon = strchr(server, ':');
   if (colon != NULL) {
      *colon = '\0';
      port = (unsigned)atoi(colon + 1);
   }
   if (!ipaddr_aton(server, &dns_addr)) {
      dns_done = false;
      dns_ok = false;
      err_t rc = net_dns_lookup(server, &dns_addr, dns_found, NULL);
      if (rc == ERR_OK)
         dns_ok = true;
      else if (rc != ERR_INPROGRESS || !wait_until(&dns_done, SMB_COMMAND_TIMEOUT_US) || !dns_ok) {
         snprintf(last_error, sizeof last_error, "cannot resolve %s", server);
         return false;
      }
   }
   if (port != 0u)
      snprintf(server_ip, sizeof server_ip, "%s:%u", ipaddr_ntoa(&dns_addr), port);
   else
      snprintf(server_ip, sizeof server_ip, "%s", ipaddr_ntoa(&dns_addr));
   return true;
}

static volatile bool connect_done;
static int           connect_status;

static void connected(struct smb2_context *smb2, int status, void *data, void *arg)
{
   (void)smb2; (void)data; (void)arg;
   connect_status = status;
   connect_done = true;
}

static void drop(void)
{
   if (ctx != NULL)
      smb2_destroy_context(ctx);     /* closes its socket through the shim */
   ctx = NULL;
   smbp_close_all();
}

static volatile bool network_up;

bool smb_client_connect(void)
{
   if (ctx != NULL)
      return !link_lost;
   if (!smb_client_configured() || !smb_client_can_wait())
      return false;

   /* At power-on the WiFi is still joining when the Beeb first asks: wait
      for an address, once.  Later a missing network fails at once. */
   const wifi_lwip_context_t *net = wifi_lwip_get_context();
   if (net == NULL || !net->address_ready) {
      if (network_waited) {
         snprintf(last_error, sizeof last_error, "no network");
         return false;
      }
      network_waited = true;
      uint32_t t0 = RPI_GetSystemTime();
      for (;;) {
         net = wifi_lwip_get_context();
         if (net != NULL && net->address_ready)
            break;
         if (RPI_GetSystemTime() - t0 > SMB_NETWORK_WAIT_US) {
            snprintf(last_error, sizeof last_error, "no network");
            return false;
         }
         wifi_lwip_poll();
         watchdog_feed();
      }
   }
   network_waited = true;

   if (!resolve(config_get("smb_server")))
      return false;

   ctx = smb2_init_context();
   if (ctx == NULL) {
      snprintf(last_error, sizeof last_error, "out of memory");
      return false;
   }
   link_lost = false;
   const char *user   = config_get("smb_user");
   const char *pass   = config_get("smb_password");
   const char *domain = config_get("smb_domain");
   const char *seal   = config_get("smb_encrypt");
   if (user != NULL)   smb2_set_user(ctx, user);
   if (pass != NULL)   smb2_set_password(ctx, pass);
   if (domain != NULL) smb2_set_domain(ctx, domain);
   smb2_set_workstation(ctx, "PI1MHZ");
   smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
   smb2_set_version(ctx, SMB2_VERSION_ANY);
   if (seal != NULL && seal[0] == '1')
      smb2_set_seal(ctx, 1);

   connect_done = false;
   if (smb2_connect_share_async(ctx, server_ip, config_get("smb_share"), user,
                                connected, NULL) != 0) {
      note_error("connect");
      drop();
      return false;
   }
   if (!wait_until(&connect_done, SMB_COMMAND_TIMEOUT_US) || connect_status != 0) {
      if (connect_done)
         note_error("logon");
      else if (!link_lost)
         snprintf(last_error, sizeof last_error, "no answer from %s", server_ip);
      drop();
      return false;
   }
   last_error[0] = '\0';
   session++;
   if (session == 0u)
      session = 1u;
   return true;
}

void smb_client_reset(void)
{
   if (link_lost)
      drop();
   link_lost = false;
}

void smb_client_status(char *out, size_t size)
{
   if (!smb_client_configured())
      snprintf(out, size, "not configured");
   else if (ctx != NULL && !link_lost)
      snprintf(out, size, "\\\\%s\\%s connected (%s)", config_get("smb_server"),
               config_get("smb_share"), server_ip);
   else
      snprintf(out, size, "\\\\%s\\%s %s%s", config_get("smb_server"),
               config_get("smb_share"), link_lost ? "link lost: " : "not connected",
               last_error[0] ? last_error : "");
}
