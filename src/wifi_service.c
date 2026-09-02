/* Pi1MHz-side WiFi control service, speaking the ElkWiFi ROM's ABI.
 *
 * Radio on/off, scan, join/leave, interface config and link status for a
 * Beeb-side ROM.  The command numbers and the layout of each command block
 * are the ROM's, not ours: they can only change in lockstep with it, which
 * is why services.h pins the range and this file asserts the two agree.  The
 * names in here are local and mean nothing to the ROM.
 *
 * PING and DATETIME are in this range too, but the mechanism for them lives
 * in net_service.c, which owns lwIP - this file only formats their replies.
 *
 * The AP5 passes &FCA0-&FCAF, &FCFF and JIM to its 1 MHz connector. Pi1MHz's
 * existing services mailbox at &FCA6 therefore works on an unmodified AP5;
 * the original cartridge UART at &FC30 does not. Slow work is latched in FIQ
 * and completed from the main poll loop.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "Pi1MHz.h"
#include "scripts/gitversion.h"
#include "BeebSCSI/filesystem.h"
#include "config.h"
#include "wifi_service.h"
#include "ram_emulator.h"
#include "services.h"
#include "wifi/sdio.h"
#include "wifi/wifi.h"
#include "wifi/wifi_lwip.h"
#include "net_service.h"
#include "rpi/systimer.h"
#include "rpi/asm-helpers.h"

#define WIFI_SVC_TEXT_MAX 240u
/* Two small files this service owns, next to Pi1MHz.cfg on the card.  They
   are named for what they hold rather than for the ROM that asks for them,
   and both are plain text so they can be read, edited or deleted by hand.

   WiFi.profile is written by JOIN and applied at every init, so it overrides
   the wifi_ssid / wifi_password in Pi1MHz.cfg - delete it to go back to the
   configured network.  Its first line is a format tag: WIFIPROF1 means the
   four-line form below, and anything else is read as the original two-line
   ssid/password form.  A profile written before the rename says ELKWIFI1, so
   that tag is still accepted - without this, carrying an old file across
   under the new name would parse the tag itself as the SSID. */
#define WIFI_FILE "/Pi1MHz/WiFi.profile"
#define WIFI_PROFILE_HEADER "WIFIPROF1"
#define WIFI_PROFILE_HEADER_OLD "ELKWIFI1"
#define LAPOPT_FILE "/Pi1MHz/WiFi.scanopt"
/* AP5 exposes the standard 64K JIM window selected by &FCFF; it does not
 * forward Pi1MHz's extension selectors at &FCFD/&FCFE, and JIM page 0 is the
 * host's service reply buffer (OSWORD &65 clients read up to 241 contiguous
 * bytes of it).  The UEF stream's JIM layout notes travel with the UEF
 * cluster, which is held back from this submission. */
#define WIFI_SVC_VERSION_RESPONSE \
   "Pi1MHz ElkWiFi 0.1.67, kernel " GITVERSION "\r\n\r\nOK\r\n"

_Static_assert(WIFI_SVC_CMD_FIRST == SERVICE_CMD_WIFI_FIRST,
               "ElkWiFi service range start disagrees with services.h");
_Static_assert(WIFI_SVC_CMD_LAST == SERVICE_CMD_WIFI_LAST,
               "ElkWiFi service range end disagrees with services.h");

/* Off unless wifi_service_enable=1 in Pi1MHz.cfg.  When off, init returns before
   registering anything, so the command range stays unclaimed (the dispatcher
   echoes, exactly what the ElkWiFi ROM sees on a Pi without the service), no
   poll slot is taken and no profile is read from the card. */
static bool wifi_service_enabled;

static volatile bool request_pending;
static volatile bool request_cancel;
static volatile uint32_t request_pointer;
static volatile uint32_t request_status_address;
static bool service_initialised;
static bool scan_waiting;
static uint8_t scan_fields = 127u;

