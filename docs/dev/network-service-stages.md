# Network service - stage-by-stage implementation plan

Detailed plan behind [network-service-plan.md](network-service-plan.md).
Each stage is self-contained and shippable; effort estimates are person-days
for one implementer.

## Implementation status (2026-08-03)

- **Stage 1 - raw TCP/UDP sockets + DNS: DONE + fully HW-validated.**
  `src/net_service.c` (commands 45-57): open/dns/connect/bind/listen/send/recv/
  recv_avail/close/status + udp_sendto/udp_recvfrom + irq(57), an 8 KB per-handle
  NOINIT RX ring with drop-free ERR_MEM back-pressure, and an **opt-in** nIRQ via
  `services_irq_set` (default disarmed - a stuck nIRQ froze the Beeb, fixed
  2026-08-03). Host-tested (`src/tests/net`, 104 checks + fuzzer, ASan/UBSan)
  **and HARDWARE-VALIDATED** on a real BBC Master + Pi across the whole surface:
  TCP connect/send/recv + live DNS, a **UDP sendto/recvfrom echo** round-trip,
  and the **Beeb as a TCP server** (bind/listen/accept + recv of inbound data,
  clean EOF). Gated by `net_enable=1` (off by default). Beeb clients:
  `beeb/net/NETDEMO.BAS` (client), `NETUDP.BAS` (UDP), `NETSRV.BAS` (server);
  built onto an .ssd with `beeb/net/mkssd.py`.
- **Stage 2 - N: device: DONE + HW-validated.** `url_open/read/write/close/status`
  (60-64), a `scheme://host[:port][/path]` parser, and the **TCP:** and
  **HTTP:** adapters (HTTP GET with header-strip + status-code parse). Host-
  tested and hardware-validated (HTTP GET on a real Master against a LAN server:
  body with headers stripped, status 200, clean EOF; `beeb/net/NETHTTP.BAS`).
  Not yet: HTTP POST/PUT/DELETE, chunked decoding, the UDP: scheme, dir enum.
- Stages 3-5 (UDP scheme/TELNET/TNFS, TLS, FujiNet compat): not started.
- Known follow-up: heavy churn of aborted TCP listeners can wedge the service's
  poll (needs a reflash to clear) - investigate the listener/reset teardown.

The per-stage detail below is the original plan.

## Cross-stage decisions (apply to every stage)

These were settled during planning and bind all stages:

- **Command map.** One service, `net_service.c`, claims the whole
  `SERVICE_CMD_NET_FIRST..LAST = 45..79` range in `services.h`. Raw sockets
  = 45-56; the `N:` device = 60-65; 57-59 and 66-79 answer
  `NET_ERR_UNSUPPORTED` so later stages slot in without re-registering.
  Emulator-table entry `{"net", net_service_init, 0xA6, 1}` shares the
  services base and goes **after** `wifi` and `aun` (poll order = table
  order, so `net_poll` runs after lwIP has drained frames).
- **IRQ is a services-framework concern, not a per-service FRED byte.**
  (No separate `&FCB1` - an earlier draft had one; that is exactly the
  bolt-on the services refactor exists to avoid.) The services port already
  owns the IRQ register at base+5 (`&FCAB`) and the shared nIRQ line. The
  net service does **not** define its own status byte: on an event it just
  raises the shared nIRQ through a services helper, and the Beeb - exactly
  like FujiNet's PROCEED-then-STATUS model - responds by issuing
  `net_status` (command 54) to find which handle fired.

  **What already exists (as of the services-IRQ refactor, commit e139813):**
  `services.h`/`services_emulator.c` provide

      /* Publishes `status` to the IRQ register (base+5, = &FCAB) AND
         asserts/clears nIRQ for `source`.  This is AUN's path - it writes
         the shared status byte. */
      void services_irq(uint8_t source, uint8_t status);

  AUN calls this (its `&FCAB` byte - bit7 ready, bit6 immediate, bits0-5
  frame count - is fixed AUNFS-ROM ABI and stays exactly as-is). The net
  service must **not** write that byte, so Stage 1 adds the anticipated
  line-only variant (the `services.h` comment already earmarks it):

      /* Raise/lower this service's contribution to the shared nIRQ line
         WITHOUT touching the &FCAB status byte.  The framework ORs sources
         and drives Pi1MHz_nIRQ_ASSERT/CLEAR. */
      void services_irq_set(uint8_t source, bool asserted);

  New services route their IRQ through `services_irq_set` and expose detail
  via their own status command, never a new FRED byte. (If a services-wide
  IRQ *status* register is ever wanted, design it into the framework from
  the start, with AUN remaining the legacy carve-out.)
