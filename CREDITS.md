# Credits and Acknowledgements

Pi1MHz stands on a lot of other people's work. This file records the
third-party code, firmware and ROMs it builds on, and the projects it was
derived from. Each component keeps its own copyright and licence notice in
its own files; the summaries below point at those.

## Pi1MHz

Written and maintained by Dominic Plunkett (dp111) and contributors.
Licensed under the GNU General Public License v3 (see `ReadMe.md`).

## Thanks

With thanks to Ken Lowe, Mark Usher, Hoglet, BigEd, Peter Clarke and others in the BBC
Micro community for their testing, advice and contributions.

The Raspberrypi Engineers who have helped me with this project.

## Vendored libraries

| Component | Source / author | Licence | In tree |
|---|---|---|---|
| **FatFs** R0.16 — FAT filesystem | ChaN, <http://elm-chan.org/fsw/ff/> | BSD-style (1-clause) | `src/BeebSCSI/fatfs/` |
| **lwIP** — TCP/IP stack | Swedish Institute of Computer Science; Adam Dunkels and contributors | BSD-3-Clause | `src/wifi/lwip/` |
| **TinyUSB** — USB device stack (MTP) | Ha Thach, <https://tinyusb.org> | MIT | `src/usb/tinyusb/` |
| **FastSID** — MOS6581/8580 SID emulation | Teemu Rantanen, Michael Schwendt, Ettore Perazzoli; vendored from **VICE** 3.1. Ported into Pi1MHz as BeebSID by **Andrew Fawcett**. | GPL-2.0 | `src/fastsid/`, `src/BeebSID/` (see `src/fastsid/ORIGIN.md`) |
| **Newlib** ARM string routines | Red Hat / newlib `machine/arm`, pulled via the arm-none-eabi toolchain | BSD-style | `src/lib/armstring-pi/` (see `NOTES.md`) |

## Derived code

- **BeebSCSI** — the ADFS hard-disc / SCSI emulation is based on Simon Inns'
  BeebSCSI (Domesday86), GPL-3.0. <https://www.domesday86.com/?page_id=400> ·
  `src/BeebSCSI/`
- **PiTubeDirect** — the HDMI screen renderer and BBC fonts are cut down from
  PiTubeDirect (David Banks / hoglet67 and contributors). `src/framebuffer/`
- **Teletext (MODE 7)** — the SAA5050 teletext renderer in
  `src/framebuffer/teletext.c` is by **Rod Thomas** (original, Jan 2021)
  with significant additions by **Hoglet** (Feb 2021), via PiTubeDirect.
- **Teletext adapter (ATS)** — the Acorn Teletext Adapter emulation
  (`src/teletext_emulator.c`, `&FC10-&FC13`) is a port of the **BeebEm**
  project's `Teletext.cpp` to the Pi1MHz bus by Dominic Plunkett.
- **PicoWi** — the CYW43 WiFi join/association sequence is a faithful port of
  Jeremy Bentham's bare-metal PicoWi driver (`picowi_join.c`), including its
  ioctl ordering and settle delays. <https://iosoft.blog> · `src/wifi/sdio.c`
