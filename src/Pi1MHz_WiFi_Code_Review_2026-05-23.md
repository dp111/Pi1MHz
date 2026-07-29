# Pi1MHz WiFi Subsystem — Code Review

**Date:** 2026-05-23
**Scope:** every C and header file under `src/wifi/` *except* the third-party
lwIP source tree (`wifi/lwip/`). Files reviewed: `webserver.c/.h`,
`netname.c/.h`, `wifi.c/.h`, `wifi_lwip.c/.h`, `sdio.c/.h`, `sdio_host.c/.h`,
`cyw43.c/.h`, `framebuffer_export.c/.h`, `lwipopts.h`.
**Method:** four detailed parallel passes over the subsystem, followed by
verification of every High and Medium finding directly against the source.
Line numbers are as of this review and may shift with later edits.

## Summary

One High finding, two Medium, and a set of Low/hardening items. No
memory-corruption, use-after-free, or RX-buffer-overrun bug was found in the
normal operating paths — the SDIO receive parsers and the webserver's lwIP
callback handling are, on inspection, correctly bounded. Two issues initially
flagged as "Critical" were investigated and **ruled out** (see the last
section); they are recorded so it is clear they were considered.

| Severity | Count |
|----------|-------|
| High | 1 |
| Medium | 2 |
| Low / Improvement | 12 |

---

## High

### H1 — WiFi RX/TX path stacks ~5 KB of frame buffers on the bare-metal stack

**Location:** `wifi_lwip.c` — `wifi_lwip_drain_rx_frames()` and
`wifi_lwip_link_output()`; `sdio.c` — `sdio_runtime_send_ethernet_frame()`.

**Issue:** `wifi_lwip_drain_rx_frames()` declares `uint8_t frame[1600]` and
holds it live while it calls `netif->input()`. That call runs
`ethernet_input → ip4_input → tcp_input`, and an incoming segment frequently
triggers an immediate transmit (an ACK or a response), which calls
`netif->linkoutput` = `wifi_lwip_link_output()` — which declares its **own**
`uint8_t frame[1600]` — which calls `sdio_runtime_send_ethernet_frame()` —
which declares `uint8_t tx_frame[SDIO_RUNTIME_MAX_FRAME_SIZE]` (~1600+).

So on the nested path `drain_rx → input → tcp → link_output → send_ethernet`,
roughly **4.8 KB of frame buffers plus all of lwIP's own call frames are live
on the stack simultaneously**.

**Impact:** on a bare-metal target with a fixed (and not large) stack this is
a real stack-overflow risk — and a stack overflow is silent memory corruption,
not a clean failure. The project does build with `-fstack-usage`, so the
per-function numbers are visible, but the *cumulative* depth of this specific
nested path is the thing to check.

**Fix:** make these buffers `static`. All three paths are non-reentrant in
this cooperative single-threaded design — `wifi_lwip_drain_rx_frames()` is only
ever entered from `wifi_lwip_poll()`, and `wifi_lwip_link_output()` /
`sdio_runtime_send_ethernet_frame()` handle one frame at a time. A `static`
RX buffer and a separate `static` TX buffer (they must stay separate, because
TX is invoked from inside the RX call chain) removes ~4.8 KB from the worst-case
stack depth at no behavioural cost.

---

## Medium

### M1 — `sdio_probe_ack_interrupts()` blocks the poll loop for up to ~300 ms

**Location:** `sdio.c` — `sdio_probe_ack_interrupts()` (the `for (hmb_round …
< 30)` loop, ~line 3946) with `usleep(10000u)` per iteration; reached from the
runtime boot stage `SDIO_RUNTIME_STAGE_ACK_INTERRUPTS` (`sdio.c:4567-4568`,
inside `sdio_runtime_tick()`).

**Issue:** `sdio_runtime_tick()` is a registered poll hook. The
`ACK_INTERRUPTS` stage calls `sdio_probe_ack_interrupts()`, which can spin for
up to 30 × 10 ms = **300 ms of blocking `usleep`** in a single tick.
`sdio_probe_enable_functions()` similarly does up to ~100 ms (`usleep(1000)`
×100).