/* PING and DATETIME are served by net_service's async utilities: the
   mechanism is IP, so it lives with lwIP.  Kept here: the reply the ROM
   expects, and the UTC offset, which is this service's configuration. */
#define NTP_UNIX_EPOCH 2208988800u
#define NTP_SERVER "pool.ntp.org"
static uint32_t network_time_seconds;
static int16_t time_utc_offset_minutes;

static void response_string(uint32_t cp, const char *value);
static void response_printf(uint32_t cp, const char *format, ...)
   __attribute__((format(printf, 2, 3)));
static bool command_string(uint32_t cp, const char **value);




static void asynchronous_close(void)
{
   /* The ROM's CANCEL covers the whole command range, so it has to abort
      work owned by both layers: the two IP utilities live in net_service
      (each invalidates its own lwIP callbacks as it closes), the scan is
      ours. */
   net_ping_cancel();
   net_time_cancel();
   sdio_runtime_scan_cancel();
   scan_waiting = false;
}




static uint8_t wifi_ping(uint32_t cp)
{
   uint32_t elapsed_ms = 0u;

   switch (net_ping_poll(&elapsed_ms)) {
      case NET_UTIL_IDLE: {
         const char *host;
         if (!sdio_runtime_link_is_up() || !command_string(cp, &host)
             || host[0] == '\0' || !net_ping_start(host))
            return WIFI_SVC_ERR_NETWORK;
         return WIFI_SVC_BUSY;
      }
      case NET_UTIL_DONE:
         response_printf(cp, "+%lu\r\n", (unsigned long)elapsed_ms);
         return WIFI_SVC_OK;
      /* A ping that goes unanswered is a result, not a fault: the ROM
         prints it. */
      case NET_UTIL_TIMEOUT:
         response_string(cp, "+timeout\r\n");
         return WIFI_SVC_OK;
      case NET_UTIL_FAILED:
         return WIFI_SVC_ERR_NETWORK;
      default:
         return WIFI_SVC_BUSY;
   }
}




