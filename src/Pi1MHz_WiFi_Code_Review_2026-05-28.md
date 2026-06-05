# Pi1MHz WiFi Subsystem — Code Review (follow-up)

**Date:** 2026-05-28
**Scope:** every C/header file under `src/wifi/` except the third-party
lwIP tree. Files: `webserver.c/.h`, `netname.c/.h`, `wifi.c/.h`,
`wifi_lwip.c/.h`, `sdio.c/.h`, `sdio_host.c/.h`, `cyw43.c/.h`, `md5.c/.h`
(new), `framebuffer_export.c/.h`, `lwipopts.h`.
**Method:** focused re-read of the changes since the 2026-05-23 review
(MAC handling overhaul, WebDAV + digest auth, dispatcher refactor,
cleanup), plus a sanity sweep of the unchanged areas. Findings cross-
checked against the source. Line numbers as of this review.

## Summary

The pre-existing High and two Medium issues from 2026-05-23 (H1, M1, M2)
have all been **applied and verified working in production**.

The new WebDAV implementation and the SET_MAC runtime IOCTL path are
the bulk of new code surface. A handful of fresh findings — one of them
the same class as M1/M2 — and several open carry-overs from last time.

| Severity | New | Carry-over | Total |
|----------|-----|------------|-------|
| High     | 1   | 0          | 1     |
| Medium   | 4   | 0          | 4     |
| Low / Improvement | 9 | 12     | 21    |

Two of the High/Medium findings from last time have follow-ups marked
inline; the rest were fully closed.

---

## High

### W1 — WebDAV COPY runs the whole file copy synchronously inside a TCP recv callback

**Location:** `webserver.c:2614-2649` — `route_dav_move_or_copy()` when
called with `is_move=false`.

**Issue:** the COPY path opens both source and destination files, then
runs a tight `do { f_read; f_write } while` loop with no per-tick yield.
The entire transfer happens inside a single `ws_recv` callback chain
(`ws_recv → conn_consume → process_request → route_dav_move_or_copy`).
For a 10 MB file on a slow SD card this is potentially seconds of
blocking — the cooperative poll loop is stalled, the 1 MHz BBC bus is
starved, and the lwIP timer never runs (so TCP retransmits and DHCP
renewals back up).

This is exactly the same class of issue as **M1**
(`sdio_probe_ack_interrupts`) and **M2** (framebuffer BMP build) from
the 2026-05-23 review, both already fixed. Drag-and-drop within a
Windows Explorer WebDAV mount issues a COPY request, so a user
exercising the WebDAV mount in the normal way will hit this.

**Fix:** turn COPY into a per-tick state machine like
`CONN_RECV_DAV_PUT`: add `CONN_COPY_RUNNING`, stash the src/dst FILs
and progress on `ws_conn_t`, push one 8 KB chunk per `conn_pump` call,
queue the 201/204 response when src EOF is reached. The
`f_read`/`f_write` cost per chunk is bounded (<10 ms on a typical
card) so the bus glitch becomes invisible.

---

## Medium

### W2 — Content-Length parser in `route_dav_put` has unchecked integer overflow

**Location:** `webserver.c:2437-2442`.

```c
for (p = len_hdr; *p != '\0'; ++p) {
   if (*p < '0' || *p > '9')
      return ws_error(c, 400, "Bad Request", "Malformed Content-Length.");
   content_length = (content_length * 10u) + (uint32_t)(*p - '0');
}
```

A client can send `Content-Length: 999999999999999999`. The multiply
wraps modulo 2^32, leaving an arbitrary small value in
`content_length`. The streaming model then closes the file after that
many bytes and replies 201 — and either the file is short, or further
PUT body bytes are silently dropped by `conn_consume`. Either way a
client can confuse the server about the upload size.

**Fix:** check for overflow on each multiply (`if (content_length >
UINT32_MAX / 10u) return 413 Payload Too Large;`), or accumulate into
`uint64_t` and reject `> WS_PUT_MAX_BYTES`. Adding `WS_PUT_MAX_BYTES`
(e.g. 256 MB or the SD free space) is the most useful guard.

### W3 — `sdio_runtime_set_mac_step` ignores IOCTL send + response failures

**Location:** `sdio.c:3351-3367`.

