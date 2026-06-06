#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "econet_config.h"

static struct { uint8_t net, stn; uint32_t ip; uint16_t port; } got[8];
static int n;
static bool add(void *u, uint8_t net, uint8_t stn, uint32_t ip, uint16_t port)
{ (void)u; got[n].net=net; got[n].stn=stn; got[n].ip=ip; got[n].port=port; n++; return true; }

int main(void)
{
    uint8_t net, stn; uint16_t port;

    /* station */
    assert(eco_parse_station("1.32", &net, &stn) && net==1 && stn==32);
    assert(eco_parse_station("32", &net, &stn) && net==0 && stn==32);
    assert(eco_parse_station("254.254", &net, &stn) && net==254 && stn==254);
    assert(!eco_parse_station("1.0", &net, &stn));      /* station 0 */
    assert(!eco_parse_station("255.1", &net, &stn));    /* net > 254 */
    assert(!eco_parse_station("1.32x", &net, &stn));    /* junk */
    assert(!eco_parse_station("", &net, &stn));
    assert(!eco_parse_station(NULL, &net, &stn));

    /* station = last octet of our IP (econet_station=ip / net.ip / ip.ip) */
    bool from_ip;
    net = 9; from_ip = true;
    assert(eco_station_is_ip("ip", &net, &from_ip) && net==0 && !from_ip);
    net = 9; from_ip = true;
    assert(eco_station_is_ip("IP", &net, &from_ip) && net==0 && !from_ip);
    net = 9; from_ip = true;
    assert(eco_station_is_ip("1.ip", &net, &from_ip) && net==1 && !from_ip);
    net = 9; from_ip = true;
    assert(eco_station_is_ip("254.ip", &net, &from_ip) && net==254 && !from_ip);
    from_ip = false;                               /* net from IP third octet */
    assert(eco_station_is_ip("ip.ip", &net, &from_ip) && from_ip);
    from_ip = false;
    assert(eco_station_is_ip("IP.IP", &net, &from_ip) && from_ip);
    assert(!eco_station_is_ip("32", &net, &from_ip));        /* literal station */
    assert(!eco_station_is_ip("1.32", &net, &from_ip));      /* literal net.stn */
    assert(!eco_station_is_ip("ipx", &net, &from_ip));       /* trailing junk */
    assert(!eco_station_is_ip("1.ipx", &net, &from_ip));     /* trailing junk */
    assert(!eco_station_is_ip("ipx.ip", &net, &from_ip));    /* bad prefix */
    assert(!eco_station_is_ip("255.ip", &net, &from_ip));    /* net > 254 */
    assert(!eco_station_is_ip("ip.ip.ip", &net, &from_ip));  /* too many */
    assert(!eco_station_is_ip("", &net, &from_ip));
    assert(!eco_station_is_ip(NULL, &net, &from_ip));
    /* a literal "ip" must NOT be accepted by the numeric station parser */
    assert(!eco_parse_station("ip", &net, &stn));

    /* port */
    assert(eco_parse_port("32768", &port) && port==32768);
    assert(!eco_parse_port("0", &port));
    assert(!eco_parse_port("65536", &port));
    assert(!eco_parse_port(NULL, &port));

    /* map */
    n = 0;
    assert(eco_parse_map("1.254=192.168.1.10,1.200=192.168.1.11:32769", add, NULL) == 2);
    assert(got[0].net==1 && got[0].stn==254 && got[0].port==0);
    assert(got[0].ip == (192u | 168u<<8 | 1u<<16 | 10u<<24));   /* network order */
    assert(got[1].port==32769 && got[1].ip == (192u | 168u<<8 | 1u<<16 | 11u<<24));
    n = 0;
    assert(eco_parse_map("0.200=10.0.0.7", add, NULL) == 1 && got[0].net==0);
    assert(eco_parse_map("", add, NULL) == 0);
    assert(eco_parse_map(NULL, add, NULL) == 0);
    assert(eco_parse_map("1.254=192.168.1.10,", add, NULL) == -1);  /* trailing comma */
    assert(eco_parse_map("1.254=192.168.1", add, NULL) == -1);      /* short ip */
    assert(eco_parse_map("1.0=10.0.0.1", add, NULL) == -1);         /* station 0 */
    assert(eco_parse_map("1.254=10.0.0.1:0", add, NULL) == -1);     /* port 0 */
    assert(eco_parse_map("garbage", add, NULL) == -1);

    printf("all econet_config tests passed\n");
    return 0;
}
