# Credits and Acknowledgements

Pi1MHz stands on a lot of other people's work. This file records the
third-party code, firmware and ROMs it builds on, and the projects it was
derived from. Each component keeps its own copyright and licence notice in
its own files; the summaries below point at those.

## Pi1MHz

Written and maintained by Dominic Plunkett (dp111) and contributors.
Licensed under the GNU General Public License v3 (see `ReadMe.md`).

## Thanks

With thanks to Ken Lowe, Mark Usher, Hoglet, BigEd and others in the BBC
Micro community for their testing, advice and contributions.

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
- **Howard Hinnant's** `days`↔`civil` date algorithms are used for the WebDAV
  timestamp conversion. `src/wifi/webserver.c`

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