**Impact:** the cooperative poll loop also services the BBC 1 MHz bus, so a
300 ms tick starves that bus. This is the exact problem the CLM download and
the join sequence were deliberately refactored away from (they were converted
to deadline-based per-tick steps) — `sdio_probe_ack_interrupts()` is the one
boot stage that was left synchronous, so it is an inconsistency rather than a
design choice.

**Fix:** convert the HMB-round loop into a deadline-based per-tick sub-state,
the same pattern as `sdio_runtime_clm_download_step()` — record a deadline,
return, and re-enter on the next tick. The diagnostic-only blocking variants
(`sdio_card_identify()`, `sdio_probe_request_alp_clock()`, etc.) are *not* on
the runtime path and can be left alone.

### M2 — Framebuffer BMP is built entirely inside one lwIP callback

**Location:** `webserver.c` — `route_framebuffer_bmp()`.

**Issue:** the whole BMP — `malloc`, a `memset` of the entire image, and a
per-pixel conversion loop over up to ~2 million pixels (capped at
`WS_FB_BMP_MAX` = 6 MB) — is produced **synchronously inside the lwIP `recv`
callback**, which runs inside `wifi_lwip_poll()`.

**Impact:** a long single-shot CPU burst that stalls the cooperative poll loop.
For the default 640×512 mode it is a few milliseconds (tolerable); for a large
32-bpp custom mode it is tens of milliseconds in one tick — a visible bus
stall. It is request-triggered, not continuous, so impact is bounded, but it
is the same class of issue as M1.

**Fix:** the cleanest option is to stream the BMP the way file downloads are
streamed (build the header, then convert and send a few rows per `conn_pump`
call) instead of materialising the whole image up front. A simpler partial
mitigation is to cap the exportable dimensions lower.

---

## Low / Improvement

### L1 — Dead code after `return` in `sdio_runtime_boot_firmware()`

`sdio.c:1561-1562`:

```c
   return 0;
   return sdio_runtime_finalize_boot_stage(dev, 0u, &chip, now_us);
```

Line 1562 is unreachable. It is harmless at runtime (the `return 0` correctly
defers to a later tick) but it is a leftover and signals a half-finished edit.
Remove line 1562.

### L2 — `route_framebuffer_bmp()` trusts the framebuffer geometry unchecked

`webserver.c` — `route_framebuffer_bmp()` reads pixels at
`src = fb + y*pitch`, `src[x*bpp]`, trusting that `pitch`, `width`, `height`
and `size` from `framebuffer_export_get_info()` are mutually consistent. If
`pitch < width*bpp` (or `size < pitch*height`) the last row reads past the end
of the framebuffer region. The values come from a mailbox query and are
normally consistent, so this is defensive hardening rather than a known bug.
Add a check that `pitch >= width * (bpp/8)` and `size >= pitch * height`
before the loop, and bail with an error if not.

### L3 — `netname.c` mDNS buffer size is unrelated to the hostname cap

`netname.c` — `netname_send_mdns()` builds into `uint8_t msg[96]`. The worst
case is `34 + g_host_len` bytes, and `g_host_len` is capped at
`NETNAME_HOST_MAX` (32), so the maximum is 66 — **safe today**. But `96` and
`NETNAME_HOST_MAX` are independent constants; raising `NETNAME_HOST_MAX` past
~60 would silently overflow the stack buffer. Size the buffer as
`[34 + NETNAME_HOST_MAX]`, or add a `static_assert`.

### L4 — `cyw43.c` image-load error paths assume `filesystemReadFile` never leaks

