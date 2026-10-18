STATUS (2026-10-18): BUILT and hardware-tested on branch smb-mmfs (Zero W,
Zero 2 W; Samba and Windows shares).  Beyond this plan, everything the Beeb
reads through the Pi goes to the share - the FAT service (MMFS/MMFS2 in file
mode), FujiNet and Music 5000 recordings too - and *FX147,203,6 chooses it.
Open: video only ~11-14 fps over WiFi (bigger and pipelined reads were
measured slower, so not stop-and-wait); FujiNet/M5000 on the share untested
on hardware.  User docs: docs/user/network-share.md.  The body below is the
original design (2026-10-15, from master 3711645 and libsmb2 4cce769);
claims are marked MEASURED, INFERRED or GUESSED.

# The Beeb's storage on a network share (`storage=smb`)

Goal: the Beeb's discs (BeebSCSI, BeebVFS + video, MMFS, the FAT transfer
directory) live in an ordinary folder on a server and are used exactly as
if they were on the card.  The server must be easy for Windows users.

## Why SMB, and why at the file level

- A block device (NBD/iSCSI) is the smaller firmware job - one more FatFs
  volume behind `diskio.c`, like `storage=usb` - but the server then holds a
  FAT32 *image*, not a folder: nobody can drop files into it while the Pi
  uses it (two writers on one FAT corrupt it), and Windows has no NBD
  server.  WSL does not fix this: WSL2 is behind NAT (the Pi cannot reach
  it without port forwarding or "mirrored" networking) and the image still
  cannot be edited while served.
- SMB is Windows file sharing: right-click a folder, Share, done.  Every
  NAS, Samba (Linux, macOS, Pi) serves it.  The user documentation is one
  page.  The cost moves into the firmware: an SMB client and a routing
  layer above FatFs.

## The client: libsmb2

Checked 2026-10-15 (`github.com/sahlberg/libsmb2`):

- Licence LGPL-2.1-or-later - combinable with GPLv3.  One file is not:
  `lib/md4c.c` is RSA's MD4 reference with its attribution licence; replace
  it with a public-domain MD4 (~200 lines).  SHA/HMAC are the RFC 4634
  code, MD5 and the reference AES are public domain.  `libdcerpc` (BSD-2)
  is not needed.
- Size MEASURED: ~130 KB linked (cortex-a53, `-O2`, gc-sections, the
  async calls listed below), of which `errors.c` is 25 KB of strings
  (trimmable) and `socket.c` 10 KB (replaced, see below).  The rpi kernel
  is ~820 KB today.
- Protocol: SMB 3.1.1 with pre-auth integrity (SHA-512), AES-CMAC signing,
  NTLMv2 logon, AES-128-CCM encryption (no GCM; Windows accepts CCM).
  Windows 11 24H2 requires signing and refuses guest logons by default, so
  signing and a user/password are mandatory, not options.
- Its async API (`smb2_*_async`, `smb2_which_events`, `smb2_service`) is a
  non-blocking event loop - the shape of our poll loop.

### Signing cost - use mbedTLS's AES

libsmb2's AES (`lib/aes_reference.c`, tiny-AES) re-expands the key for
every 16-byte block.  MEASURED (qemu-arm instruction count, CMAC over a
64 KB message, baseline subtracted):

| AES under the CMAC | ARM1176 | A53 (A32) |
|---|---|---|
| libsmb2 reference | 252 insn/byte | 320 insn/byte |
| mbedTLS 3.6.7, firmware config, key set once | 52 insn/byte | 54 insn/byte |

GUESSED (1 instruction ~ 1 cycle at 1 GHz, not yet checked against the
cycle counter on hardware): at the ~2.2 MB/s WiFi ceiling (MEASURED,
`wifi-throughput-2026-09.md`) signing costs ~55-70% of the CPU with
libsmb2's AES, ~12% with mbedTLS's, ~25% if a share also demands
encryption.  So `lib/aes.c`'s `AES128_ECB_encrypt` (it already switches to
an Apple implementation) gets an mbedTLS branch with a cached key
schedule.  First bench item: confirm the cycles on a Zero 2 W.

## Transport: a socket shim over raw lwIP

Our lwIP is `NO_SYS=1`, `LWIP_SOCKET=0`.  libsmb2 calls `socket`,
`connect`, `setsockopt`, `readv`, `writev`, `close`, `poll` and
`getaddrinfo` directly in `lib/socket.c` - there is no transport hook.
New `src/net_smb_socket.c` provides those names over `tcp_new`/
`tcp_connect`/`tcp_write`/`tcp_recved`:

- a table of two descriptors (one connection, plus one mid-reconnect);
- a receive queue of pbufs; `readv` drains it, returns `EAGAIN` when empty;
- `writev` copies into `tcp_write` (lwIP `TCP_SND_BUF` 32 MSS), `EAGAIN`
  when full;
- `poll` reports readiness without waiting (libsmb2 only asks it from the
  sync API, which we do not use);
- `getaddrinfo` through the existing DNS (net_dns.c), so `server=nas.lan`
  works where the router's DNS knows the name; a dotted IP always works.

libsmb2's own `lib/socket.c` is compiled with these names in scope; it is
vendored code, so any change to it is behind a named `#if`.

## The routing layer: `fsr_*`

`src/fsroute.c` / `fsroute.h`: the FatFs calls with FatFs's exact
signatures, types and `FRESULT` codes -

    fsr_open fsr_close fsr_read fsr_write fsr_lseek fsr_sync fsr_truncate
    fsr_expand fsr_stat fsr_unlink fsr_rename fsr_mkdir
    fsr_opendir fsr_readdir fsr_closedir fsr_getfree