- **1MHz-WiFi host ROM** — `beeb/1mhz-wifi/` is a sideways ROM written for
  this project by **Peter Clarke**, the host half of `wifi_service`. It
  implements the command interface of **Roland Leurs' ElkWiFi** cartridge ROM
  and keeps that ROM's command-table *format*, and `*VERSION` credits ElkWiFi
  for it, but it carries no ElkWiFi source: the parts that did derive from it
  (the cassette filing system, itself deriving from Martin Barr's UPCFS) are
  not in this ROM. That distinction matters, because upstream
  <https://github.com/AtomicRoland/ElkWiFi> carries **no licence** - no
  LICENSE file, nothing in its README, and its sources are marked only
  "(c) Roland Leurs, May 2020" - so ElkWiFi code could not be carried here.

- **Howard Hinnant's** `days`↔`civil` date algorithms are used for the WebDAV
  timestamp conversion. `src/wifi/webserver.c`
- **Acorn MOS 3.20 and GXR 1.20 graphics** — `src/framebuffer/primitives.c`
  re-implements in C two sets of Acorn ROM routines, © Acorn Computers Ltd:
  the circle, arc, chord and sector drawing of the BBC Master's MOS 3.20
  (`master_walk`), worked out from Tom Seddon's MOS disassembly
  <https://github.com/tom-seddon/acorn_mos_disassembly>, and the ellipse
  drawing of the Graphics Extension ROM 1.20 (`gxr_ellipse`), worked out from
  Toby Nelson's GXR reassembly <https://github.com/tobylobster/GXR-pages>.
  No ROM code or data is included. The rest of the VDU driver's BBC
  behaviour (lines, triangles, text, VDU 5 and so on) was matched by testing
  against the ROMs, not taken from their code; each source file names what
  it follows.

## Test tools

- **beebjit** — Chris Evans' BBC Micro emulator, run headless as the
  reference for `tools/vdutest/`. <https://github.com/scarybeasts/beebjit>
- `tools/vdutest/mos65.py` runs the MOS 3.20 VDU code in a small 65C12
  interpreter as a faster reference. The Acorn ROM images it and beebjit use
  are read from the beebjit tree at run time and are not part of Pi1MHz.

## Redistributed firmware

- **CYW43 WiFi firmware** (`brcmfmac43430/43436/43455-sdio.*`) — Cypress /
  Infineon, as redistributed by the Raspberry Pi `firmware-nonfree` /
  `linux-firmware` projects. Shipped under `firmware/Pi1MHz/wifi/` for the
  onboard WiFi chip; redistributed under the terms of that firmware's own
  licence. `firmware/Pi1MHz/wifi/`

## Redistributed ROMs (Beeb-side sideways ROMs)

These are third-party BBC Micro / Master ROM images shipped for convenience so
the emulated hardware can be used as it was originally. They remain the
copyright of their respective owners and are included for use with genuine
hardware; they are not part of Pi1MHz's own GPL-3.0 licence.

- **ADFS**, **ANFS 4.26**, **NFS 3.65**, **ATS** (Advanced Teletext System) —
  Acorn Computers Ltd / © BBC. `firmware/Pi1MHz/ADFS.rom`, `AUNFSM128.rom`,
  `AUNFSbeeb.rom`, `ATS.rom`. The AUNFS ROMs are ports of Acorn's Econet
  filing systems with the Econet hardware layer replaced by Pi1MHz's AUN
  commands.
- **1MHz-WiCFS** — the UEF cassette filing system, in the merged ROM image
  `firmware/Pi1MHz/1mhz-wicfs.rom` (helper 17). It derives from **Roland
  Leurs'** ElkWiFi 0.23 <https://github.com/AtomicRoland/ElkWiFi> and through
  it from **Martin Barr's** UPCFS, and is combined with Peter Clarke's
  1MHz-WiFi ROM. Roland Leurs' terms, quoted in full and given for this use:

  > 1. You have the right to use the Work in any way you want for
  >    non-commercial use. Commercial use is considered when you integrate the
  >    Work into your own products or replicate the Work and sell it.
  > 2. You may create your own hardware and software based on the Work for
  >    non-commercial use. However, for deviated projects you must use the
  >    same license.

  So this image is **not** under Pi1MHz's GPL-3.0: it carries the licence
  above, as clause 2 requires of anything derived from that Work. Pi1MHz is
  a non-commercial project and ships it on those terms. The GPL-3.0
  1MHz-WiFi ROM in `beeb/1mhz-wifi/` is a separate image (helper 16) and is
  unaffected.
- **MMFS / MMFS2** — Martin Mather and the MMFS project; Pi1MHz-specific
  sideways-RAM builds. <https://github.com/hoglet67/MMFS> ·
  `firmware/Pi1MHz/SWMMFS.rom`, `SWMMFS2.rom`
- **BeebSCSI helper ROM** — Simon Inns (Domesday86). `firmware/Pi1MHz/BSRom.rom`

## Static analysis

- **PVS-Studio** — free static analysis for open-source projects, used on
  Pi1MHz. <https://pvs-studio.com>

---

If you are a copyright holder and something here is miscredited or should not
be redistributed, please open an issue on the Pi1MHz GitHub repository.
