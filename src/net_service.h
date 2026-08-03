#ifndef NET_SERVICE_H
#define NET_SERVICE_H

/*
  The IP/net service - Berkeley-ish TCP/UDP sockets on the &FCA6 services
  port (command range SERVICE_CMD_NET_FIRST..LAST in services.h).

  Shape follows the FAT and AUN services exactly: a page-aligned command
  block lives in the JIM buffer; the command-register low nibble selects a
  handle (0..NET_MAX_HANDLES-1, like a FAT file handle); the result byte is
  read back from the command register.  The FRED write handler runs in FIQ
  and only *latches* the request - all lwIP work happens on the main loop in
  net_poll(), never in FIQ (the cardinal rule: see the AUN service and
  never-error-to-a-mounted-ADFS).  Async operations return NET_PENDING (bit 7
  set); the Beeb re-issues the same command to poll for completion, exactly
  like the AUN TX/TX_POLL and FAT idioms.

  Stage 1 (this file): raw TCP client sockets + DNS.  UDP, listen/accept and
  the N: device verbs slot into the reserved range in later stages.

  ---- Command block layout (offsets from the page-aligned command pointer,
       multi-byte fields little-endian) --------------------------------------

    [0]        command number (NET_CMD_*)
    handle   = command-register low nibble (data & 0x0F)

    open  (45)  in : [1] type (NET_TYPE_TCP / NET_TYPE_UDP)
    dns   (46)  in : [1..] hostname, NUL-terminated
                out: [4..7] resolved IPv4 (network order: [4]=first octet)
    connect(47) in : [1..4] IPv4 (as dns out), [5..6] port
    send  (50)  in : [1..3] length, [4..7] JIM source offset (from DISC_RAM_BASE)
                out: [1..3] bytes actually queued
    recv  (51)  in : [1..3] max length, [4..7] JIM dest offset
                out: [1..3] bytes actually read (0 = none yet; result NET_EOF
                     once the peer closed and the ring has drained)
    recv_avail(52) out: [1..3] bytes waiting in the RX ring
    bind  (48)  in : [1..2] local port (UDP: bind now; TCP: kept for listen)
    listen(49)  -  first call opens the listener (NET_PENDING); each later
                   call yields the next accepted connection's handle in [1]
                   (NET_OK), or NET_PENDING while none is waiting
    udp_sendto(55) in : [1..4] IPv4, [5..6] port, [7..9] length,
                        [10..13] JIM source offset;  out: [7..9] length sent
    udp_recvfrom(56) in : [7..9] max length, [10..13] JIM dest offset
                     out: [1..4] peer IPv4, [5..6] peer port, [7..9] length
                          (0 = nothing waiting)
    close (53)  -  graceful close (NET_PENDING until FIN/timeout)
    status(54)  out: [1] state (net_state_t), [2] flags (NET_FLAG_*),
                     [3..6] remote IPv4, [7..8] remote port,
                     [9..11] bytes waiting in the RX ring

  Every result is a single byte read back from the command register:
    0x00        OK
    0x80        NET_BUSY - the command is latched but the poll has not run
                yet; spin (the FAT-service "BMI wait" idiom), clears in one
                poll pass
    0x01        NET_PENDING - async op still in progress; RE-ISSUE the command
                to poll again (bit-7-clear so the spin exits first)
    0x20        NET_EOF
    0x21..0x3F  NET_ERR_* errors
  The error range is kept clear of the FatFs FR_* codes (0..20) and the AUN
  codes (0..7) so a mixed-service Beeb error handler can tell the families
  apart.
*/

#include <stdint.h>

/* Emulator-table init: instance = nIRQ source id, address = services base. */
void net_service_init(uint8_t instance, uint8_t address);

/* ---- ABI constants (shared with the Beeb-side client and the tests) ----- */