`cyw43.c:71-96` — when a `filesystemReadFile()` call returns length 0 the code
treats it as "nothing was allocated": the firmware path `return false`s
without releasing, and the CLM path does `g_cyw43_clm_data = NULL` (line 94)
which would discard the pointer. If `filesystemReadFile()` ever returns 0 with
a buffer still allocated (e.g. an SD read that fails *after* the `malloc`),
that buffer leaks. This is a boot-time, rare-error leak of one buffer, but it
is worth hardening: `free()` before `= NULL` (free(NULL) is safe), and release
on the firmware-fail path too — or fix `filesystemReadFile()` (in
`BeebSCSI/filesystem.c`, outside this subsystem) to free on its own failure
paths.

### L5 — Blocking SDIO-host wrappers and a very long `TIMEOUT_WAIT` remain

`sdio_host.c` — the runtime correctly uses the non-blocking
`sdio_host_open_start()` / `sdio_host_open_poll()` state machine. However the
blocking wrappers `sdio_host_open()` / `sdio_host_apply_clock_rate()` still
exist (the former is reached only via the optional `sdio_probe` diagnostic),
and `sdio_host_apply_clock_rate()` uses `TIMEOUT_WAIT` with `0x1000000`
iterations of `RPI_WaitMicroSeconds(1)` — a ~16-second freeze if a hardware
bit never sets. Tighten that bound (e.g. 100 ms), and consider removing the
now-unused blocking wrappers so they cannot be reintroduced into a poll path.

### L6 — Latent buffer-overflow margins with no guard

Two spots in `sdio.c` are correct today only by exact arithmetic, with zero
margin and no assertion:

* `tx_control_template_payload_bytes[80]` (`sdio.h:209`) — every payload
  builder fits in 80 bytes, but `strlen(name)` of the iovar name is never
  bounds-checked before the `memcpy`. Safe only because all callers pass short
  string literals.
* `sdio_cyw43_condense_nvram()` — the `malloc(length + 4u)` slack is *exactly*
  enough for the worst-case trailing-NUL + alignment padding.

Both would benefit from an explicit bounds check (or a slightly larger margin
plus a comment), so a future change cannot silently overflow them.

### L7 — Webserver upload parses only the first multipart part

`webserver.c` — `upload_begin_part()` requires the first `multipart/form-data`
part to be the file. A browser `<input type="file">` sends exactly one part,
so this is fine in practice, but any client that sends another form field
first gets "No file was selected". Minor robustness gap — skip non-file parts
instead of failing.

### L8 — URL-decoded paths are silently truncated

`webserver.c` — `ws_url_decode()` / `ws_normalize_path()` truncate a decoded
path to `WS_PATH_MAX`. There is no traversal risk (`ws_path_is_safe()` still
rejects `..` afterwards); a truncated path simply fails to open and returns
404. Returning 400 ("path too long") would be cleaner than a misleading 404.

### L9 — A failed first WiFi boot can never be retried

`wifi.c` — `g_wifi_init_done` latches `true` on the first `wifi_init()` call,
so `init_emulator()` (re-run on every BBC RST) never re-attempts WiFi. That is
the intended behaviour for a *working* connection (a reset must not tear it
down). But it also means a transient first-boot failure — SDIO not ready,
firmware preload failed, join timed out — can never be retried, even though a
reset is the natural recovery action. Consider latching `g_wifi_init_done`
only once boot reaches `WIFI_BOOT_STAGE_COMPLETE` without `WIFI_STATE_ERROR`,
so a reset after a failure does re-attempt.

### L10 — lwIP pool sizes are adequate but tight

`lwipopts.h` — `MEMP_NUM_UDP_PCB` is 6 (DHCP + DNS + NetBIOS + mDNS = 4 in
use) and `MEMP_NUM_PBUF` is 24. Both work but leave little headroom; raising
them to 8 and 32 costs negligible RAM. The core TCP tuning (`TCP_MSS` 1460,
`TCP_WND`/`TCP_SND_BUF` = 8×MSS, `MEM_SIZE` 32 KB, `MEMP_NUM_TCP_SEG` 40) is
internally consistent and sound.

### L11 — Minor correctness/clarity items