- **Write the socket layer against lwIP's `altcp` API from day one**
  (`altcp_new`/`altcp_connect`/`altcp_write`/`altcp_recv`/`altcp_close`,
  `struct altcp_pcb *`). With `LWIP_ALTCP=0` this compiles to plain `tcp_*`
  at **zero cost**; Stage 4 then adds TLS by flipping one lwipopts flag
  instead of refactoring every callback. This is the single most important
  cross-stage decision - retrofitting altcp later touches every callback.
- **Non-blocking, FIQ-latch/poll-drives-lwIP.** The FRED write handler runs
  in FIQ and only latches `{pending, cp, addr, handle}` (the AUN one-slot
  mailbox). All lwIP work - and, crucially, **every lwIP callback** - runs
  in main-loop context inside `net_poll`/`wifi_lwip_poll`, never FIQ, never
  re-entrant (`NO_SYS=1`). lwIP is never called from FIQ. This is the
  cardinal rule (see the FAT/AUN work and never-error-to-a-mounted-ADFS).
- **Result convention** mirrors FAT/AUN: result byte `< 0xE0`, bit 7 set =
  in progress (Beeb re-issues to poll), a network error range `0x20-0x3F`
  kept clear of the FatFs `FR_*` (0-20) and AUN (0-7) codes so a mixed-
  service Beeb error handler can tell the families apart.
- **`N:` verbs follow FujiNet semantics** (for fujinet-lib source
  compatibility - Stage 5), while the *command-block format* stays ours
  (page-aligned block in JIM, handle = command-register low nibble). Raw
  sockets keep a richer native status block.
- **Host-testable** by the AUN pattern: adapters reach the socket layer
  only through a small injected ops struct; lwIP is stubbed on the host.
  Tests drive the real FRED dispatch under ASan/UBSan
  (`src/tests/{aun,services,webserver}` are the templates).
- **No ARMv8 crypto extensions.** Broadcom BCM2835/2837 silicon does not
  implement them - enabling `+crypto` would be a SIGILL repeat of the
  `d092e3f` CP15/A53 lesson. All crypto is software.
- **Security gates** in `Pi1MHz.cfg`: `net_enable` (whole service off by
  default), `net_tls`/`net_tls_verify` (Stage 4). A disabled service still
  *answers* (with `NET_ERR_DISABLED`) - never leaves a command unclaimed,
  which would hang a naive Beeb poll loop.

---

## Stage 1 - raw TCP/UDP socket primitive (45-56) ~ 9 days

New: `src/net_service.h` (ABI) and `src/net_service.c` (~700-900 lines,
AUN-emulator-shaped, single file - the "engine" here *is* lwIP, so the
testable seam is the lwIP call boundary). `src/tests/net/`.

**Handle table** - 8 handles (hard cap `< MEMP_NUM_TCP_PCB` 16 so the
webserver/AUN keep PCB headroom), selected by the command nibble like FAT:

    net_state_t: FREE, IDLE, RESOLVING, CONNECTING, CONNECTED,
                 LISTENING, CLOSING, ERROR
    per handle: type(TCP/UDP), state, last_err, altcp_pcb*/udp_pcb*,
                RX ring (8 KB, NOINIT, byte-stream for TCP /
                  [ip,port,len,payload] records for UDP), rx_eof,
                dns state, one-deep accept backlog, remote ip/port,
                irq_arm + events bitmap

