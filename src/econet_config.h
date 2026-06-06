/* econet_config.h - parsers for the econet cmdline.txt options.
 *
 * Pure C, no dependencies, host-testable. The options (values must not
 * contain spaces - get_cmdline_prop() stops at the first space):
 *
 *   econet_station=1.32          our station as net.stn (or just "32",
 *                                net defaults to 0)
 *   econet_station=ip            our station = the last octet of the
 *                                Pi's own IPv4 address (or "1.ip" to
 *                                also set the net to 1, or "ip.ip" to
 *                                take the net from the third octet too)
 *   econet_port=32768            local AUN listen UDP port
 *   econet_map=1.254=192.168.1.10,1.200=192.168.1.11:32769
 *                                comma-separated peer map entries:
 *                                net.stn=a.b.c.d[:port], port defaults
 *                                to 32768 (only needed when several
 *                                stations share one host IP)
 */

#ifndef ECONET_CONFIG_H
#define ECONET_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Parse "net.stn" or "stn". Returns false on syntax error or invalid
 * values (station must be 1-254, net 0-254). */
bool eco_parse_station(const char *s, uint8_t *net, uint8_t *stn);

/* Recognise the special econet_station values that derive the station
 * number (and optionally the net) from our own IPv4 address a.b.c.d:
 *   "ip"      -> stn = d,  *net = 0,          *net_from_ip = false
 *   "<n>.ip"  -> stn = d,  *net = n (0-254),  *net_from_ip = false
 *   "ip.ip"   -> stn = d,  (*net unused),     *net_from_ip = true
 * The station octet (d) and, when *net_from_ip, the net octet (c) are
 * filled in by the caller from the live address. Case-insensitive "ip".
 * Returns true on a match; false otherwise, in which case the caller
 * falls back to eco_parse_station. */
bool eco_station_is_ip(const char *s, uint8_t *net, bool *net_from_ip);

/* Parse a network number 0-254 (econet_learn). */
bool eco_parse_net(const char *s, uint8_t *net);

/* Parse econet_machine=<8 hex digits>: the 4 machine-peek reply bytes
 * (machine type lo, hi, NFS version minor, major). */
bool eco_parse_machine(const char *s, uint8_t id[4]);

/* Parse a decimal UDP port (1-65535). */
bool eco_parse_port(const char *s, uint16_t *port);

/* Callback invoked for each parsed map entry. ip_be is the IPv4
 * address as a u32 in network byte order ('a' in the lowest-addressed
 * byte). Return false to abort parsing. */
typedef bool (*eco_map_add_fn)(void *user, uint8_t net, uint8_t stn,
                               uint32_t ip_be, uint16_t port);

/* Parse the econet_map value, invoking 'add' per entry. Returns the
 * number of entries added, or -1 on a syntax error (entries before the
 * error have already been added). */
int eco_parse_map(const char *s, eco_map_add_fn add, void *user);

#endif
