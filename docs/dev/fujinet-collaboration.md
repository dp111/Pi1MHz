# FujiNet collaboration - status & pickup plan

Where the FujiNet work stands and exactly what to do when **Mark Fisher
(fenrock)** comes back. Written 2026-08-03. Background research and the
byte-level details are in the session notes (`fujinet-tnfs-assessment` memory)
and [network-service-stages.md](network-service-stages.md) §Stage 5.

## The two meanings of "FujiNet compatibility"

1. **USE the FujiNet ecosystem** - reach FujiNet's content (public **TNFS**
   servers, disc-image hosts, HTTP). Needs *nothing* from the FujiNet project.
2. **BE a FujiNet client platform** - run FujiNet's client library / apps on the
   Beeb through Pi1MHz. Needs coordination + an external toolchain, gated on
   licensing.

## ✅ Done (sense 1, and all the code-side prep for sense 2)

- **N: `TNFS:` scheme - read / browse / write - hardware-validated** on a real
  BBC Master over the 1MHz bus (list a share, read a file, write a file + read it
  back). Plus HTTP/TCP/UDP/TELNET. So a Beeb can consume FujiNet's TNFS content
  today. This is the actual, dependency-free ecosystem interop and it is shipped.
- **Codec validated byte-for-byte against real FujiNet `tnfsd`**
  (`src/tests/net/tnfs_live.c`, 12/12). `MOUNT "/"` is correct and universal - no
  mountpoint change needed (tnfsd serves its root as `/`).
- **FujiNet conformance pass** so a future `fujinet-lib` "bbc" backend is a thin
  shim: `url_status` leads with the 4-byte **DVSTAT**
  `{bytes_waiting_lo, hi, connected, error}`; `url_open` uses FujiNet **aux1**
  open modes (4=read / 8=write / 12=read-write / 13=dir). Our N: verbs already
  mirror FujiNet's `network_open/read/write/status/close`.

## ⛔ Blocked on the human step (sense 2)

None of the following is code we can do unilaterally:

1. **Licensing.** `markjfisher/fn-rom` and `markjfisher/fujinet-nio-lib` are both
   `license: null` (all-rights-reserved). We **cannot legally fork/adapt** them
   until Mark adds a license (expected GPL-3.0, compatible with Pi1MHz's GPLv3).
2. **The transport question.** fenrock's BBC FujiNet works **today over RS423
   serial to a separate ESP32**; he has stated he wants to move to the **1MHz
   bus** - which is Pi1MHz's native interface. The pitch: **Pi1MHz becomes the
   1MHz-bus FujiNet transport/device, replacing the ESP32**, backed by the net
   stack in this repo.
3. **cc65 BBC (Acorn) C target** exists only as an unofficial fork
   (`markjfisher/cc65` `bbc`/`bbc-clib`), needed to build the client lib/apps.

## Pickup plan when Mark responds

1. **Confirm licensing** on `fn-rom` + `fujinet-nio-lib` (GPL-3.0 hoped). Until
   this lands, do not fork/vendor their code.
2. **Propose Pi1MHz as the BBC 1MHz-bus transport** for `fujinet-nio-lib` - his
   own roadmap item. The clean seam is nio-lib's **"direct transport" backend**:
   ~4 functions (`fn_transport_init/ready/exchange/fn_platform_name`). Map its
   FujiBus request packet onto our JIM N: command block (write cmd/aux/handle,
   dispatch `&FCAA`, spin bit 7, read result + DVSTAT, bulk via the JIM window).
   The mapping is close to 1:1 because our N: verbs + DVSTAT + aux modes are
   already FujiNet-shaped (that was the point of the conformance pass).
3. **Scope: network (N:) device only.** Skip the `fuji_*` host-slot/disk-mount/
   config device - it is meaningless on Pi1MHz, which *is* the storage provider.
4. **First proof-of-concept:** hand-write one `fn_open("N:HTTP://...")` +
   `fn_read` as a JIM-block sequence and check the FujiBus-packet <-> JIM-block
   field mapping is 1:1 (open aux, DVSTAT bytes, HTTP channel mode). We already
   have `beeb/net/NETHTTP.BAS` doing essentially this - annotate it against the
   nio-lib `fn_transport_exchange` contract. If the mapping is clean, write the
   backend against `markjfisher/cc65@bbc` and run it under nio-lib's Beebium
   `test-bbc-scripted` harness pointed at a Pi.

## Explicit non-goals

- Pi1MHz emulating a FujiNet device to *existing* host software (wire ABI is
  Atari SIO framing, not our mailbox; ~no BBC FujiNet host software exists).
- The `fuji_*` config/host-slot/disk device.

## Contacts & sources

- **Mark Fisher / fenrock** - `#acorn-and-beebs` (FujiNet Discord); stardot
  thread <https://stardot.org.uk/forums/viewtopic.php?t=32874>.
- `markjfisher/fn-rom` (BBC FujiNet ROM, MMFS2-based), `markjfisher/fujinet-nio-lib`
  (clean rewrite, BBC is a first-class target; transport delegated to fn-rom),
  `markjfisher/cc65@bbc`.
- `github.com/FujiNetWIFI/tnfsd` (build `make OS=LINUX` after
  `mv atari-boot-xex-file.asm{,.bak}` to skip the `xa` assembler dep).

## Host-side test tooling (this repo / session, not on the Pi)

Small servers used for the on-hardware validations live in the working
directory (`claude-tmp/`), not committed here: `tnfs-server.ps1` (needs
`SIO_UDP_CONNRESET` off), `telnet-server.ps1`, `http-responder.ps1`,
`udp-echo.ps1`, `connect-send.ps1`. The real-`tnfsd` interop check is committed
at `src/tests/net/tnfs_live.c`.
