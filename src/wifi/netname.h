#ifndef WIFI_NETNAME_H
#define WIFI_NETNAME_H

/* Local-network name resolution, so the Pi can be reached by name
 * instead of an IP address:
 *
 *   NetBIOS - answers NetBIOS name-service queries on UDP 137.  Windows
 *             sends these as broadcasts, so "http://<hostname>/" works.
 *
 *   mDNS    - periodically multicasts an A-record announcement for
 *             "<hostname>.local"; mDNS resolvers (macOS, iOS, Linux,
 *             Windows 10+) cache it, making "http://<hostname>.local/"
 *             resolve.
 *
 * netname_init() is called once the lwIP netif is up. */

void netname_init(void);

#endif
