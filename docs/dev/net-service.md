# IP/net service - as-built reference

What is implemented today, and how the Beeb drives it. The forward-looking
design is in [network-service-plan.md](network-service-plan.md) /
[network-service-stages.md](network-service-stages.md); this page is the
as-built ABI and status. Source: `src/net_service.c` / `src/net_service.h`.
Off unless **`net_enable=1`** in `Pi1MHz.cfg`.

## Where it lives

A service on the `&FCA6` services port (command range 45-79 in `services.h`),
shaped exactly like the FAT and AUN services. The FRED write handler latches
the request in FIQ; `net_service_poll()` does every lwIP call on the main loop.
Async ops return `NET_PENDING`; the Beeb re-issues the command to poll. TCP is
written against lwIP's `altcp` (maps to `tcp_*` at `LWIP_ALTCP=0`; TLS-ready).

## Wire protocol (how the Beeb issues a command)

The command block for handle N (0-15) is at JIM offset `&FFF000 + N*&100`.

1. Set the 24-bit address pointer: `&FCA6`=low, `&FCA7`=mid, `&FCA8`=high.
2. Write the command byte then its args through the auto-incrementing data
   port `&FCA9` (or `!&FCA6=...` to write four bytes at once).
3. Dispatch: write `&F0+N` to the command register `&FCAA`.
4. Poll `&FCAA`: **bit 7 set = busy**, the poll hasn't run yet - spin. Then the
   byte is the result: `&00`=OK, `&01`=`NET_PENDING` (async in progress -
   re-dispatch), `&20`=EOF, `&21..&3F`=errors, `&28`=`NET_ERR_DISABLED`.
5. Result fields (resolved IP, byte counts, status) are read back from the
   block through the data port. Bulk payload lives in a JIM data buffer
   addressed by a 24-bit offset in the command block.

See `beeb/net/NETDEMO.BAS` (raw sockets) and `NETHTTP.BAS` (N: device) for
worked examples, and `net_service.h` for the exact field layouts.

## Commands

Layer 1 - raw sockets (handle = command-register low nibble, like a FAT file):

| # | Command | Notes |
|---|---------|-------|
| 45 | open | `[1]` type (0 TCP, 1 UDP) |
| 46 | dns | `[1..]` hostname → `[4..7]` IPv4 (async) |
| 47 | connect | `[1..4]` IPv4, `[5..6]` port (async) |
| 48 | bind | `[1..2]` local port (UDP bind now / TCP listen later) |
| 49 | listen | opens a listener; each poll yields an accepted handle in `[1]` |
| 50 | send | `[1..3]` len, `[4..7]` JIM src → `[1..3]` queued |
| 51 | recv | `[1..3]` max, `[4..7]` JIM dst → `[1..3]` read (EOF once closed+drained) |
| 52 | recv_avail | → `[1..3]` bytes waiting |
| 53 | close | graceful |
| 54 | status | `[1]` state, `[2]` flags, `[3..6]` remote IP, `[7..8]` port, `[9..11]` rx_avail |
| 55 | udp_sendto | `[1..4]` IP, `[5..6]` port, `[7..9]` len, `[10..13]` JIM src |
| 56 | udp_recvfrom | → `[1..4]` peer IP, `[5..6]` port, `[7..9]` len, payload to `[10..13]` JIM dst |
| 57 | irq | `[1]` 0=disarm (default) / 1=arm nIRQ — only arm if a handler is installed |

Layer 2 - the N: device (open a URL like a file):

| # | Command | Notes |
|---|---------|-------|
| 60 | url_open | `[2..]` `scheme://host[:port][/path]`; async resolve+connect(+HTTP request) |
| 61 | url_read | like recv; the HTTP adapter strips response headers, returns only the body |
| 62 | url_write | like send |
| 63 | url_close | |
| 64 | url_status | `[1]` state, `[2]` flags, `[3..4]` HTTP status code |

Schemes: `TCP:` (raw wrapper), `HTTP:` (GET + header-strip + status). `UDP:`,
POST/chunked and dir enumeration are not implemented yet.

## Design notes

- **RX**: an 8 KB per-handle NOINIT ring. TCP is a byte stream; UDP holds
  `[4 ip][2 port][2 len][payload]` records. Drop-free back-pressure - `recv_cb`
  returns `ERR_MEM` (without freeing) to park a pbuf lwIP re-presents once the
  Beeb drains, and the advertised window is clamped to the ring.
- **nIRQ**: **opt-in** (`irq`, command 57) and **disarmed by default**. When
  armed it is level-triggered via `services_irq_set` - asserted while any handle
  has unread RX, cleared when drained. A purely polling client (NETDEMO/NETHTTP)
  installs no IRQ handler, so it must leave nIRQ disarmed: a level-triggered
  line that stayed asserted with RX unread would lock the Beeb in an IRQ storm.
  A Beeb reset disarms it (the handler is gone). Found on hardware.
- **BBC reset**: all pcbs are torn down from the poll (never lwIP from
  init/FIQ); the handle table (BSS) is clean on first boot.
- Every JIM offset/length and the hostname/URL strings are bounds-checked
  (`net_buffer_ok`/`net_string_ok`) - the command block is untrusted.

## Status

- **Raw sockets (45-56): DONE, host-tested + hardware-validated** on a real
  BBC Master + Pi - TCP connect over WiFi, send, live DNS resolve, and a raw
  connect to a LAN server, all over the bus. (UDP 55/56 host-tested; the same
  lwIP UDP path is hardware-proven via AUN.)
- **N: device (60-64): host-tested + hardware-validated** - HTTP GET on a real
  Master against a controlled LAN server: body returned with response headers
  stripped, HTTP status 200, clean EOF. Host tests: `src/tests/net`, 104 checks
  under ASan/UBSan incl. a 40 k-iteration fuzzer.
- **nIRQ**: opt-in (`irq`, 57), default disarmed - see Design notes; a stuck
  nIRQ froze the Beeb when asserted for a polling client, fixed 2026-08-03.
- Not started: TLS/HTTPS, TELNET, TNFS, a native sideways-ROM `*`-command API.
