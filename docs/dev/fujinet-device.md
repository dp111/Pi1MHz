# FujiNet device on the Pi - design

STATUS 2026-09-26 (evening): phase 1 (disk) BUILT and working on a Master
with a Pi Zero 2 W. Pi side on branch fujinet-device (376c79d devices +
host tests, 926a670 service). ROM side: fn-rom branch pi1mhz-1mhz-link
(6bf7040, claude-tmp/fn-rom), BUILD_INTERFACE=1MHZ, for upstream; its
serial build is byte-identical to before.

MEASURED on hardware, fn-rom 0.02 Master build in sideways RAM:
*FHOST set/get (resolved and stored by the Pi, on the card), *FIN,
*FMOUNT, *FDRIVE, *CAT, *TYPE, *LOAD, *SAVE (new file allocated after the
last, byte-exact on the card, other files intact); WebDAV PUT/DELETE of a
mounted image refused 423; plain BREAK keeps mounts, CTRL-BREAK starts a
new host session (unmounts) as fujinet-nio specifies. TNFS images (2b05832,
on the shared net_tnfs.c engine from 1f6ec9d) also MEASURED on the Master
against FujiNet's own tnfsd: *FHOST tnfs://host/dir, *FIN, *FMOUNT,
*CAT, *TYPE, *LOAD, *SAVE (byte-exact on the server), BREAK keeps the
mount. A request waiting on the server holds the Beeb's busy bit and is
re-run each main-loop pass (see fujinet/fn_store.h). NOT hardware-tested:
*FLS (a utilities-disc command, not in the ROM), phase 2 (network).

Phase 2 so far (host-tested only): net_service's URL verbs split into cores
(4859208) with a C API and HTTP methods/body on top (06a75a4), and the
network device fujinet/fn_network.c - open/read/write/close for http:// and
tcp://. Still to come: JSON translation, https://.

Bench trap: a TNFS server with two interfaces on one subnet answers from
its primary address; address it by that one, or every client here (the N:
device too) rejects the replies and the server logs a new session per
resend. The exchange ABI
below is as built; fn-rom's author has not reviewed it yet.

## What this is