Routing is by path: a path on drive `2:` goes to SMB, anything else
straight to FatFs (zero cost beyond one compare).  The path rule already
exists: `filesystemStoragePath()` prefixes `filesystemStorageRoot()`, which
is "" or "1:" today; `storage=smb` makes it "2:".  `/Pi1MHz` stays on the
card as for USB.  "2:" never reaches FatFs (`FF_VOLUMES` stays 2).

### Network files keep FatFs's types

An SMB file is still a `FIL`, a directory still a `DIR`:

- `fil->obj.fs` points at a dummy `FATFS` (`fsr_net_fs`) - the "this is
  SMB" mark; the SMB handle lives in a small side table keyed by the `FIL`.
  FatFs's own validation rejects the dummy, so a network `FIL` that ever
  reached real FatFs fails safe (`FR_INVALID_OBJECT`), never corrupts.
- The shim keeps `fil->fptr` and `fil->obj.objsize` current, so the
  `f_tell`/`f_size`/`f_eof` macros and the 28 places that read `FIL`
  fields directly work unchanged.
- Fast seek: `fsr_lseek(fp, CREATE_LINKMAP)` on a network file returns
  `FR_OK` and leaves `cltbl` alone - seeking costs nothing over SMB, and
  `filesystemAttachLinkMap()` (videoplayer) is satisfied.
- `fsr_readdir` fills `FILINFO` (LFN name, size, attributes, FAT date/time
  from the SMB timestamps).  libsmb2's opendir fetches the whole listing at
  once; fine for the directory sizes the Beeb uses (INFERRED).
- `fsr_expand` (contiguous pre-allocation, used by FORMAT) becomes "set the
  size"; `fsr_getfree` asks the share; VFS stays read-only (LUN >= 8 writes
  accepted and discarded above this layer, as now).

### Call sites

Only the Beeb's paths change, to explicit `fsr_*` calls (no `#define f_open`
- every routed call stays greppable and FatFs itself is untouched):

| file | FatFs calls |
|---|---|
| BeebSCSI/filesystem.c | 49 |
| videoplayer.c | 17 |
| fat_service.c | 15 |
| fujinet/fn_store_fatfs.c | 11 |
| BeebSCSI/filesystem_safewrite.c | 10 |
| M5000_emulator.c | 9 |

The webserver (80), MTP (25), secure service and USB storage stay on
FatFs: WebDAV/MTP show the card, as with `storage=usb`.

### Waiting

FatFs calls block; libsmb2 is async.  `fsr_*` starts the operation, then
runs `wifi_lwip_poll()` + `smb2_service()` until the callback fires or the
deadline passes, feeding the watchdog - the `usb_storage.c`
`command_wait()` pattern, same 10 s limit, and the Beeb gets a disc error,
never a hang.  Main loop only; never FIQ (unchanged rule).

**Re-entrancy - decided (dp111, 2026-10-15): refuse, don't wait.**  Some
callers run *inside* the network poll (FujiNet's store, `fn_store_fatfs.c`,
is driven from the net service); a wait there would re-enter
`wifi_lwip_poll()`.  `wifi_lwip_poll()` keeps a depth counter; an `fsr_*`
call routed to SMB while it is non-zero returns `FR_TIMEOUT` at once, with
no SMB traffic.  FatFs-routed calls are unaffected.  So with
`storage=smb` FujiNet's store is unavailable (its calls fail cleanly);
moving those callers to the async calls is left for later.

### Read-ahead

ADFS reads a few 256-byte sectors per command; each `f_read` would be a
round trip of a few ms on WiFi (INFERRED).  Each open network file gets a
read-ahead window (32-64 KB, GUESSED size) - one SMB READ fills it, small
reads are served from it, a write or seek outside invalidates it.  The
video player's frame reads (25-150 KB each, MEASURED on the current
Domesday side) are large enough to go straight through; at the median
frame + audio the side needs ~1.15 MB/s against the ~2.2 MB/s ceiling, so
playback fits but peaks want the player's 2-deep feed to stay ahead
(INFERRED).

### Link loss

As for an unplugged USB drive: the command fails (disc error), the share is
not used again until the next BREAK, which reconnects.  An open video is
closed like a jukebox to an empty drive.

## Configuration (Pi1MHz.cfg)

    storage=smb
    smb_server=192.168.0.10        (or a name the DNS knows)
    smb_share=Beeb
    smb_user=beeb
    smb_password=...

The password sits in plain text on the card: the user docs say to make a
dedicated local account for the share, not to use their own.
`*FX147,203,6` selects the share like 4/5 select card/drive.

## User documentation (Windows), outline

1. Make a local account `beeb` with a password (Settings > Accounts >
   Other users) - Windows 11 refuses guest access.
2. Make a folder, e.g. `C:\Beeb`, copy `BeebSCSI0`, `BeebVFS0`... into it.
3. Folder > Properties > Sharing > Advanced Sharing > Share this folder,
   Permissions: `beeb` Full Control (Read for a video-only library).
4. Network profile Private (so File and Printer Sharing is allowed through
   the firewall).
5. The PC's IP address (`ipconfig`) into `smb_server`.

## Stages

1. Socket shim + libsmb2 (mbedTLS AES) building in the tree, a host test
   against Samba: connect, read, write, list.  Cycle count of signing on
   the Zero 2 W.
2. `fsroute` with FatFs pass-through only; call sites converted; host
   tests of every existing suite still pass (no behaviour change).
3. The SMB route: files, directories, read-ahead, deadline, link loss.
   Host tests against Samba; then hardware against a Windows 11 share,
   each feature with the SD card as the control (ADFS read/write, MMFS,
   VFS + video).
4. `*FX147,203,6`, /status rows (share, server, round-trip, bytes), docs.

Not planned: SMB from WebDAV/MTP; Kerberos; SMB1.