static bool leap_year(uint32_t year)
{
   return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static int16_t utc_offset_parse(const char *value)
{
   bool negative = false;
   int32_t parsed = 0;
   if (value == NULL || value[0] == '\0') return 0;
   if (*value == '+' || *value == '-') {
      negative = *value == '-';
      value++;
   }
   if (*value == '\0') return 0;
   while (*value != '\0') {
      if (*value < '0' || *value > '9') return 0;
      parsed = parsed * 10 + (*value++ - '0');
      if (parsed > 14 * 60) return 0;
   }
   return (int16_t)(negative ? -parsed : parsed);
}


static void format_network_time(uint32_t cp, bool date)
{
   static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
   int64_t adjusted = (int64_t)(network_time_seconds - NTP_UNIX_EPOCH)
                    + (int64_t)time_utc_offset_minutes * 60;
   uint32_t seconds = adjusted < 0 ? 0u : (uint32_t)adjusted;
   uint32_t days = seconds / 86400u;
   uint32_t year = 1970u;
   uint32_t month = 0u;
   while (days >= (leap_year(year) ? 366u : 365u))
      days -= leap_year(year++) ? 366u : 365u;
   while (month < 11u) {
      uint32_t length = month_days[month] + ((month == 1u && leap_year(year)) ? 1u : 0u);
      if (days < length) break;
      days -= length;
      month++;
   }
   if (date)
      response_printf(cp, "%02lu-%02lu-%04lu\r\n", (unsigned long)(days + 1u),
                      (unsigned long)(month + 1u), (unsigned long)year);
   else {
      seconds %= 86400u;
      response_printf(cp, "%02lu:%02lu:%02lu\r\n",
                      (unsigned long)(seconds / 3600u),
                      (unsigned long)((seconds / 60u) % 60u),
                      (unsigned long)(seconds % 60u));
   }
}

static uint8_t wifi_datetime(uint32_t cp)
{
   bool date = Pi1MHz->JIM_ram[cp + 1u] == 0u;

   switch (net_time_poll(&network_time_seconds)) {
      case NET_UTIL_IDLE:
         if (!sdio_runtime_link_is_up() || !net_time_start(NTP_SERVER))
            return WIFI_SVC_ERR_NETWORK;
         return WIFI_SVC_BUSY;
      case NET_UTIL_DONE:
         format_network_time(cp, date);
         return WIFI_SVC_OK;
      /* Unlike ping, a clock the ROM cannot read is an error, not a reply. */
      case NET_UTIL_TIMEOUT:
      case NET_UTIL_FAILED:
         return WIFI_SVC_ERR_NETWORK;
      default:
         return WIFI_SVC_BUSY;
   }
}

static const char *security_name(wifi_security_t security)
{
   switch (security) {
      case WIFI_SECURITY_OPEN: return "OPEN";
      case WIFI_SECURITY_WEP: return "WEP";
      case WIFI_SECURITY_WPA: return "WPA";
      case WIFI_SECURITY_WPA2: return "WPA2";
      case WIFI_SECURITY_AUTO:
      default: return "AUTO";
   }
}

static bool security_parse(const char *value, wifi_security_t *security)
{
   if (strcasecmp(value, "AUTO") == 0) *security = WIFI_SECURITY_AUTO;
   else if (strcasecmp(value, "OPEN") == 0) *security = WIFI_SECURITY_OPEN;
   else if (strcasecmp(value, "WEP") == 0) *security = WIFI_SECURITY_WEP;
   else if (strcasecmp(value, "WPA") == 0) *security = WIFI_SECURITY_WPA;
   else if (strcasecmp(value, "WPA2") == 0) *security = WIFI_SECURITY_WPA2;
   else return false;
   return true;
}

/* The stock two-argument command remains *JOIN ssid password and selects
 * WPA/WPA2 automatically.  A mode prefix is additive and needs no ROM ABI
 * change: OPEN, WEP:key, WPA:passphrase, WPA2:passphrase or AUTO:passphrase. */
static bool join_password_parse(const char *input, const char **password,
                                wifi_security_t *security)
{
   static const struct { const char *prefix; wifi_security_t security; } modes[] = {
      { "AUTO:", WIFI_SECURITY_AUTO }, { "WPA:", WIFI_SECURITY_WPA },
      { "WPA2:", WIFI_SECURITY_WPA2 }, { "WEP:", WIFI_SECURITY_WEP }
   };
   if (strcasecmp(input, "OPEN") == 0) {
      *password = "";
      *security = WIFI_SECURITY_OPEN;
      return true;
   }
   for (size_t i = 0u; i < sizeof modes / sizeof modes[0]; i++) {
      size_t length = strlen(modes[i].prefix);
      if (strncasecmp(input, modes[i].prefix, length) == 0) {
         *password = input + length;
         *security = modes[i].security;
         return true;
      }
   }
   *password = input;
   *security = WIFI_SECURITY_AUTO;
   return true;
}

/* Load the credentials saved by *JOIN before the cooperative WiFi boot state
 * machine gets its first poll.  wifi_reconfigure_and_rejoin() also handles
 * the no-Pi1MHz.cfg-SSID case by scheduling initial SDIO bring-up. */
static void wifi_credentials_load(void)
{
   uint8_t storage[WIFI_SSID_MAX_LEN + WIFI_PASSWORD_MAX_LEN + 32u];
   uint8_t *data = storage;
   uint32_t length = filesystemReadFile(WIFI_FILE, &data,
                                        (unsigned int)(sizeof storage - 1u));
   uint32_t split = 0u;
   wifi_security_t security = WIFI_SECURITY_AUTO;
   const char *ssid;
   const char *password;

   if (length == 0u || length >= sizeof storage)
      return;
   data[length] = '\0';
   while (split < length && data[split] != '\r' && data[split] != '\n')
      split++;
   if (split == 0u || split == length)
      return;
   data[split++] = '\0';
   while (split < length && (data[split] == '\r' || data[split] == '\n'))
      split++;
   ssid = (const char *)data;
   if (strcmp(ssid, WIFI_PROFILE_HEADER) == 0
       || strcmp(ssid, WIFI_PROFILE_HEADER_OLD) == 0) {
      uint32_t mode_end = split;
      while (mode_end < length && data[mode_end] != '\r' && data[mode_end] != '\n')
         mode_end++;
      if (mode_end == length) return;
      data[mode_end++] = '\0';
      if (!security_parse((const char *)&data[split], &security)) return;
      while (mode_end < length && (data[mode_end] == '\r' || data[mode_end] == '\n'))
         mode_end++;
      ssid = (const char *)&data[mode_end];
      split = mode_end;
      while (split < length && data[split] != '\r' && data[split] != '\n') split++;
      if (split == length) return;
      data[split++] = '\0';
      while (split < length && (data[split] == '\r' || data[split] == '\n')) split++;
   }
   if (strlen(ssid) == 0u || strlen(ssid) > WIFI_SSID_MAX_LEN) return;
   {
      uint32_t end = split;
      while (end < length && data[end] != '\r' && data[end] != '\n')
         end++;
      if (end - split > WIFI_PASSWORD_MAX_LEN)
         return;
      data[end] = '\0';
   }
   password = (const char *)&data[split];
   /* A BBC/Electron reset re-registers this service but does not reset the
    * Pi or CYW43. Do not turn an already-live association into a full rejoin
    * merely because the saved profile was read again. That rejoin can take
    * more than a minute on a busy access point and was the slow-reset
    * regression. Changed credentials still take the normal rejoin path. */
   if (sdio_runtime_started()) {
      const wifi_config_t *current = wifi_get_config();
      if (current != NULL && strcmp(current->ssid, ssid) == 0
          && strcmp(current->password, password) == 0
          && current->security == security)
         return;
   }
   (void)wifi_reconfigure_and_rejoin(ssid, password, security);
}

static void lapopt_load(void)
{
   uint8_t storage[8];
   uint8_t *data = storage;
   uint32_t length = filesystemReadFile(LAPOPT_FILE, &data,
                                        (unsigned int)(sizeof storage - 1u));
   if (length == 0u || length >= sizeof storage)
      return;
   data[length] = '\0';
   while (length != 0u && (data[length - 1u] == '\r' || data[length - 1u] == '\n'))
      data[--length] = '\0';
   if (strcmp((const char *)data, "7") == 0)
      scan_fields = 7u;
   else if (strcmp((const char *)data, "127") == 0)
      scan_fields = 127u;
}

static void response_printf(uint32_t cp, const char *format, ...)
{
   char value[WIFI_SVC_TEXT_MAX + 1u];
   va_list args;
   va_start(args, format);
   int length = vsnprintf(value, sizeof value, format, args);
   va_end(args);
   if (length < 0)
      value[0] = '\0';
   value[WIFI_SVC_TEXT_MAX] = '\0';
   response_string(cp, value);
}

static void ip_text(char out[16], const wifi_ipv4_addr_t *ip)
{
   (void)snprintf(out, 16u, "%u.%u.%u.%u", (unsigned)ip->octets[0],
                  (unsigned)ip->octets[1], (unsigned)ip->octets[2],
                  (unsigned)ip->octets[3]);
}

static bool two_strings(uint32_t start, const char **first, const char **second)
{
   uint32_t limit = DISC_RAM_BASE + DISC_RAM_SIZE;
   uint32_t end = start;
   if (start >= limit)
      return false;
   while (end < limit && end - start <= WIFI_SSID_MAX_LEN
          && Pi1MHz->JIM_ram[end] != 0u)
      end++;
   if (end >= limit || end - start > WIFI_SSID_MAX_LEN)
      return false;
   *first = (const char *)&Pi1MHz->JIM_ram[start];
   start = end + 1u;
   end = start;
   while (end < limit && end - start <= WIFI_PASSWORD_MAX_LEN
          && Pi1MHz->JIM_ram[end] != 0u)
      end++;
   if (end >= limit || end - start > WIFI_PASSWORD_MAX_LEN)
      return false;
   *second = (const char *)&Pi1MHz->JIM_ram[start];
   return true;
}

static uint8_t wifi_join(uint32_t cp)
{
   const wifi_config_t *config = wifi_get_config();
   uint8_t operation = Pi1MHz->JIM_ram[cp + 1u];

   /* Stock ROM revisions differ here: some turn JOIN ? into operation zero,
      while others pass the literal question-mark byte through. Both spell
      the same standard association query. */
   if (operation == (uint8_t)'?')
      operation = WIFI_SVC_JOIN_QUERY;

   if (operation == WIFI_SVC_JOIN_QUERY) {
      if (sdio_runtime_rejoin_busy()) {
         response_string(cp, "WIFI CONNECTING\r\n\r\nOK\r\n");
         return WIFI_SVC_OK;
      }
      if (!sdio_runtime_link_is_up()) {
         response_string(cp, "No AP\r\n\r\nOK\r\n");
         return WIFI_SVC_OK;
      }
      response_printf(cp, "+CWJAP:\"%s\"\r\n\r\nOK\r\n", config->ssid);
      return WIFI_SVC_OK;
   }
   if (operation == WIFI_SVC_JOIN_LEAVE) {
      if (!wifi_disconnect())
         return WIFI_SVC_ERR_NETWORK;
      response_string(cp, "WIFI DISCONNECT\r\n\r\nOK\r\n");
      return WIFI_SVC_OK;
   }
   if (operation == WIFI_SVC_JOIN_RADIO_OFF) {
      if (!wifi_disable_radio())
         return WIFI_SVC_ERR_NETWORK;
      response_string(cp, "WIFI OFF\r\n\r\nOK\r\n");
      return WIFI_SVC_OK;
   }
   if (operation != WIFI_SVC_JOIN_SET)
      return WIFI_SVC_ERR_PARAM;

   const char *ssid;
   const char *password;
   wifi_security_t security;
   if (!two_strings(cp + 2u, &ssid, &password) || ssid[0] == '\0')
      return WIFI_SVC_ERR_PARAM;
   if (!join_password_parse(password, &password, &security))
      return WIFI_SVC_ERR_PARAM;
   if (!wifi_profile_is_valid(ssid, password, security))
      return WIFI_SVC_ERR_PARAM;
   char persisted[WIFI_SSID_MAX_LEN + WIFI_PASSWORD_MAX_LEN + 32u];
   int length = snprintf(persisted, sizeof persisted, "%s\n%s\n%s\n%s\n",
                         WIFI_PROFILE_HEADER, security_name(security), ssid, password);
   if (length < 0 || (size_t)length >= sizeof persisted
       || filesystemWriteFile(WIFI_FILE, (const uint8_t *)persisted,
                              (uint32_t)length) != (uint32_t)length)
      return WIFI_SVC_ERR_IO;
   if (!wifi_reconfigure_and_rejoin(ssid, password, security))
      return WIFI_SVC_ERR_NETWORK;
   /* Association runs in the cooperative WiFi poll and owns the SDIO runtime
      for dozens of setup ioctls. Never keep the shared ElkWiFi command page
      pending across that work: a host timeout followed by LAP/IFCFG would
      overwrite the still-live JOIN request. Acknowledge the accepted request
      immediately; JOIN ? and IFCFG expose its live outcome. */
   response_printf(cp, "WIFI CONNECTING %s\r\n\r\nOK\r\n",
                   security_name(security));
   return WIFI_SVC_OK;
}

static uint8_t wifi_ifcfg(uint32_t cp)
{
   const wifi_network_config_t *net = wifi_get_network_config();
   const wifi_lwip_context_t *live = wifi_lwip_get_context();
   sdio_runtime_status_t radio = sdio_runtime_get_status();
   uint8_t mac[6] = {0};
   char ip[16] = "0.0.0.0";
   if (net->has_address) ip_text(ip, &net->address);
   /* DHCP owns the live values in lwIP; the parsed configuration object
      intentionally contains no address in DHCP mode.  Report the netif so
      *IFCFG changes from zeroes as soon as JOIN has obtained its lease. */
   if (radio.link_up && live != NULL && live->netif_added) {
      const ip4_addr_t *live_ip = netif_ip4_addr(&live->netif);
      (void)snprintf(ip, sizeof ip, "%u.%u.%u.%u", ip4_addr1(live_ip),
                     ip4_addr2(live_ip), ip4_addr3(live_ip), ip4_addr4(live_ip));
   }
   (void)sdio_runtime_get_chip_mac(mac);
   /* Function 18 is a public ElkWiFi ABI, not the Pi diagnostic surface.
      Keep it original-compatible and comfortably below the ROM's bounded
      239-byte mailbox copy. Applications such as ElkChat consume these exact
      records; Pi-specific link detail is available through *ONLINE. */
   response_printf(cp,
      "+CIFSR:STAIP,\"%s\"\r\n+CIFSR:STAMAC,\"%02x:%02x:%02x:%02x:%02x:%02x\"\r\n"
      "\r\nOK\r\n",
      ip, (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
      (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
   return WIFI_SVC_OK;
}

static uint8_t wifi_online(uint32_t cp)
{
   const wifi_lwip_context_t *live = wifi_lwip_get_context();
   sdio_runtime_status_t radio = sdio_runtime_get_status();

   if (radio.link_up && live != NULL && live->netif_added) {
      const ip4_addr_t *address = netif_ip4_addr(&live->netif);
      if (!ip4_addr_isany_val(*address)) {
         response_printf(cp, "ONLINE %u.%u.%u.%u\r\n",
                         ip4_addr1(address), ip4_addr2(address),
                         ip4_addr3(address), ip4_addr4(address));
         return WIFI_SVC_OK;
      }
   }
   if (radio.join_busy || radio.link_up)
      response_string(cp, "OFFLINE CONNECTING\r\n");
   else if (wifi_get_state() == WIFI_STATE_DISABLED)
      response_string(cp, "OFFLINE WIFI OFF\r\n");
   else if (wifi_get_state() == WIFI_STATE_ERROR)
      response_string(cp, "OFFLINE ERROR\r\n");
   else
      response_string(cp, "OFFLINE\r\n");
   return WIFI_SVC_OK;
}

static uint8_t wifi_scan(uint32_t cp)
{
   sdio_wifi_scan_result_t results[SDIO_WIFI_SCAN_MAX_RESULTS];
   char response[WIFI_SVC_TEXT_MAX + 1u];
   size_t used = 0u;

   if (!scan_waiting) {
      if (!sdio_runtime_scan_start())
         return WIFI_SVC_ERR_NETWORK;
      scan_waiting = true;
      return WIFI_SVC_BUSY;
   }
   if (sdio_runtime_scan_busy())
      return WIFI_SVC_BUSY;
   if (!sdio_runtime_scan_complete()) {
      scan_waiting = false;
      return WIFI_SVC_ERR_NETWORK;
   }

   uint8_t count = sdio_runtime_scan_results(results,
                                              SDIO_WIFI_SCAN_MAX_RESULTS);
   response[0] = '\0';
   for (uint8_t i = 0u; i < count; ++i) {
      int written;
      if (scan_fields == 7u) {
         written = snprintf(&response[used], sizeof response - used,
            "+CWLAP:(%u,\"%s\",%d)\r\n", (unsigned)results[i].security,
            results[i].ssid, (int)results[i].rssi);
      } else {
         written = snprintf(&response[used], sizeof response - used,
            "+CWLAP:(%u,\"%s\",%d,\"%02x:%02x:%02x:%02x:%02x:%02x\",%u)\r\n",
            (unsigned)results[i].security, results[i].ssid, (int)results[i].rssi,
            (unsigned)results[i].bssid[0], (unsigned)results[i].bssid[1],
            (unsigned)results[i].bssid[2], (unsigned)results[i].bssid[3],
            (unsigned)results[i].bssid[4], (unsigned)results[i].bssid[5],
            (unsigned)results[i].channel);
      }
      if (written < 0 || (size_t)written >= sizeof response - used)
         break;
      used += (size_t)written;
   }
   if (used == 0u)
      (void)snprintf(response, sizeof response, "OK\r\n");
   else
      (void)snprintf(&response[used], sizeof response - used, "\r\nOK\r\n");
   response_string(cp, response);
   scan_waiting = false;
   return WIFI_SVC_OK;
}

static bool command_string(uint32_t cp, const char **value)
{
   uint32_t limit = DISC_RAM_BASE + DISC_RAM_SIZE;
   if (cp + 1u >= limit)
      return false;
   for (uint32_t pos = cp + 1u;
        pos < limit && pos <= cp + WIFI_SVC_TEXT_MAX + 1u; pos++) {
      if (Pi1MHz->JIM_ram[pos] == 0u || Pi1MHz->JIM_ram[pos] == '\r') {
         Pi1MHz->JIM_ram[pos] = 0u;
         *value = (const char *)&Pi1MHz->JIM_ram[cp + 1u];
         return true;
      }
   }
   return false;
}

static void response_string(uint32_t cp, const char *value)
{
   size_t length = 0u;
   while (length < WIFI_SVC_TEXT_MAX && value[length] != '\0')
      length++;
   memcpy(&Pi1MHz->JIM_ram[cp + 1u], value, length);
   Pi1MHz->JIM_ram[cp + 1u + length] = 0u;
}

static uint8_t process_request(uint32_t cp)
{
   uint8_t command = Pi1MHz->JIM_ram[cp];
   switch (command) {
      case WIFI_SVC_CMD_STATUS:
         /* Version discovery identifies the installed Pi service. It is not
          * a radio-health probe and must remain available while CYW43 setup,
          * association or DHCP is incomplete. Radio control has its own
          * command and IFCFG/ONLINE expose the live network state. */
         response_string(cp, WIFI_SVC_VERSION_RESPONSE);
         return WIFI_SVC_OK;

      case WIFI_SVC_CMD_RADIO:
         /* Public function 24 must return promptly. Start radio setup here,
          * but do not make the caller wait for firmware or association. */
         if (wifi_get_state() == WIFI_STATE_DISABLED && !wifi_enable_radio())
            return WIFI_SVC_ERR_NO_WIFI;
         if (wifi_get_state() == WIFI_STATE_ERROR)
            return WIFI_SVC_ERR_NO_WIFI;
         response_string(cp, "OK\r\n");
         return WIFI_SVC_OK;

      case WIFI_SVC_CMD_SCAN:
         return wifi_scan(cp);

      case WIFI_SVC_CMD_JOIN:
         return wifi_join(cp);

      case WIFI_SVC_CMD_IFCFG:
         return wifi_ifcfg(cp);

      case WIFI_SVC_CMD_ONLINE:
         return wifi_online(cp);

      case WIFI_SVC_CMD_LAPOPT:
         if (Pi1MHz->JIM_ram[cp + 1u] != 7u
             && Pi1MHz->JIM_ram[cp + 1u] != 127u)
            return WIFI_SVC_ERR_PARAM;
         scan_fields = Pi1MHz->JIM_ram[cp + 1u];
         {
            const char *option = scan_fields == 7u ? "7\n" : "127\n";
            uint32_t length = (uint32_t)strlen(option);
            if (filesystemWriteFile(LAPOPT_FILE, (const uint8_t *)option,
                                    length) != length)
               return WIFI_SVC_ERR_IO;
         }
         response_printf(cp, "+CWLAPOPT:%u\r\n\r\nOK\r\n",
                         (unsigned)scan_fields);
         return WIFI_SVC_OK;

      case WIFI_SVC_CMD_PING:
         return wifi_ping(cp);

      case WIFI_SVC_CMD_DATETIME:
         return wifi_datetime(cp);

      case WIFI_SVC_CMD_CANCEL:
         asynchronous_close();
         response_string(cp, "OK\r\n");
         return WIFI_SVC_OK;

      default:
         return WIFI_SVC_ERR_UNSUPPORTED;
   }
}

void wifi_service_command(uint32_t cp, uint32_t addr, uint8_t data)
{
   (void)data;
   if (Pi1MHz->JIM_ram[cp] == WIFI_SVC_CMD_CANCEL) {
      request_pointer = cp;
      request_status_address = addr;
      request_cancel = true;
      Pi1MHz_MemoryWrite(addr, WIFI_SVC_BUSY);
      return;
   }
   if (request_pending) {
      Pi1MHz_MemoryWrite(addr, WIFI_SVC_BUSY);
      return;
   }

   /* STATUS is version discovery, not a radio-presence test. It is a fixed
      memory reply and can complete in the FIQ callback even while radio setup
      is in progress or has failed. All filesystem, scan and network work
      remains deferred below. */
   if (Pi1MHz->JIM_ram[cp] == WIFI_SVC_CMD_STATUS) {
      response_string(cp, WIFI_SVC_VERSION_RESPONSE);
      Pi1MHz_MemoryWrite(addr, WIFI_SVC_OK);
      return;
   }
   request_pointer = cp;
   request_status_address = addr;
   request_pending = true;
   Pi1MHz_MemoryWrite(addr, WIFI_SVC_BUSY);
}

static void wifi_service_poll(void)
{
   uint32_t cp, addr;
   if (request_cancel) {
      cp = request_pointer;
      addr = request_status_address;
      asynchronous_close();
      request_pending = false;
      request_cancel = false;
      response_string(cp, "OK\r\n");
      Pi1MHz_MemoryWrite(addr, WIFI_SVC_OK);
      return;
   }
   if (!request_pending)
      return;
   cp = request_pointer;
   addr = request_status_address;
   uint8_t result = process_request(cp);
   if (result == WIFI_SVC_BUSY)
      return;
   request_pending = false;
   request_cancel = false;
   Pi1MHz_MemoryWrite(addr, result);
}

void wifi_service_init(uint8_t instance, uint8_t address)
{
   (void)instance;
   (void)address;
   wifi_service_enabled = config_get_bool("wifi_service_enable");
   if (!wifi_service_enabled)
      return;
   /* Pi1MHz clears its main poll table on every BBC reset.  The services
      registry is idempotent, so renew this claim on every init as well: this
      recovers if a future reset path also clears the registry and avoids a
      one-shot failure when another optional service filled the old table. */
   if (!service_initialised) {
      service_initialised = true;
      lapopt_load();
      time_utc_offset_minutes = utc_offset_parse(
         config_get("wifi_service_utc_offset_minutes"));
   }
   /* Re-read the saved profile on every host reset, but credentials_load
    * preserves an already-running association when the profile is unchanged. */
   wifi_credentials_load();
   (void)services_register(WIFI_SVC_CMD_STATUS, WIFI_SVC_CMD_LAST,
                           wifi_service_command);
   /* A host reset abandons the old OSWORD/star-command caller. Complete any
      request latched during reset reinitialisation with a bounded error before
      clearing it, so FCAA can never be left at BUSY. */
   asynchronous_close();
   unsigned int cpsr = _disable_interrupts_cspr();
   if (request_pending || request_cancel)
      Pi1MHz_MemoryWrite(request_status_address, WIFI_SVC_ERR_NETWORK);
   request_pending = false;
   request_cancel = false;
   scan_waiting = false;
   _restore_cpsr(cpsr);
   Pi1MHz_Register_Poll(wifi_service_poll, "wifisvc");
}