* `sdio.c` — the RX `hwtag` is read as `uint16_t[2]` after a byte-wise CMD53
  transfer. It works on the little-endian ARM target but is endian-dependent;
  decoding the bytes explicitly (the file already has `sdio_load_u32_le`-style
  helpers) would be clearer and portable.
* `wifi.c` — `wifi_validate_config()` has a static-IP check that can never
  fire given how `wifi_parse_ip_mode()` orders its work; remove or comment it.
* `wifi.c` — `wifi_debug_printf("WIFI: debug enabled…")` hard-codes the
  `WIFI:` prefix that `wifi_debug_log()` adds elsewhere — cosmetic
  inconsistency.
* `wifi_lwip.c` — the RX drain loop `return`s out of the whole budget on a
  single transient `pbuf_alloc`/`input` failure; a `break` (stop this cycle,
  resume next poll) expresses the intent better. pbuf alloc/free balance
  itself is correct.

### L12 — Idle poll hooks stay registered

`wifi.c` — `wifi_boot()` remains a registered poll hook after reaching
`WIFI_BOOT_STAGE_COMPLETE` and is called (to hit a no-op `default`/`COMPLETE`
case) every poll iteration for the life of the device. The cost is negligible
(one call + switch), but if `Pi1MHz_Register_Poll()` ever gains a deregister,
this and the post-boot `wifi_lwip`/`webserver` hooks could drop out.

---

## Investigated and ruled out

Two issues surfaced during review that looked serious and were checked in
detail against the source; **neither is a real bug**:

* **NetBIOS response format (`netname.c`).** The positive name-query response
  was suspected of omitting the answer record's name field. It does not: with
  `QDCOUNT = 0` and `ANCOUNT = 1`, the 34-byte encoded name copied to
  `resp[12..45]` *is* the answer record's `RR_NAME`, followed by
  TYPE/CLASS/TTL/RDLENGTH/flags/address. The 62-byte layout matches the lwIP
  reference `netbiosns.c` exactly.

* **Use-after-free in `ws_recv()` (`webserver.c`).** It was suspected that
  `conn_pump()` could `free()` the connection object mid-callback while the
  pbuf-segment loop kept using it. It cannot: `conn_pump()` only closes a
  connection once `bytes_acked >= bytes_queued`, and no ACK can have arrived
  during the request's own `recv` callback (`bytes_acked` is 0), so the first
  pump never closes. The only synchronous close path is `ws_oom()` on an
  allocation failure, and that propagates `false` back through `conn_consume()`
  so the loop stops without touching the freed object. `conn_consume()`
  returning `true` therefore guarantees the connection is still alive.

## Also verified clean

* The SDIO receive path (`sdio_runtime_complete_read_ethernet_frame_timeout`,
  `sdio_runtime_note_event`, `sdio_probe_read_post_header_prefix`) validates
  every chip-supplied length and offset against the buffer size before
  indexing — no RX over-read/over-write found. `total_length` is capped to the
  buffer size up front.
* Webserver FatFS handles: `f_open` is matched by `f_close` on every path,
  including error paths, in `conn_close`, `ws_err`, `upload_fail` and
  `upload_finish`.
* lwIP pbuf ownership in `webserver.c` and `netname.c` — every received pbuf
  is freed exactly once; every allocated pbuf is freed after `udp_sendto`.
* `cyw43.c` image lifetime — `cyw43_release_boot_images()` vs
  `cyw43_release_images()` is correct and idempotent; no double-free.
* The NetBIOS name decoder bounds its `'A'..'P'` decode and length-trim
  correctly, guarded by `NBNS_REQUEST_MIN`.

---

## Suggested order of work

1. **H1** — make the RX/TX frame buffers `static` (small change, removes a
   real stack-overflow risk).
2. **M1** — convert `sdio_probe_ack_interrupts()` to per-tick steps (removes
   the last ~300 ms boot-time poll-loop stall).
3. **M2** — stream the framebuffer BMP instead of building it in one shot.
4. **L1** — delete the dead line.
5. The remaining Low items as convenient — L4, L5, L6 and L9 are the most
   worthwhile; the rest are polish.
