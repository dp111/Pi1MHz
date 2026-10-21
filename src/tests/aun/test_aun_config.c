#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aun_config.h"

static struct { uint8_t net, stn; uint32_t ip; uint16_t port; } got[8];
static int n;
static bool add(void *u, uint8_t net, uint8_t stn, uint32_t ip, uint16_t port)
{ (void)u; got[n].net=net; got[n].stn=stn; got[n].ip=ip; got[n].port=port; n++; return true; }

int main(void)
{
    uint8_t net, stn, id[4];

    /* station */
    assert(aun_parse_station("1.32", &net, &stn) && net==1 && stn==32);
    assert(aun_parse_station("32", &net, &stn) && net==0 && stn==32);
    assert(aun_parse_station("254.254", &net, &stn) && net==254 && stn==254);
    assert(!aun_parse_station("1.0", &net, &stn));      /* station 0 */
    assert(!aun_parse_station("255.1", &net, &stn));    /* net > 254 */
    assert(!aun_parse_station("1.32x", &net, &stn));    /* junk */
    assert(!aun_parse_station("", &net, &stn));
    assert(!aun_parse_station(NULL, &net, &stn));

    /* station = last octet of our IP (aun_station=ip / net.ip / ip.ip) */
    bool from_ip;
    net = 9; from_ip = true;
    assert(aun_station_is_ip("ip", &net, &from_ip) && net==0 && !from_ip);
    net = 9; from_ip = true;
    assert(aun_station_is_ip("1.ip", &net, &from_ip) && net==1 && !from_ip);
    from_ip = false;
    assert(aun_station_is_ip("ip.ip", &net, &from_ip) && from_ip);
    from_ip = false;
    assert(aun_station_is_ip("IP.IP", &net, &from_ip) && from_ip);
    assert(!aun_station_is_ip("32", &net, &from_ip));
    assert(!aun_station_is_ip("1.32", &net, &from_ip));
    assert(!aun_station_is_ip("ipx", &net, &from_ip));
    assert(!aun_station_is_ip("ipx.ip", &net, &from_ip));
    assert(!aun_station_is_ip("255.ip", &net, &from_ip));
    assert(!aun_station_is_ip("ip.ip.ip", &net, &from_ip));
    assert(!aun_station_is_ip(NULL, &net, &from_ip));
    assert(!aun_parse_station("ip", &net, &stn));

    /* machine (aun_port= is text_parse_port_str: tests/textparse) */
    assert(aun_parse_machine("0a0B10ff", id) && id[0]==0x0A && id[1]==0x0B && id[2]==0x10 && id[3]==0xFF);
    assert(!aun_parse_machine("0a0b10f", id));          /* 7 digits */
    assert(!aun_parse_machine("0a0b10fff", id));        /* 9 digits */
    assert(!aun_parse_machine("0a0b10fg", id));         /* not hex */
    assert(!aun_parse_machine(NULL, id));

    /* map */
    n = 0;
    assert(aun_parse_map("1.254=192.168.1.10,1.200=192.168.1.11:32769", add, NULL) == 2);
    assert(got[0].net==1 && got[0].stn==254 && got[0].port==0);
    assert(got[0].ip == (192u | 168u<<8 | 1u<<16 | 10u<<24));   /* network order */
    assert(got[1].port==32769 && got[1].ip == (192u | 168u<<8 | 1u<<16 | 11u<<24));
    n = 0;
    assert(aun_parse_map("0.200=10.0.0.7", add, NULL) == 1 && got[0].net==0);
    assert(aun_parse_map("", add, NULL) == 0);
    assert(aun_parse_map(NULL, add, NULL) == 0);
    assert(aun_parse_map("1.254=192.168.1.10,", add, NULL) == -1);  /* trailing comma */
    assert(aun_parse_map("1.254=192.168.1", add, NULL) == -1);      /* short ip */
    assert(aun_parse_map("1.0=10.0.0.1", add, NULL) == -1);         /* station 0 */
    assert(aun_parse_map("1.254=10.0.0.1:0", add, NULL) == -1);     /* port 0 */
    assert(aun_parse_map("1.254=10.0.0.1:65536", add, NULL) == -1); /* port too big */
    assert(aun_parse_map("1.254=10.0.0.1:", add, NULL) == -1);      /* no port digits */
    assert(aun_parse_map("1.254=10.0.0.0001", add, NULL) == -1);    /* octet of 4 digits */
    assert(aun_parse_map("1.254=10.0.0.256", add, NULL) == -1);     /* octet over 255 */
    assert(aun_parse_map("garbage", add, NULL) == -1);

    printf("all aun_config tests passed\n");
    return 0;
}