**lwIP callbacks** (all main-loop context, `arg = &net_h[i]`):
`recv`/`sent`/`connected`/`err`/`poll`/`accept` (TCP), `dns_found`, `udp_recv`.
Key discipline copied from the webserver: on `err_cb` the pcb is **already
freed** - NULL it, never touch again.

**Back-pressure, drop-free** (the elegant part): at open, clamp the pcb's
`rcv_wnd`/`rcv_ann_wnd` to the 8 KB ring (the global 44×MSS `TCP_WND` is for
the webserver and must not apply to a Beeb socket). `recv_cb` copies a whole
pbuf into the ring or, if it won't fit, **returns `ERR_MEM` without freeing**
- lwIP parks the pbuf and re-presents it (≤250 ms) once the Beeb drains.
Nothing is dropped; the peer stalls in-order. UDP has no back-pressure -
ring-full drops the datagram and counts it.

**Commands**: `open`(45)/`dns`(46)/`connect`(47)/`bind`(48)/`listen`(49)/
`send`(50)/`recv`(51)/`recv_avail`(52)/`close`(53)/`status`(54)/
`udp_sendto`(55)/`udp_recvfrom`(56). Async ops (connect/dns/listen) return
`NET_PENDING (0x80)`; the Beeb re-issues the same command to poll (the AUN
TX/TX_POLL idiom). `recv` never blocks - returns actual=0 when empty,
`NET_EOF` once `rx_eof` and drained. `send` caps at `min(len, tcp_sndbuf,
8 KB)` with `TCP_WRITE_FLAG_COPY` and calls `wifi_lwip_rx_kick()` (the AUN
lesson: a send usually precedes a reply). Every JIM offset/length goes
through `net_buffer_ok()`/`net_string_ok()` clones of the FAT bounds checks
- the command block is untrusted.

**nIRQ**: via the framework, not a dedicated byte (see cross-stage
decisions). `net_poll` recomputes "any armed handle has an event" and calls
`services_irq_set(NET_IRQ_SOURCE, any)` on change; the services framework
ORs sources and drives `Pi1MHz_nIRQ_ASSERT/CLEAR`. The Beeb, on nIRQ, issues
`net_status` to learn which handle. A handle's event bits clear when the
Beeb reads its status/recv. **BBC RST** (init re-runs): set
`net_reset_pending`, tear down all pcbs from `net_poll` (never lwIP from
init/FIQ).

**Host tests**: stub `lwip/{tcp,udp,pbuf,dns,err}.h`, capture the callback
pointers so tests replay network events; assert the connect/dns/recv/back-
pressure/listen/nIRQ lifecycles; `fuzz_net.c` in the `fuzz_fat.c` shape.

**Risks**: main-loop starvation (one command/pass, all copies ≤8 KB, well
under the 1200 µs budget); pbuf-pool sharing (window clamp caps pinning at
~6 pbufs/handle); the `rcv_wnd` clamp touches lwIP fields (pin the version,
verify with a Wireshark window capture on hardware).

