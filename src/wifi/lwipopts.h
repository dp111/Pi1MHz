#ifndef PI1MHZ_LWIPOPTS_H
#define PI1MHZ_LWIPOPTS_H

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

#define MEM_ALIGNMENT                   4

#define LWIP_RAW                        1
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
/* Econet/AUN: large fileserver bulk transfers arrive as fragmented UDP
 * datagrams (up to AUN_MAX_DATA = 8K + headers), so reassembly and
 * fragmentation are enabled. IP_REASS_MAX_PBUFS sized for one 8K
 * datagram in flight (8192/PBUF_POOL_BUFSIZE + slack). */
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              16

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
/* The receive window governs uploads exactly as the send window governs
 * downloads, and pushing files *to* the Pi is the common direction for a
 * file server.  It was left at 8 * MSS while the send window grew, which
 * capped WebDAV PUT well below what the link can carry.  Deliberately
 * larger than TCP_SND_BUF (44 vs 32 MSS): receive needs the headroom while
 * frames wait on SD writes.  PBUF_POOL_SIZE below has to cover a full window of frames
 * sitting in the pool waiting to be written to the card. */
#define TCP_WND                         (44 * TCP_MSS)   /* 64,240: the most that fits u16_t without window scaling */
/* The send window is the throughput limiter, so it is sized generously and
 * the heap is sized to fit several connections at that size.  tcp_write()
 * allocates from MEM_SIZE, so a send buffer is really a per-connection claim
 * on one shared heap: at 8 * MSS against a 32 KB heap, three saturated
 * downloads committed all of it and a fourth could not queue a byte, got a
 * 200 with an empty body, and was killed by the idle watchdog ~30 s later.
 *
 * Measured single-stream, same build otherwise:
 *
 *    32 KB heap,  4 * MSS   0.69 MB/s
 *    32 KB heap,  8 * MSS   1.11 MB/s   (only 3 of 4 streams complete)
 *    64 KB heap,  8 * MSS   1.13 MB/s
 *   128 KB heap, 16 * MSS   1.64 MB/s
 *
 * The window matters this much because the connection is latency-bound, not
 * bandwidth-bound: the refill waits for an ACK to be noticed by the
 * cooperative poll loop, several times longer than the 1-2 ms ping RTT would
 * suggest.  Aggregate across four streams exceeds single-stream throughput,
 * which is the same thing seen from the other side.
 *
 * An older note here said not to raise MEM_SIZE, after 64 KB was measured
 * making all four streams crawl.  That was before the SDPCM credit window was
 * honoured: the driver dropped frames on a failed transfer, so the small heap
 * was accidentally acting as an admission controller and anything larger
 * collapsed into RTO backoff.  With flow control in place that no longer
 * applies.
 *
 * MEMP_NUM_TCP_SEG has to cover TCP_SND_QUEUELEN, which lwIP derives from
 * TCP_SND_BUF - the build fails its own sanity check otherwise.
 */
#define TCP_SND_BUF                     (32 * TCP_MSS)
#define MEM_SIZE                        (256 * 1024)
#define MEMP_NUM_TCP_SEG                160
/* Sixteen, not eight.  A PCB is small, and running out is not a graceful
 * degradation: lwIP simply stops accepting, so the client gets nothing at all
 * and cannot tell a full server from a dead one.  Eight was reachable in
 * ordinary use - four downloads, a browser keeping connections alive and a
 * status poller was enough, and connections then failed instantly.  TIME_WAIT
 * makes it worse, since a closed connection holds its PCB for a while after
 * the transfer is over. */
#define MEMP_NUM_TCP_PCB                16
/* DHCP + DNS + NetBIOS + mDNS = 4 in use today; 8 leaves headroom for
   ad-hoc UDP without dipping into the unused-PCB pool. */
#define MEMP_NUM_UDP_PCB                8
/* Received frames are pool pbufs, so an upload holds up to a full TCP_WND
   of them until the webserver has written them to the card - and an SD
   write stall is exactly when the pool is most needed.  Sized to cover a
   full receive window (44 frames) with slack for PROPFIND and ARP/DHCP,
   which is what the pool-exhausted path used to drop. */
#define MEMP_NUM_PBUF                   96
#define PBUF_POOL_SIZE                  96
/* Bound out-of-order queueing.  lwIP's default is unlimited, and every
   out-of-order segment pins a pool pbuf: at a 44-segment receive window, two
   lossy uploads can pin 88 of the 96 and leave nothing for the ACKs, ARP,
   DHCP or AUN reassembly - including nothing for the retransmission that
   would fill the hole and release them, so the connections sit pinned until
   the idle watchdog aborts them. */
#define TCP_OOSEQ_MAX_PBUFS             16

#endif