# Network Service - design plan (FujiNet-style `N:` device + raw sockets)

Status: **proposal / planning**. Not yet implemented.

Goal: give the Beeb internet access through the existing services port, in
two layers that share one implementation:

1. **Raw sockets** - a Berkeley-ish TCP/UDP primitive (`connect`, `send`,
   `recv`, `close`, `listen`/`accept`, `dns_resolve`).
2. **A FujiNet-style `N:` device** on top - the Beeb opens a URL-shaped
   endpoint (`N:HTTP://…`, `N:TCP://…`, `N:TNFS://…`) and does
   `open`/`read`/`write`/`close`; the Pi runs the protocol.

Both are one new service on the `&FCA6` services port. The design reuses,
almost unchanged, three things this codebase already has:

- the **services-port command model** (`services.h`, `fat_service.c`) - a
  page-aligned command block in the JIM buffer, a handle index in the low
  nibble of the command register, results written back to the register;
- the **AUN non-blocking pattern** (`AUN/aun_emulator.c`) - the FRED write
  runs in FIQ and only *latches* a request (`aun_pending` + cp/addr); a
  registered poll callback does all lwIP work on the main loop. **lwIP is
  never touched from FIQ**;
- **lwIP** itself: `LWIP_TCP`/`LWIP_UDP`/`LWIP_DNS` are all on, 16 TCP PCBs,
  8 UDP PCBs, a 256 KB heap. (No TLS/altcp yet - see phase 4.)

## Why this shape

The FAT service is already `fopen`/`fread`/`fwrite`/`fclose` on a
*filename*. The `N:` device is the identical command shape with a **URL**
in place of the filename. So the high-level network API is "open a URL like
a file", and the raw-socket API is the same handle/buffer machinery one
level down. Nothing new is invented at the bus level.

## Placement

- New file `src/net_service.c`, registered from `services_emulator_init`.
- `services.h`: claim a command range in the free 45-255 space, e.g.

      #define SERVICE_CMD_NET_FIRST   45u   /* net_service.c */
      #define SERVICE_CMD_NET_LAST    79u

  (raw sockets 45-59, `N:` device 60-79, headroom to 79).
- A `net_poll()` registered with `Pi1MHz_Register_Poll` drives all lwIP
  work; the command handler (FIQ) only validates the block and latches a
  request per handle, exactly like AUN.

## Handle model

- A fixed table of **N handles** (start with 8; hard cap = `MEMP_NUM_TCP_PCB`
  = 16). The command register's low nibble selects the handle, same as the
  FAT service's file handle - so a Beeb program juggles up to 16 files and
  sockets in the same 0-15 index space, which is a nice consistency.
- Each handle holds: an lwIP `tcp_pcb`/`udp_pcb`, state
  (idle/resolving/connecting/connected/listening/closing/error), a Pi-side
  **RX ring** (frames arrive asynchronously; the Beeb drains on its own
  clock), and - for the `N:` device - a protocol-adapter context.
