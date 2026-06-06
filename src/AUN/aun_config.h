/* aun_config.h - parsers for the sun cmdline.txt options.
 *
 * Pure C, no dependencies, host-testable. The options (values must not
 * contain spaces - get_cmdline_prop() stops at the first space):
 *
 *   aun_station=1.32          our station as net.stn (or just "32",
 *                                net defaults to 0)
 *   aun_port=32768            local AUN listen UDP port
 *   aun_map=1.254=192.168.1.10,1.200=192.168.1.11:32769
 *                                comma-separated peer map entries:
 *                                net.stn=a.b.c.d[:port], port defaults
 *                                to 32768 (only needed when several
 *                                stations share one host IP)
 */

#ifndef AUN_CONFIG_H
#define AUN_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Parse "net.stn" or "stn". Returns false on syntax error or invalid
 * values (station must be 1-254, net 0-254). */
bool aun_parse_station(const char *s, uint8_t *net, uint8_t *stn);

bool aun_station_is_ip(const char *s, uint8_t *net, bool *net_from_ip);

/* Parse a network number 0-254 (aun_learn). */
bool aun_parse_net(const char *s, uint8_t *net);

/* Parse aun_machine=<8 hex digits>: the 4 machine-peek reply bytes
 * (machine type lo, hi, NFS version minor, major). */
bool aun_parse_machine(const char *s, uint8_t id[4]);

/* Parse a decimal UDP port (1-65535). */
bool aun_parse_port(const char *s, uint16_t *port);

/* Callback invoked for each parsed map entry. ip_be is the IPv4
 * address as a u32 in network byte order ('a' in the lowest-addressed
 * byte). Return false to abort parsing. */
typedef bool (*aun_map_add_fn)(void *user, uint8_t net, uint8_t stn,
                               uint32_t ip_be, uint16_t port);

/* Parse the aun_map value, invoking 'add' per entry. Returns the
 * number of entries added, or -1 on a syntax error (entries before the
 * error have already been added). */
int aun_parse_map(const char *s, aun_map_add_fn add, void *user);

#endif