```c
(void)sdio_probe_send_single_tx_control_template_timeout(dev,
                                                        &g_sdio_probe_result,
                                                        SDIO_RUNTIME_POLL_TIMEOUT_US);
g_runtime_step_sent = true;
g_runtime_step_deadline_us = now + 10000u;
return 0;
...
(void)sdio_drain_fn2_responses(dev);
g_runtime_step_sent = false;
sdio_debug_log("SET_MAC: cur_etheraddr <- ...");   /* logs the requested MAC, NOT the
                                                      MAC the chip accepted */
```

The SET IOCTL send is `(void)`-cast and the response drain is
discarded. If the chip rejects the SET (e.g. firmware refuses
`cur_etheraddr` SET while interface is up — some brcmfmac builds do)
the SET_MAC stage thinks it succeeded and the chip silently keeps its
factory OTP MAC. The QUERY_MAC stage then reads back the OTP MAC, so
the lwIP netif and chip agree (both OTP) and DHCP works — the
**desired** SoC MAC is just not used.

The debug log line further misleads: it logs the requested MAC, so
the user thinks the SET worked even when it didn't.

**Fix:** capture the CDC response status the same way the existing
GET_VAR machinery does (request-id-matched), and log a clear warning
on non-zero status. Optionally fail the boot (or fall through to
QUERY_MAC and let the netif end up with the OTP MAC, but log the
inconsistency).

### W4 — `route_dav_propfind` silently emits a partial listing on heap exhaustion

**Location:** `webserver.c:2300-2316` (the children loop).

```c
ws_strbuf_t url;
sb_init(&url);
if (!is_root) sb_urlpath(&url, sdpath);
sb_putc(&url, '/');
sb_urlpath(&url, chld.fname);
if (child_is_dir) sb_putc(&url, '/');
if (url.data != NULL) {
   dav_emit_response(&b, url.data, child_is_dir,
                     (uint32_t)chld.fsize, chld.fname);
}
sb_free(&url);
```

If any `sb_putc`/`sb_urlpath` realloc fails mid-iteration, `url.data`
ends up `NULL` and the entry is silently skipped. The outer
multistatus continues — the client sees a truncated but well-formed
PROPFIND response and believes that is the complete directory. Files
that exist on the SD card simply vanish from view in Explorer.

**Fix:** propagate the OOM. Bail with `return ws_oom(c);` after
`sb_free(&b)` (and any other live `ws_strbuf_t`) instead of skipping.

### W5 — HEAD requests send full response body (RFC 7231 violation)

**Location:** `webserver.c:2829-2848` — HEAD is dispatched to the same
GET handlers, which queue the body via `start_download`,
`route_files_get`, etc.

**Issue:** RFC 7231 requires HEAD to produce identical headers to GET
but **no message body**. We send the full body. Worst case is HEAD
on a large file (Windows Explorer does this before deciding whether
to download): the full file is transferred twice — once for HEAD,
once for the follow-up GET.

The comment at `webserver.c:2771-2772` claims "HEAD is treated as GET
for hash purposes per RFC" — but `ws_digest_verify` uses the literal
method string ("HEAD" or "GET") in HA2, which is what the RFC
actually requires. The comment is misleading; the code is correct for
the hash but wrong for the body suppression.

**Fix:** stash `c->is_head` from the request line; in `conn_pump`,
when `c->is_head` is set, drain the body sources but skip
`tcp_write`. The header is still sent.

---

## Low / Improvement (new)

### W6 — `route_dav_delete` rejects non-empty collections instead of recursing

**Location:** `webserver.c:2487-2497`.

RFC 4918 §9.6.1 says `DELETE` on a collection MUST behave as
`DELETE` with `Depth: infinity`. Our implementation refuses with 409
("Directory is not empty") and a comment that says "the client must
do depth-first". Windows Explorer's redirector handles this
gracefully by deleting children first, but strict clients fail. A
small recursive `f_unlink` walk (bounded to a reasonable depth, e.g.
8) would be RFC-compliant.

### W7 — `ws_hex_eq_ci` is not actually constant-time

