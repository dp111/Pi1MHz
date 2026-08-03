# Pi1MHz net service — BBC BASIC client

`NETDEMO.BAS` is a BBC BASIC library + demo that drives the Pi1MHz IP/net
service (the `&FCA6` services port, commands 45–65) — the raw-socket layer
(45–57) plus the `N:` device URL verbs (60–65) described in
[../../docs/dev/network-service-stages.md](../../docs/dev/network-service-stages.md).

It is the reference Beeb-side client and the vehicle for hardware-testing the
firmware net service. A native sideways-ROM `*`-command API comes in a later
stage; this BASIC library is deliberately first so the ABI can be shaken out
from the Beeb side while it is still settling.

`mkssd.py` packs all of the demos below into a bootable SSD (`*OPT4,3` runs a
`!BOOT` menu). Each is a plain-text listing — `*EXEC <name>` or paste it in:

| Demo | Layer | Does |
|------|-------|------|
| `NETDEMO`  | raw sockets | TCP connect / send / recv / DNS |
| `NETUDP`   | raw sockets | UDP send + recvfrom (echo) |
| `NETSRV`   | raw sockets | TCP server: bind / listen / accept |
| `NETHTTP`  | N: device   | `HTTP://` GET (headers stripped by firmware) |
| `NETTNFS`  | N: device   | `TNFS://` directory list + file read |
| `NETTNFSW` | N: device   | `TNFS://` file write |
| `NETTEL`   | N: device   | `TELNET://` session (firmware IAC filter) |

The N: device (`FNurl_open("SCHEME://host/path")` then `FNurl_read`/`FNurl_write`)
picks an adapter from the URL scheme; edit `url$`/`H` and `RUN`.

## Prerequisites

- A Pi1MHz build with the net service (`src/net_service.c`).
- **`net_enable=1` in `/Pi1MHz/Pi1MHz.cfg`** — the service is OFF by default.
  With it off, every command returns `NET_ERR_DISABLED` (`&28`).
- WiFi configured and associated (the demo waits for an IP).

## Running the demo

1. Get `NETDEMO.BAS` onto the Beeb (e.g. copy it to an MMFS/DFS disc, or type
   it in). It is a plain text listing — `*EXEC NETDEMO` or paste it in.
2. Edit lines 130–150 for your target: `host$` (a dotted-quad IP), `port`,
   `path$`, and the socket handle `H` (0–15).
3. `RUN`. It opens a TCP socket, connects, sends an HTTP/1.0 GET, prints the
   reply, and closes.

To resolve a name instead of hard-coding an IP, call `FNdns(H, "example.com")`
before connecting — on success it leaves the four octets in `dns0..dns3`.

## The library (PROCs/FNs)

| Call | Does |
|------|------|
| `FNopen(N,T)` | open handle N (T=0 TCP, 1 UDP) → result code |
| `FNconnect_str(N,IP$,P)` | TCP connect to `"a.b.c.d"`:P (blocks polling) |
| `FNdns(N,HOST$)` | resolve HOST$ → `dns0..dns3`; returns result |
| `FNsend(N,S$)` | send S$ → bytes queued (−1 on error) |
| `FNrecv(N,B,MX)` | recv ≤MX bytes into JIM offset B; sets `nlen`; returns code |
| `PROCclose(N)` | close handle N |

Result codes (`net_service.h`): `0`=OK, `&01`=NET_PENDING (async — re-issue;
the library's `FNgo_wait` does this for you), `&20`=EOF, `&21…&3F`=errors,
`&80`=busy (transient — spin; `FNgo` does this for you).

## How the wire protocol works

The command block for handle N lives at JIM offset `&FFF000 + N*&100`. To run a
command: set the 24-bit address pointer (`&FCA6/7/8`) to that block, write the
command byte then its arguments through the auto-incrementing data port
(`&FCA9`), then write `&F0+N` to the command register (`&FCAA`) to dispatch.
Poll `&FCAA`: bit 7 set = the poll has not run yet (spin); then the byte is the
result — `&01` means re-dispatch (async still in progress), anything else is
final. Result fields (resolved IP, bytes sent/received, status) are read back
from the block through the data port. Bulk payload lives in a JIM data buffer
(`txb`/`rxb` in the demo) addressed by a 24-bit offset in the command block.
