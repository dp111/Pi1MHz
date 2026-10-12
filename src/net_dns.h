/* net_dns.h - name lookups that apply the network's DNS domain.
 *
 * lwIP's resolver sends a name exactly as given and has no search list, so on
 * a network whose DNS only knows "nas.example.net" a lookup of
 * "nas" is answered NXDOMAIN by every resolver.  Every other
 * client turns the short name into the full one with the domain the DHCP
 * server hands out (option 15).  net_dns_lookup() does the same, as a stub
 * resolver does with "ndots:1" (RFC 1535 section 3, resolv.conf(5)):
 *
 *   - a name with a dot in it is looked up as it is (a trailing dot too);
 *   - a name of one label gets ".<domain>" appended;
 *   - the domain is the wifi_domain config key, else option 15 of the held
 *     DHCP lease; with neither, the name is looked up as it is.
 *
 * Use it in place of dns_gethostbyname() wherever a person typed the name.
 */
#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "config.h"
#include "wifi/wifi_lwip.h"
#include "lwip/dns.h"

/* Longest name a DNS message can carry (RFC 1035 section 2.3.4) is 253 text
   characters, plus the NUL. */
#define NET_DNS_NAME_MAX 254u

/* Build "<name>.<domain>" in out and return true when name is a single label
   that wants the domain; false (out untouched) when it should be looked up
   as given. */
static inline bool net_dns_search_name(const char *name, const char *domain,
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
       || strchr(name, ':') != NULL)
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

/* The domain in force: the config key, else the DHCP lease's, else NULL. */
static inline const char *net_dns_domain(void)
{
   const char *configured = config_get("wifi_domain");

   if (configured != NULL && configured[0] != '\0')
      return configured;
   return wifi_lwip_dhcp_domain();
}

static __attribute__((noinline, unused))
err_t net_dns_lookup(const char *name, ip_addr_t *addr,
                     dns_found_callback found, void *arg)
{
   char full[NET_DNS_NAME_MAX];

   /* lwIP copies the name into its table, so the buffer need not outlive the
      call; the callback gets lwIP's copy. */
   if (net_dns_search_name(name, net_dns_domain(), full, sizeof full))
      name = full;
   return dns_gethostbyname(name, addr, found, arg);
}

#endif
