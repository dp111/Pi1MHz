/* net_dns.c - name lookups that apply the network's DNS domain: see
 * net_dns.h. */
#include <string.h>
#include <strings.h>

#include "net_dns.h"
#include "config.h"
#include "wifi/wifi_lwip.h"

bool net_dns_search_name(const char *name, const char *domain,
                         char *out, size_t size)
{
   size_t nlen, dlen;
   bool   digits = true;

   if (name == NULL || domain == NULL)
      return false;
   while (*domain == '.')
      domain++;
   nlen = strlen(name);
   dlen = strlen(domain);
   while (dlen > 0u && domain[dlen - 1u] == '.')
      dlen--;
   if (nlen == 0u || dlen == 0u || strchr(name, '.') != NULL
       || strchr(name, ':') != NULL || strcasecmp(name, "localhost") == 0)
      return false;
   for (size_t i = 0u; i < nlen; i++)
      if (name[i] < '0' || name[i] > '9')
         digits = false;
   if (digits)                       /* "3232235521" is an address, not a name */
      return false;
   if (nlen + 1u + dlen >= size || nlen + 1u + dlen > NET_DNS_NAME_MAX - 1u)
      return false;
   memcpy(out, name, nlen);
   out[nlen] = '.';
   memcpy(out + nlen + 1u, domain, dlen);
   out[nlen + 1u + dlen] = '\0';
   return true;
}

const char *net_dns_domain(void)
{
   const char *configured = config_get("wifi_domain");

   if (configured != NULL && configured[0] != '\0')
      return configured;
   return wifi_lwip_dhcp_domain();
}

/* A lookup that was given the domain, while lwIP has it: the caller's
   callback and name, kept so that a failure can be retried with the name as
   typed and the answer handed back under that name.  lwIP calls back exactly
   once per request (an answer, or NULL on failure or timeout), so a slot is
   always freed.  When every slot is busy the name is looked up as typed. */
#define NET_DNS_SLOTS 4u

typedef struct {
   bool               used;
   dns_found_callback found;
   void              *arg;
   ip_addr_t          ip;              /* a cached answer to the retry */
   char               name[NET_DNS_NAME_MAX];
} net_dns_slot_t;

static net_dns_slot_t s_slot[NET_DNS_SLOTS];

static void net_dns_answer(net_dns_slot_t *slot, const ip_addr_t *ipaddr)
{
   char               name[NET_DNS_NAME_MAX];
   dns_found_callback found = slot->found;
   void              *arg   = slot->arg;

   /* Free the slot first: the callback may start another lookup. */
   memcpy(name, slot->name, sizeof name);
   slot->used = false;
   found(name, ipaddr, arg);
}

/* The answer to "<name>.<domain>": passed on, or on failure the name as typed
   is tried instead. */
static void net_dns_found_full(const char *lwip_name, const ip_addr_t *ipaddr,
                               void *arg);

/* The answer to the retry with the name as typed: passed on as it is. */
static void net_dns_found_bare(const char *lwip_name, const ip_addr_t *ipaddr,
                               void *arg)
{
   net_dns_slot_t *slot = arg;

   (void)lwip_name;
   if (slot->used)
      net_dns_answer(slot, ipaddr);
}

static void net_dns_found_full(const char *lwip_name, const ip_addr_t *ipaddr,
                               void *arg)
{
   net_dns_slot_t *slot = arg;

   (void)lwip_name;
   if (!slot->used)
      return;
   if (ipaddr == NULL) {
      err_t e = dns_gethostbyname(slot->name, &slot->ip, net_dns_found_bare, slot);
      if (e == ERR_INPROGRESS)
         return;
      if (e == ERR_OK)
         ipaddr = &slot->ip;
   }
   net_dns_answer(slot, ipaddr);
}

err_t net_dns_lookup(const char *name, ip_addr_t *addr,
                     dns_found_callback found, void *arg)
{
   char            full[NET_DNS_NAME_MAX];
   net_dns_slot_t *slot = NULL;
   err_t           e;

   if (!net_dns_search_name(name, net_dns_domain(), full, sizeof full))
      return dns_gethostbyname(name, addr, found, arg);
   for (unsigned i = 0u; i < NET_DNS_SLOTS; i++)
      if (!s_slot[i].used) {
         slot = &s_slot[i];
         break;
      }
   if (slot == NULL)                    /* all busy: as typed, as before */
      return dns_gethostbyname(name, addr, found, arg);

   slot->used  = true;
   slot->found = found;
   slot->arg   = arg;
   strcpy(slot->name, name);            /* fits: shorter than full */
   /* lwIP copies the name into its table, so full need not outlive this. */
   e = dns_gethostbyname(full, addr, net_dns_found_full, slot);
   if (e == ERR_INPROGRESS)
      return e;
   if (e != ERR_OK)                     /* refused outright: try as typed */
      e = dns_gethostbyname(name, addr, net_dns_found_bare, slot);
   if (e != ERR_INPROGRESS)
      slot->used = false;              /* answered now, or failed now */
   return e;
}