/* Command numbers (must lie within SERVICE_CMD_NET_FIRST..LAST). */
#define NET_CMD_OPEN         45u
#define NET_CMD_DNS          46u
#define NET_CMD_CONNECT      47u
#define NET_CMD_BIND         48u   /* set local port (UDP bind / TCP listen) */
#define NET_CMD_LISTEN       49u   /* accept inbound TCP on the bound port    */
#define NET_CMD_SEND         50u
#define NET_CMD_RECV         51u
#define NET_CMD_RECV_AVAIL   52u
#define NET_CMD_CLOSE        53u
#define NET_CMD_STATUS       54u
#define NET_CMD_UDP_SENDTO   55u
#define NET_CMD_UDP_RECVFROM 56u
/* Layer 2 - the N: device: open a URL like a file (Stage 2). */
#define NET_CMD_URL_OPEN     60u   /* in [2..] URL (0-term); scheme picks adapter */
#define NET_CMD_URL_READ     61u   /* like recv, on the URL's stream              */
#define NET_CMD_URL_WRITE    62u   /* like send                                   */
#define NET_CMD_URL_CLOSE    63u
#define NET_CMD_URL_STATUS   64u   /* [1] state, [2] flags, [3..4] HTTP code      */

/* Socket type (open [1]). */
#define NET_TYPE_TCP         0u
#define NET_TYPE_UDP         1u

/* URL adapter (chosen from the scheme by net_url_open). */
#define NET_URL_TCP          0u
#define NET_URL_UDP          1u
#define NET_URL_HTTP         2u

/* Result byte. */
#define NET_OK               0x00u
#define NET_EOF              0x20u /* peer closed and the RX ring is drained */
#define NET_ERR_INUSE        0x21u /* handle already open                    */
#define NET_ERR_NOTOPEN      0x22u /* handle not open / wrong type           */
#define NET_ERR_PARAM        0x23u /* bad JIM offset/length or arguments     */
#define NET_ERR_DNS          0x24u /* name resolution failed                 */
#define NET_ERR_CONN         0x25u /* refused / reset / unreachable          */
#define NET_ERR_NOMEM        0x26u /* out of pcbs / heap                     */
#define NET_ERR_UNSUPPORTED  0x27u /* command not implemented (yet)          */
#define NET_ERR_DISABLED     0x28u /* net_enable=0 in Pi1MHz.cfg             */
/* NET_PENDING is bit-7-CLEAR on purpose.  Bit 7 set means "the command was
   latched in FIQ but the main-loop poll has not produced a result yet" - the
   Beeb spins on it (the FAT-service "BMI wait" idiom) and it clears within one
   poll pass.  An async operation (connect/dns/close) that is still in progress
   must therefore report a bit-7-CLEAR code so the spin exits; the Beeb then
   RE-ISSUES the command to poll again.  A busy-spin that never cleared would
   deadlock, since the Pi only re-evaluates on a fresh dispatch. */
#define NET_PENDING          0x01u /* async in progress: re-issue the command */
#define NET_BUSY             0x80u /* transient: poll has not run yet (spin)  */

/* status [1] - handle state. */
typedef enum {
   NET_ST_FREE       = 0u,
   NET_ST_IDLE       = 1u,   /* open, not connected                         */
   NET_ST_RESOLVING  = 2u,
   NET_ST_CONNECTING = 3u,
   NET_ST_CONNECTED  = 4u,
   NET_ST_LISTENING  = 5u,
   NET_ST_CLOSING    = 6u,
   NET_ST_ERROR      = 7u
} net_state_t;

/* status [2] - flag bits. */
#define NET_FLAG_CONNECTED   0x01u
#define NET_FLAG_RX_EOF      0x02u /* peer sent FIN                          */
#define NET_FLAG_ERROR       0x04u
#define NET_FLAG_RX_READY    0x08u /* bytes waiting in the RX ring           */

#define NET_MAX_HANDLES      8u    /* hard cap < MEMP_NUM_TCP_PCB (16)       */
#define NET_RX_RING_SIZE     8192u /* per-handle byte-stream ring            */

#endif /* NET_SERVICE_H */