- **nIRQ** signals "data available / state changed" so the Beeb can be
  interrupt-driven instead of polling. This is a **services-framework**
  concern: `services_irq(source, status)` (already in `services_emulator.c`,
  used by AUN) owns the shared `+5` status register and the nIRQ line, keyed
  by the service's nIRQ source id - not a per-service FRED byte. The `+5`
  register (`&FCAB`) carries AUN's grandfathered ABI byte, so the net
  service should signal nIRQ through the same source but read its own state
  via its status command (FujiNet's PROCEED-then-STATUS model) rather than
  disturbing that byte; grow `services_irq` a line-only variant when that
  lands. See the stage plan's cross-stage decisions.

## Data flow

Same as the FAT read/write commands: bulk payload lives in the shared JIM
buffer, addressed by a 24-bit offset in the command block; the command
register returns a short result/length. TX copies out of JIM into a pbuf;
RX copies from the handle's ring into JIM. No lwIP buffer is ever exposed
to the Beeb.

## Command set (concrete, mirrors the FAT command block format)

Handle = low nibble of the command register. Multi-byte fields
little-endian. Result in the command register: `0` = OK, `>0` = error
(reuse the FatFs-style codes plus a small network-error range), bit 7 set =
still in progress (poll again) - identical to the FAT/AUN convention.

### Layer 1 - raw sockets (45-59)

    45 net_open      +1 = type (0 TCP, 1 UDP); returns a handle, sets state idle
    46 net_dns       +1.. hostname (0-term) -> +4..7 resolved IPv4 (async: bit7 while resolving)
    47 net_connect   +1..4 IPv4, +5..6 port           (async: bit7 while connecting)
    48 net_bind      +1..2 local port                 (for UDP / server listen)
    49 net_listen    server: accept inbound TCP; completion yields a new handle
    50 net_send      +1..3 length, +4..7 JIM src       -> actual sent
    51 net_recv      +1..3 max length, +4..7 JIM dst    -> actual read (0 = none yet)
    52 net_recv_avail                                   -> bytes waiting in the RX ring
    53 net_close     graceful close (async: bit7 until FIN/timeout)
    54 net_status                                       -> state byte + flags (connected, EOF, error)
    55 net_udp_sendto +1..4 IP +5..6 port +7..9 len +10..13 JIM src
    56 net_udp_recvfrom -> peer IP/port + payload into JIM

### Layer 2 - `N:` device (60-79), built on layer 1 + Pi-side adapters

    60 net_url_open  +1 = mode (read/write/dir), +2.. URL (0-term) -> handle
                     URL scheme picks the adapter: TCP: UDP: HTTP: HTTPS:
                     TELNET: TNFS: FTP: (grown over the phases below)
    61 net_url_read  +1..3 max length, +4..7 JIM dst    -> actual read
    62 net_url_write +1..3 length, +4..7 JIM src        -> actual written
    63 net_url_close
    64 net_url_status                                   -> state + HTTP code / EOF / error
    65 net_url_dir_*  (TNFS/FTP directory enumeration, readdir-style)

The `N:` layer maps 1:1 onto the FAT service's verbs, so the same Beeb
code path (and even a filing-system ROM) can address `N:HTTP://…` where it
would a local file.

## Protocol adapters (Pi side, grown incrementally)

- **TCP / UDP** - trivial pass-throughs over layer 1.
- **HTTP** - method + headers + chunked handling on the Pi (we already have
  a chunked decoder in the webserver); GET/POST/PUT/DELETE, status code in
  `net_url_status`. Read = response body, write = request body.
- **TELNET** - TCP plus IAC option negotiation stripped/handled on the Pi.
- **TNFS** (Tiny Network File System, Dylan Smith) - a network *filesystem*
  adapter, so `N:TNFS://server/path` behaves like the FAT read/dir commands
  against a remote share. Big ecosystem of retro content; strong fit with
  our existing FAT verbs.
- **HTTPS / FTP / SSH** - later; HTTPS needs a TLS library (phase 4).

## Phasing

1. **MVP** - layer-1 TCP: `net_open`/`dns`/`connect`/`send`/`recv`/
   `recv_avail`/`close`/`status`, non-blocking via `net_poll`, RX ring,
   nIRQ. Enough for telnet/BBS/gopher/IRC/NTP clients on the Beeb.
   Validate with a Beeb-side test *and* a host lwIP loopback test.
2. **`N:` device + HTTP + TCP/UDP schemes** - the FujiNet-shaped verbs and
   the HTTP adapter. Now the Beeb can GET/POST a URL with trivial code.
3. **UDP sockets + TELNET + TNFS** - datagram sockets and the two most
   useful adapters (remote terminal, remote filesystem).
4. **TLS / HTTPS** - add mbedtls (or lwIP altcp_tls). This is the big one:
   memory footprint and cert handling need their own assessment; keep it
   behind a cfg flag.
5. **FujiNet wire compatibility (evaluate, don't assume)** - read the
   FujiNet firmware `N:`-device + TNFS protocol and the cc65 BBC client;
   decide whether presenting a FujiNet-compatible face (so existing BBC
   FujiNet software runs) is cheap enough to prefer over our native verbs.

## FidoNet

Falls out of the above: binkp is just TCP, so a FidoNet mailer runs either
Beeb-side over `net_open TCP`/`N:TCP://`, or as a Pi-side gateway that polls
a hub and drops packets as files the Beeb reads via the FAT service - the
latter fits Pi1MHz's FatFs nature and needs only layer 1.

## Open decisions

- **Native verbs vs FujiNet compatibility** (phase 5) - ecosystem
  compatibility vs. implementation cost. Needs the protocol read first.
- **Blocking model on the Beeb** - all commands are poll-for-completion
  (bit 7 busy), matching FAT/AUN; a thin blocking wrapper can live in a ROM.
- **JIM buffer sharing** - the net service shares the 16 MB services buffer
  with FAT. Define non-overlapping regions, or give net its own address
  window, to avoid a download and a socket transfer colliding.
- **Security** - outbound internet from the Beeb. Gate behind a cfg flag
  (e.g. `net_enable`), consider a host allow/deny list, and note there is
  no inbound exposure unless `net_listen` is used. The `N:` model is
  inherently safer than raw sockets (the Beeb never drives lwIP directly).

## Risks

- **Main-loop starvation** - the cardinal rule (see the FAT/AUN work and
  never-error-to-a-mounted-ADFS): the FIQ handler must only latch; every
  lwIP call, every copy, happens in `net_poll` with bounded per-pass work.
- **PCB / heap limits** - 16 TCP PCBs and a 256 KB heap are shared with the
  webserver, WebDAV, AUN and DHCP/DNS. Cap net handles and size RX rings so
  a busy Beeb socket set cannot starve the management webserver.
- **TLS cost** - mbedtls is large in flash and RAM; phase 4 needs a real
  footprint budget before committing.
- **RX buffering** - asynchronous inbound data must land somewhere before
  the Beeb reads it; bound the per-handle ring and apply TCP back-pressure
  (advertise a small window) rather than dropping.

## Rough effort

Phase 1 (MVP TCP sockets) is the bulk of the new machinery and is
self-contained - a few hundred lines of `net_service.c` following the AUN
template, plus a host test. Phases 2-3 are mostly per-adapter code. Phase 4
(TLS) is a separate project. Phase 5 is a research task first.