[fn-rom](https://github.com/markjfisher/fn-rom) (Mark Fisher, GPLv3) is a
BBC/Master filing-system ROM, derived from MMFS, that talks to a FujiNet
device. Today that device is an ESP32 (or the desktop build of
[fujinet-nio](https://github.com/markjfisher/fujinet-nio), also GPLv3) on
the RS423 port, and the two exchange "FujiBus" packets framed with SLIP.

Here Pi1MHz becomes the FujiNet device, on the 1MHz bus: fn-rom's empty
`BUILD_INTERFACE=1MHZ` slot gets a backend that hands whole packets to the
Pi through the services port, and a new Pi service answers them. No ESP32,
no serial cable, no PC.

fujinet-nio is the reference for what every reply must contain. Its code is
C++ with its own framework, so the Pi side is a C reimplementation of the
handlers fn-rom actually uses, not a port.

## The packet (unchanged from serial)

Six-byte header, little-endian:

    [0] device   [1] command   [2..3] length (whole packet)   [4] checksum   [5] descriptor

fn-rom's requests always carry descriptor 0 and a fixed-layout payload from
byte 6. Every reply carries descriptor 1, byte 6 = status, payload from byte
7. Status is fujinet-nio's `StatusCode` (0 Ok, 1 DeviceNotFound,
2 InvalidRequest, 3 DeviceBusy, 4 NotReady, 5 IOError, 6 Timeout,
7 InternalError, 8 Unsupported). The checksum is the running fold
`c = ((c + b) >> 8) + ((c + b) & 0xFF)` over the packet with byte 4 taken as
zero. One request is outstanding at a time; there is no sequence number.

On the bus SLIP is not needed - the exchange carries lengths - but the
header and checksum stay exactly as they are, so nothing in fn-rom above the
link layer changes and the Pi can check every packet as a real device does.

## The exchange (services port)

fn-rom's frame layer, `src/kernel/fuji_link_slip.s`, exports five entry
points and nothing else reaches the link:
`fuji_link_write_slip_frame`, `_dual`, `_triple`, `fuji_link_read_slip_frame`
and `_to_payload`. The 1MHz backend replaces that file (not the byte-level
`fuji_link_*` below it) with one that keeps those five names:

- a write copies the packet's region(s) into the buffer through the
  auto-incrementing port &FCA9, then rings the exchange;
- a read copies the reply the Pi left in the buffer out to the caller's
  destination(s), keeping the `_to_payload` split (first seven bytes to
  `buffer_ptr`, the rest to the payload pointer, bounded by its capacity).

Command **114, FUJIBUS_EXCHANGE**, claimed as the range 114-119 in
`src/services.h` (the rest kept for this service). Command block on page
&F0 (&FFF000), the active filing system's page - fn-rom is the active
filing system whenever it calls, so it never meets MMFS's use of the page.

    block[0]      114
    block[1..3]   request offset in the buffer (24-bit, low first)
    block[4..5]   request length
    block[6..8]   reply offset
    block[9..10]  reply capacity
    -> block[11..12] reply length, written by the Pi

Proposed buffer placement: request at offset &000000, reply at &000800,
2 KB each - inside the first 4 MB, which docs/advanced.md gives the active
filing system. The ROM names them in the block, so they can move.

The ROM writes &F0 to &FCAA and polls it: bit 7 set = still working (the
port echoes the write), clear = done, and the value is the exchange result:
0 = a reply is in the buffer; nonzero = no reply (bad block, bad offsets,
malformed packet). A packet the device understands but refuses is still
result 0, with the refusal in the reply's status byte - exactly what serial
would have delivered.

## The Pi service

`src/fujibus_service.c`, shaped like the FAT and net services:

- the FRED handler (FIQ) validates the block's offsets with
  `service_buffer_ok`, latches the request and returns - nothing else;
- the main-loop poll decodes the packet, verifies length and checksum,
  dispatches on the device byte, builds the reply (checksum included) in the
  reply area and writes the result byte, which clears busy. SD card work is
  only ever here, never in FIQ.

The device handlers are plain functions `(request bytes) -> (reply bytes)`
with no Pi dependencies below a small storage interface, so they build on
the host and are tested there against packets captured from fn-rom.

## Devices, by phase

Phase 1 - disk (everything fn-rom needs to mount and run software):

| Device | Commands | State |
|---|---|---|
| Disk `$FC` | mount, unmount, read/write sector, create, reinitialise, list mounts, begin host session, restore boot | 8 slots, each an open image |
| Slot catalog `$F2` | get, put, delete, range | up to 256 named entries (URIs) |
| App store `$F1` | read, write, delete | key/value; fn-rom keeps its drive-to-slot map here |
| Host `$F0` | get/set current, list/select/delete history | current host + history (fn-rom selects 0-31) |
| File `$FE` | list directory | none |

Images come from the SD card (FatFs, main loop) or a TNFS server (the
existing TNFS client). The persistent state - slot catalog, app store, host
history - lives in one small file on the card, written on change.

Phase 2 - network `$FD`: open, read, write, close, JSON translate, mapped
onto the existing net service; five channels, and "not ready" (status 4)
where fn-rom already retries. `*FJSON` needs a JSON query engine - to be
checked.

As built (fujinet/fn_network.c on net_capi_*, net_service.h): five sessions,
one net_service handle each (handles 8-12, never the Beeb's own), handle =
generation << 8 | slot so a stale handle is refused and none is ever 0 (0
is fn-rom's "open failed"). Open replies at once and starts resolving; the
main-loop poll carries the connection on, and Read answers NotReady until
it is up (fn-rom backs off for ~48 s in all). Write is the one divergence
from nio's model: fn-rom takes anything but Ok from Write as failure, so a
Write the stack cannot take yet keeps the request pending and is re-run,
safe because offsets are sequential - the session's cursor says how much of
the chunk already went. Thirty seconds without progress answers Timeout. A
repeated chunk (at or behind the cursor) is answered without being resent.
Refused for now: https:// and JSON translation (Unsupported), Info and
InfoRead, request headers other than Content-Type, bodies of unknown length
(the request is sent with its Content-Length before the body). A BBC reset
closes every session, as net_service drops its connections on reset.

Not sent by fn-rom today, so not implemented: Wi-Fi `$F3`, clock, modem,
the FujiNet control device `$70`, and the defined-but-unused commands (disk
info/clear-changed, network info, file stat/read/write, app store stat/list).
A request for one gets status 8, Unsupported.

## Testing

- Host: unit tests replay request packets (from fn-rom's own test vectors and
  captures) against the handlers and compare replies with fujinet-nio's.
- Beeb: fn-rom built with `BUILD_INTERFACE=1MHZ`; `*FMOUNT` an SSD from the
  SD card and from a TNFS server, `*CAT`, `*RUN`, a sector write read back;
  the negative control is the serial build against the same image.
