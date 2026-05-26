#ifndef PI1MHZ_LWIPOPTS_H
#define PI1MHZ_LWIPOPTS_H

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

#define MEM_ALIGNMENT                   4

#define LWIP_RAW                        0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_ICMP                       1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define IP_REASSEMBLY                   0
#define IP_FRAG                         0

#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ETHARP                     1
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_SINGLE_NETIF               1

#define ETH_PAD_SIZE                    0
#define LWIP_CHECKSUM_CTRL_PER_NETIF    0

#define LWIP_IGMP                       0
#define LWIP_MULTICAST_TX_OPTIONS       0

#define LWIP_HTTPD_CGI                  0
#define LWIP_HTTPD_SSI                  0

#endif