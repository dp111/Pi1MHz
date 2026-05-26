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
/* Skip the RFC-5227 address-conflict ARP probe that lwIP runs after the
   DHCP ACK.  That probe adds several seconds before the address can be
   used and is redundant on a network whose DHCP server already hands
   out unique leases. */
#define LWIP_DHCP_DOES_ACD_CHECK        0
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

/* --- TCP / memory tuning -------------------------------------------------
 * lwIP's stock defaults are tiny: TCP_MSS 536, TCP_SND_BUF ~1 KB and a
 * MEM_SIZE heap of only 1600 bytes.  tcp_write() copies outgoing data into
 * that heap, so the webserver can push barely 1 KB before it stalls and
 * then only crawls forward on the 2-second poll timer.  Size the buffers
 * for real throughput - the Pi has RAM to spare.  The netif MTU is 1500,
 * so a full-size 1460-byte MSS is correct.
 */
#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define MEM_SIZE                        (32 * 1024)
#define MEMP_NUM_TCP_SEG                40
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_UDP_PCB                6   /* DHCP + DNS + NetBIOS + mDNS */
#define MEMP_NUM_PBUF                   24
#define PBUF_POOL_SIZE                  24

#endif