**Location:** `webserver.c:878-890`. The function's comment talks
about a constant-time compare ("compute the diff over all bytes
even after a mismatch"), but the early `return false;` on `received_hex[i]
== '\0'` makes it data-dependent in length. For local-LAN WebDAV
this is irrelevant (digest auth over plain HTTP is already not
secret), but the comment is misleading.

### W8 — Unused local `char ctype_unused[32]` in `route_dav_put`

**Location:** `webserver.c:2420, 2443`. Declared, written nowhere,
cast to void to suppress the warning. Just remove it; the code was
presumably going to peek at Content-Type and never did.

### W9 — `ws_url_decode` does not reject control bytes; `ws_path_is_safe` does

**Location:** `webserver.c:341-357`, `:595-616`.

The decode happily produces 0x00 from `%00`, 0x0A from `%0A`, etc.
The subsequent `ws_path_is_safe` check correctly rejects any byte
< 0x20, so this is not exploitable today — but coupling the two is
brittle. A future caller that uses the decoded buffer **before**
`ws_path_is_safe` would have a NUL-injection problem. Either reject
in the decoder, or add an assert/comment at every call site.

### W10 — `ws_conn_t` is ~13 KB; eight connections is ~104 KB heap

**Location:** `webserver.c:82-139` (the struct).

Each connection holds `dl_file` + `up_file` + `dav_file` (three FIL
structs, ~560 bytes each on FF_MAX_SS=512 builds), `dl_buf[8192]`,
`reqhdr[2049]`, `up_head[1025]`, `up_dir[512]`,
`dav_put_target[512]`, etc. With `MEMP_NUM_TCP_PCB = 8`, the
worst-case parallel allocation is about 100 KB. The Pi has plenty of
RAM so this is currently fine, but it is the highest single contributor
to the webserver's memory footprint. Two of the three FILs (`up_file`
+ `dav_file`) are mutually exclusive — only one upload path is active
at a time. Union them via a `union { FIL up; FIL dav; } write_file;` to
shave ~600 bytes per connection.

### W11 — Digest auth nonce secret is a weak `now ^ 0xA5A5A5A5`

**Location:** `webserver.c:689-691` (`ws_digest_refresh_nonce`).

`g_ws_nonce_secret = now ^ 0xA5A5A5A5` where `now` is the system
timer at the moment of the first challenge. The system timer at that
point is the boot time (a few hundred ms), so the secret is
effectively `0xA5A5A5A5 ^ <small int>` and predictable. An attacker
on the LAN who captures one nonce can guess the secret and replay
future nonces. Replay would still need the password to compute a
valid HA1, so the practical impact is low, but mix in a higher-
entropy source (e.g. the chip's MAC bytes XOR'd in) to harden.

### W12 — Predictable `opaquelocktoken` in `route_dav_lock`

**Location:** `webserver.c:2686-2689`.

```c
nonce = RPI_GetSystemTime();
snprintf(token, sizeof token,
         "opaquelocktoken:%08lx-%08lx", (unsigned long)nonce,
         (unsigned long)(nonce ^ 0xA5A5A5A5u));
```

The lock token is `(timer, timer ^ constant)`. The comment
acknowledges the LOCK handler is a stub with no real state, but a
real WebDAV client that uses the token across requests (e.g. to
demonstrate the lock is theirs in an `If: (<token>)` header) gets a
trivially-guessable identifier. Same `g_ws_nonce_secret` mixin as W11
would suffice if we ever do anything with the token.

### W13 — `GET` on a directory returns 404 instead of 405

**Location:** `webserver.c:2755-2765` (`route_dav_get_file` falls
through to 404 when the path is a directory).

WebDAV semantics expect 405 ("Method Not Allowed") for GET on a
collection — clients can then choose to PROPFIND. Returning 404
("Not Found") is misleading because the path *does* exist. Low
impact; affects only strict WebDAV clients (not Windows Explorer).

### W14 — `dav_format_date` always returns 2024-01-01

**Location:** `webserver.c:2157-2164`. Documented as deliberate (no
RTC), but it means If-Modified-Since / If-Unmodified-Since headers
either always match or always fail to match — and Last-Modified
sorting in WebDAV clients shows every file as the same time. If you
ever wire the BCD clock from the SD-card filesystem mailbox or
expose a configurable boot-time epoch, this could be made useful.

---

## Carry-over from 2026-05-23 (still open)

The previous review's H1, M1, M2 are closed. The Low items below were
flagged then and are still present:

| # | What | File:line (now) | Notes |
|---|------|-----------------|-------|
| L1 | Dead `return sdio_runtime_finalize_boot_stage(...)` after `return 0` | `sdio.c:1577-1578` | Unchanged. Remove line 1578. |
| L2 | `route_framebuffer_bmp` trusts framebuffer geometry unchecked | `webserver.c` | M2 was applied (streaming) but the `pitch >= width*bpp` guard wasn't added. |
| L3 | `netname.c` mDNS buffer size independent of `NETNAME_HOST_MAX` | `netname.c:84` | Unchanged. Size it `[34 + NETNAME_HOST_MAX]`. |
| L4 | `cyw43.c` image-load error paths assume `filesystemReadFile` doesn't leak | `cyw43.c:64-96` | Unchanged. Defensive `free(g_cyw43_X_data)` before each `return false`. |
| L5 | Blocking SDIO-host wrappers + 16 s `TIMEOUT_WAIT` | `sdio_host.c` | Unchanged. Tighten `TIMEOUT_WAIT`. |
| L6 | Latent buffer-overflow margins (tx_control_template_payload_bytes[80], cyw43_condense_nvram) | `sdio.h:209`, `sdio.c` | Unchanged. Add explicit length checks. |
| L7 | Webserver upload parses only the first multipart part | `webserver.c` | Unchanged. |
| L8 | URL-decoded paths silently truncated to `WS_PATH_MAX` | `webserver.c` | Unchanged. Return 400 rather than 404. |
| L9 | `g_wifi_init_done` latches `true` on the first call so a transient first-boot failure can never be retried | `wifi.c` | Unchanged. Move the latch into the COMPLETE-without-ERROR path. |
| L10 | lwIP pool sizes adequate but tight (`MEMP_NUM_UDP_PCB`=6, `MEMP_NUM_PBUF`=24) | `lwipopts.h` | Unchanged. With WebDAV added, the TCP path is more loaded; bump to 8 / 32. |
| L11 | Minor correctness items (endian-dependent hwtag, dead static-IP check, debug prefix inconsistency, RX-drain `return` vs `break`) | various | Unchanged. |
| L12 | Idle poll hooks stay registered after boot completes | `wifi.c` | Unchanged. Now there is exactly one registered hook (`wifi_dispatch_poll`) which calls four sub-polls; each has its own early-exit guard, so cost is bounded. |

---

## Also verified clean

* **Digest auth math** — MD5, HA1, HA2 and response computation
  match RFC 7616 byte-for-byte (cross-checked against curl-sent
  values with the user's actual `webdav_user=Pi1MHz
  webdav_password=Pi1MHz` configuration).
* **The recent `Digest` prefix fix** (`webserver.c:924-936`,
  `ws_digest_verify`) — the parser now correctly handles
  `Authorization: Digest username="..."` where there's no comma
  between scheme and first field.
* **MAC alignment end-to-end** — `wifi_preload_images` →
  `sdio_runtime_set_desired_mac` → per-tick `SET_MAC` stage →
  per-tick `QUERY_MAC` stage → `sdio_runtime_get_chip_mac` →
  `wifi_lwip_netif_init`. The chip and the lwIP netif always
  advertise the same address, and the fallback in
  `wifi_lwip_netif_init` is now a hard `wifi_set_error` rather than
  a silent locally-administered MAC.
* **The H1/M1/M2 fixes** verified in place:
  H1 — three RX/TX frame buffers are `static` and 4-byte aligned;
  M1 — `sdio_runtime_ack_interrupts_step` is per-tick with the
  function-static deadline pattern;
  M2 — `CONN_SEND_FB` streams a bounded number of BMP rows per
  `conn_pump` call.
* **`md5.c`** — standard RFC 1321 implementation; matches all
  the well-known test vectors (`""`, `"a"`, `"abc"`,
  `"message digest"`) and the curl-sent digest exactly.
* **lwIP callback hygiene** — `ws_recv`, `ws_sent`, `ws_poll`,
  `ws_err`: every path that calls `tcp_abort` returns `ERR_ABRT`,
  pbufs are freed exactly once, the connection-close decision
  (`bytes_acked >= bytes_queued`) is sound.
* **FatFS handle balance** — `conn_close`, `ws_err`,
  `dav_put_consume` and `route_dav_move_or_copy` all match `f_open`
  with `f_close` on every path including errors.

---

## Suggested order of work

1. **W1** — convert WebDAV COPY to per-tick streaming. Same pattern as
   the existing PUT/download. Biggest single quality-of-service issue
   the user will hit in normal WebDAV use.
2. **W3** — capture and log the SET_MAC IOCTL response. Single
   addition to `sdio_runtime_set_mac_step` plus a CDC-decoder
   request_id correlation block similar to the GET_MAC capture.
3. **W2** — clamp Content-Length parsing. ~5 lines of code.
4. **W4** — propagate OOM in PROPFIND child loop. ~3 lines.
5. **W5** — track `is_head` in `ws_conn_t` and gate body writes on
   it in `conn_pump`. ~10 lines.
6. **L1** — delete the dead line (one-character fix).
7. The remaining Low items as convenient. **L9** and **L4** are the
   most worthwhile of the carry-overs.