**Beeb-side client (see the plan doc's "Beeb-side client" decision):** Stage 1
ships a **BBC BASIC helper library** (`PROCnet_open`/`FNnet_connect`/... over
the command block, spinning the bit-7 poll) plus 2-3 self-contained examples
(telnet/BBS, NTP set, gopher or HTTP GET). This *is* the Stage-1 on-hardware
validation vehicle - it drives the real FRED dispatch from the Beeb - and gives
users something usable on day one. No 6502: the ROM `*`-command API waits for
Stages 2-3, once the verbs have stopped moving.

Tasks: ABI/range (0.5) · skeleton+gate+RST (0.5) · handle table+open/close/
status/bind (1) · RX ring+recv (1) · TX+sent (0.5) · connect+dns (0.5) ·
UDP (0.5) · listen/accept (0.5) · nIRQ (0.5) · host tests TDD (2) · Beeb-side
BASIC helper lib + telnet/NTP/gopher examples = the on-HW validation vehicle
(1.5) · web-UI/doc (0.5). **~10.5 d.**

---

## Stage 2 - `N:` device verbs + HTTP adapter (60-65) ~ 8 days

Built on Stage 1's socket layer. New: `src/net/net_url.c` (verbs),
`net_adapter.h` (the vtable), `http_parse.c`/`http_chunk.c` (parsers ported
- *not* shared - from the webserver), `adapter_tcp.c`, `adapter_http.c`.

**Pluggable adapter vtable** (scheme → adapter, scanned case-insensitively):

    net_adapter_ops_t { scheme, default_port,
        open(h,url,mode) read(h,dst,max,*act) write(h,src,len,*act)
        close(h) status(h,*out) dir_read(h,name,attr,size) poll(h) }

Adapters reach Stage 1 only through an injected `net_sock_ops_t`
(sock_open/connect/send/recv/avail/state/close, udp_sendto, now_ms) - so
they compile and test host-side with a scripted fake socket, exactly the
`aun_transport_t` pattern.

**Verbs** mirror the FAT command block byte-for-byte where a counterpart
exists (`url_open`≈fopen, `url_read`≈fread, `url_write`≈fwrite,
`url_dir`≈readdir, result 20 = EOF/end-of-dir), **but the verb *semantics*
follow FujiNet** for fujinet-lib compatibility:
- `url_open` aux/mode byte uses FujiNet's encoding: read/write/dir bits and
  the HTTP-method overload (GET=4, POST=13, PUT=8, DELETE=5).
- `url_status` (64) writes FujiNet's **4-byte DVSTAT**
  `{bytes_waiting_lo, bytes_waiting_hi, connected, error}` (plus our
  extended fields after it for native callers) - this is what fujinet-lib's
  `network_status()` expects.
- HTTP **channel mode** (`$FC`: body / collect-headers / get-headers /
  set-headers / send-post-data) selects what `url_read`/`url_write` move,
  so a client reads the body vs. reads captured response headers vs. writes
  request headers - the FujiNet HTTP model.

`url_open` returns fast (URL parsed, adapter *started*); DNS/connect/request
continue in background, surfaced via `url_status`. The FIQ handler validates
+ latches; `net_url_poll()` (called from `net_poll`) dispatches, runs each
active handle's `poll`, writes deferred completions back to the latched FRED
register, sweeps timeouts (AUN deadline style), mirrors nIRQ.

**HTTP adapter**: request built with `Connection: close` (self-terminating
bodies, no keep-alive bookkeeping); POST/PUT body buffered (heap, capped
32 KB) and committed on first `url_read`/`url_close` (correct
`Content-Length`, no chunked *requests*). Response header phase reuses the
webserver's proven scan/parse logic **re-implemented** in `http_parse.c`
(porting `ws_parse_request_line`/`ws_find_header`/`ws_find_header_end`); body
via Content-Length / chunked (`http_chunk.c` = the webserver's hardened
4-state `dav_put_consume_chunked` decoder, output redirected to a caller
buffer) / until-close. Status code delivered only via `url_status.proto_code`;
`url_read` returns pure body bytes. Redirects/keep-alive deliberately out
(documented).

**Tests**: `test_url.c` (URL parse vectors), `test_http.c` (fake socket
feeds canned responses one byte/pass - the `test_chunked.c` torture style:
Content-Length, chunked with extensions/trailers/overlong-size/overflow,
until-close, 404/302, split status lines, header overflow, POST echo),
`test_verbs.c`+`fuzz_verbs.c` (real dispatch, nibble/bounds/mailbox/busy-bit).

Tasks: adapter.h+URL parse (0.5) · net_url.c dispatch+deferred completion
(1) · adapter_tcp pass-through (0.5) · http_parse+http_chunk ports (1) ·
adapter_http GET (1.5) · chunked+POST/PUT/DELETE (1) · status/timeouts/nIRQ/
RST (0.5) · host tests+fuzz (1.5) · NETTEST.bas hardware (0.5) · **begin the
sideways-ROM `*`-command API** (`*NOPEN`/`*NCONNECT`/... over the now-stable
raw+`N:` verbs, loaded via the ROM helper) (1). **~8-9 d.**

Risks: malformed-HTTP robustness (port the hardened logic verbatim + fuzz);
per-pass streaming budget (cap bytes/pass/handle, rely on the Stage-1
window); JIM region collision with FAT (net uses the upper half of the disc
RAM window - a documented convention, enforced regions a TODO); POST body
memory (capped, freed on close/RST).

---

## Stage 3 - UDP scheme, TELNET, TNFS ~ 8-9.5 days

**UDP scheme** (`adapter_udp.c`): datagram-preserving read (one datagram per
`url_read`; `rx_avail` = next datagram length; truncation flagged). ~120 lines.

**TELNET** (`adapter_telnet.c`): a pure, host-testable RX IAC filter
(`telnet_rx_filter`) so the Beeb sees clean text, + TX escaping
(`0xFF`→`IAC IAC`). Minimal clean-text policy: accept server `WILL
ECHO`/`WILL SGA` (reply `DO`), refuse everything else, skip subnegotiations,
normalise `CR NUL`→`CR`; a respond-once bitmap prevents negotiation loops;
replies sent from `poll` via a small ring so `read` never stalls on TX.

**TNFS** (`adapter_tnfs.c` + `tnfs_proto.h`): TNFS *is* structurally
`aun_tx_t` - single outstanding UDP datagram per handle, deadline, resend,
give up (the `aun_poll` retry engine is the template). The wire protocol,
now known from research, is UDP **port 16384**, a **4-byte header**
`{ConnID(16 LE), seq(8), cmd(8)}`, POSIX-errno status byte (`0x21`=EOF),
and a message set that maps ~1:1 onto FatFs:

    MOUNT 0x00 / UMOUNT 0x01   ≈ f_mount
    OPEN 0x29 (flags16,mode16,path -> fd)   ≈ f_open
    READ 0x21 / WRITE 0x22 / CLOSE 0x23     ≈ f_read/f_write/f_close
    LSEEK 0x25  STAT 0x24(size32)  UNLINK 0x26  RENAME 0x28
    OPENDIR 0x10 / READDIR 0x11 / CLOSEDIR 0x12  ≈ f_opendir/readdir/closedir
    SIZE 0x30 / FREE 0x31

Presented through the same `url_open`/`read`/`write`/`dir` verbs the FAT
service already uses - so `N:TNFS://server/path` reads a remote share with
the Beeb-side code it would use for a local file. Sessions per-handle (mount
on open, umount on close); every verb is ≥1 round trip, so all use the
deferred-completion machinery; the retry/seq/dup-suppression discipline is
lifted from AUN.

**Tests**: pure `telnet_rx_filter`/`tx_escape` vectors (options split across
segments, SB spanning, IAC IAC, CR NUL, loop guard); UDP boundary/truncation;
`test_tnfs.c` state machine against a scripted fake peer through injected
`udp_sendto` (the `test_aun.c` stub-transport pattern) - mount/open/read
multi-round, retry-on-silence, exhaustion, dir enumeration, close/umount
ordering; then a python fake-TNFS-server integration (lockstep.py precedent)
and on-target vs `tnfsd` + a public telnet BBS.

Tasks: UDP+tests (1) · telnet filter/escape+tests (1.5) · adapter_telnet
glue (0.5) · tnfs_proto+ctx+retry engine vs stub builders (1.5) · file verbs
+deferred completion (1.5) · dir enumeration (1) · fake-peer tests+fuzz (1) ·
real wire + tnfsd validation (1-1.5) · **finish the ROM API** (OSWORD entry +
`N:`/TNFS `*` verbs; the ROM ABI crystallises here, verbs now stable) (1).
**~9-10.5 d.**

Risks: UDP retry storms (bounded backoff + seq matching); telnet option
pathologies (respond-once + SB cap); session leakage on Beeb RST (`net_url_init`
fires best-effort UMOUNTs); UDP PCB budget (only 8, shared with AUN/DHCP/DNS
- cap concurrent TNFS/UDP handles ~4, fail cleanly).

---

## Stage 4 - TLS / HTTPS ~ 9-11 days, gated on a 2-day spike

**Verdict: do it - `altcp_tls` + mbedTLS 3.6 LTS** - after Stages 1-3, and
only past a go/no-go spike build. The design doc's "mbedTLS is large in
RAM/flash" fear dissolves on this hardware:

- **RAM is a non-issue.** mbedTLS `calloc` lands in the **newlib heap**
  (`arm_setup_heap_limit` extends it to all ARM RAM - hundreds of MB), *not*
  the 256 KB lwIP pool (as long as `MBEDTLS_PLATFORM_MEMORY` stays
  undefined). ~40-50 KB peak per connection; cap at 2 TLS handles.
- **Code size fits.** ~+150-200 KB per kernel (ARM mode, per-dir `-Os`) -
  images grow ~390 KB → ~540-590 KB, loaded from SD into RAM, no flash
  wall. ~0.1 s extra boot load.
- **The real cost is one-off handshake CPU.** Broadcom silicon has **no
  ARMv8 crypto extensions**, so all-software P-256 ECDHE + cert verify is
  ~50-100 ms (A53) / ~150-400 ms (ARM1176), in one `net_poll` pass - an
  audible audio-poller glitch once per HTTPS open, not a crash (the 1 MHz
  bus itself is FIQ-serviced and unaffected). Mitigate with TLS session
  resumption (repeat opens skip the ECC); optional later restartable-ECC.

**Integration** is small because Stage 1 already speaks altcp: for an
`HTTPS:`/`TLS:` scheme, `altcp_tls_wrap(config, altcp_tcp_new(...))` +
**`mbedtls_ssl_set_hostname()`** (SNI + hostname verification - mandatory or
CDN sites fail; the in-tree glue omits it) + `altcp_connect`. The handshake
runs incrementally off `net_poll`'s existing timer/RX pumping; the handle
sits in `connecting` (bit-7 busy) across passes - no new state machine. The
HTTP adapter above is byte-for-byte unchanged (TLS is invisible).

**New pieces**: `src/rpi/rng.c` - the BCM2835 hardware RNG (same block on
both SoCs) exposed as `mbedtls_hardware_poll` via `MBEDTLS_ENTROPY_HARDWARE_ALT`
(~40 lines, satisfies mbedTLS's mandatory-entropy requirement). No RTC/NTP,
so leave `MBEDTLS_HAVE_TIME` off → X.509 skips notBefore/notAfter (signature
chain still checked); document it, add SNTP later to turn date checks on.

**Trust store, three cfg tiers**: `net_tls_verify = none` (encryption
without auth - honest and adequate for a retro toy on a trusted LAN;
**default** when TLS is on) / `= ca` with an SD-card bundle
`/Pi1MHz/certs.pem` (roots rotate faster than firmware; SD-centric fits
Pi1MHz) / baked-in minimal roots as fallback.

**Vendoring**: mbedTLS as a submodule (`.gitmodules` pattern) pinned to
v3.6.x; list the ~45 needed `library/*.c` in a `set(mbedtls_files ...)`
block (not mbedTLS's own CMake), per-dir `-Os`, a custom
`mbedtls_config.h` (TLS 1.2 client, ECDHE-ECDSA/RSA, AES-GCM+ChaCha20,
P-256/384/x25519, no time, no platform-memory). **Vendor the altcp_tls glue
into `src/wifi/altcp_tls/`** patched for mbedTLS 3.6 (the in-tree lwIP copy
targets 2.x - EOL; crib pico-sdk's 3.x patch), keeping the lwIP submodule
pristine. Explicitly do **not** add `+crypto` to `rpi3.cmake`.

**vs. an external HTTPS proxy** (Stage 2's HTTP adapter can already reach a
LAN gateway with zero new code): the proxy is the right *fallback doc note*,
but it breaks Pi1MHz's single-board promise. In-firmware TLS wins because
the footprint fits.

Tasks: (0) Stage-1 altcp precondition (0.5) · (1) **spike build + on-HW
handshake benchmark, go/no-go** (2) · rng.c (0.5) · vendor+patch glue,
flip lwipopts (1.5) · HTTPS scheme wiring + TLS error codes (1.5) · cfg +
SD CA bundle + CMake option (1) · host loopback + on-target (example.com,
a Cloudflare site for SNI, local `s_server`) + kernel7 boot smoke + audio-
glitch measure (2) · session resumption + docs + optional restartable-ECC
(1-2). **~9-11 d**, killable after the spike.

---

## Stage 5 - FujiNet ecosystem compatibility (research-led)

The research verdict: **there is essentially no existing BBC FujiNet
software** (only an in-progress cc65 BBC target + unpublished ROM from
"fenrock", aimed at ESP32 serial), so "run existing binaries unmodified" is
a non-goal. But FujiNet's model is *already a command mailbox* - every op is
`(device, cmd, aux1, aux2) + payload + status`, and the SIO frame is just
Atari's transport. Pi1MHz's JIM command block is a *better* carrier.

So Stage 5 is two concrete moves, both largely delivered by Stages 2-3:
1. **Implement FujiNet's verb semantics** on the `N:` device (done in Stage
   2: the OPEN aux modes, 4-byte DVSTAT, channel modes; TCP/UDP/HTTP/TELNET/
   TNFS adapters).
2. **Upstream a `bbc` platform to `fujinet-lib`** whose bus shim targets the
   Pi1MHz mailbox (pack cmd/aux/handle into registers, payload into JIM,
   poll/IRQ for completion). `fujinet-lib` is portable C where only the
   per-platform bus shim changes - so every cross-platform fujinet-lib app
   then compiles and runs on the Beeb *at source level*, which is where BBC
   FujiNet software will actually come from. TNFS mounts through the FAT-
   style verbs are the easy first win.

Coordinate via FujiNet's `#acorn-and-beebs` Discord and fenrock so the
`fujinet-lib` BBC target standardises on the Pi1MHz mailbox rather than a
serial link. Effort: the shim is small (days); the value is joining a live
ecosystem instead of forking one.

---

## Roll-up

| Stage | Scope | Effort |
|---|---|---|
| 1 | Raw TCP/UDP sockets (altcp) + BBC BASIC client & examples | ~10.5 d |
| 2 | `N:` device + HTTP (FujiNet verbs); begin the ROM `*`-cmd API | ~8-9 d |
| 3 | UDP scheme + TELNET + TNFS; finish the ROM API | ~9-10.5 d |
| 4 | TLS/HTTPS (gated on a 2-d spike) | ~9-11 d |
| 5 | fujinet-lib `bbc` bus shim + upstream | ~ few d |

Beeb-side client threads through: **BASIC helper + examples in Stage 1** (also
the validation vehicle), **promoted into a sideways ROM `*`-command/OSWORD API
across Stages 2-3** once the verbs stabilise, **fujinet-lib source-compat in
Stage 5**. So IP support is usable from BASIC on day one, not only at the end.

Stages 1-3 (~28 d) deliver a fully useful networked Beeb (telnet/BBS,
HTTP fetch/POST, remote TNFS filesystems, IRC/NTP/gopher, and the substrate
for a FidoNet mailer) with both a BASIC library and a native ROM API. Stage 4
adds HTTPS. Stage 5 plugs into the FujiNet ecosystem. Each stage is
independently shippable and independently hardware-validated.
