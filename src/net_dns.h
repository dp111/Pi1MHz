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
 *   - a name of one label is looked up with ".<domain>" appended first, and
 *     as it is if that fails ("localhost" is never given the domain);
 *   - the domain is the wifi_domain config key, else option 15 of the held
 *     DHCP lease; with neither, the name is looked up as it is.
 *
 * Use it in place of dns_gethostbyname() wherever a person typed the name.
 */
#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdbool.h>
#include <stddef.h>

#include "lwip/dns.h"

/* Longest name a DNS message can carry (RFC 1035 section 2.3.4) is 253 text
   characters, plus the NUL. */
#define NET_DNS_NAME_MAX 254u

/* Build "<name>.<domain>" in out and return true when name is a single label
   that wants the domain; false (out untouched) when it should be looked up
   as given. */
bool net_dns_search_name(const char *name, const char *domain,
                         char *out, size_t size);

/* The domain in force: the config key, else the DHCP lease's, else NULL. */
const char *net_dns_domain(void);

/* dns_gethostbyname() with the domain applied as above.  Returns as it does;
   on ERR_INPROGRESS found() is called once, later, with the name as it was
   passed here (not the one sent to the server), so a caller may compare it
   with the name it asked for - except when more than four lookups are
   outstanding, when the fifth goes to lwIP as it is and is answered under
   lwIP's name for it.  found() must not start a lookup of the same name. */
err_t net_dns_lookup(const char *name, ip_addr_t *addr,
                     dns_found_callback found, void *arg);

#endif
