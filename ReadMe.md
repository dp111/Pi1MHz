# Pi1MHz

[![Codacy Badge](https://api.codacy.com/project/badge/Grade/ebe2e1bd0b1c42719c0a7ea5bec9bed2)](https://app.codacy.com/app/dominic.plunkett/Pi1MHz?utm_source=github.com&utm_medium=referral&utm_content=dp111/Pi1MHz&utm_campaign=Badge_Grade_Settings)

Pi1MHz is a Raspberry Pi bare-metal interface for the BBC Micro / Master
1MHz bus. A Pi plugged into the 1MHz bus (through a level-shifter board)
becomes, all at once:

- **Hard discs** - ADFS via BeebSCSI-compatible SCSI emulation, up to 8
  drives from image files on the SD card, plus read-only VFS/LaserDisc
  volumes for Domesday software
- **MMFS / MMFS2** - DFS disc images served from the SD card
- **Music 5000 / 3000** synthesiser, and an optional **SID** chip (BeebSID)
- **JIM RAM expansion** - hundreds of megabytes of paged RAM
- **HDMI output** - a screen redirector with a mouse-pointer overlay
- **WiFi** (on WiFi-equipped Pis) - a built-in web interface and WebDAV
  mount for the SD card
- **Econet over WiFi** (AUN) and **Teletext** (from an internet stream)
- **USB file access** (MTP) - the Pi appears as a portable device

Everything is driven from image files and a single plain-text config file
on the SD card - nothing to compile.

## Supported hardware

- BBC Micro (B / B+) or BBC Master with a 1MHz bus, plus a Pi1MHz
  level-shifter board.
- Raspberry Pi Zero / Zero W, Pi Zero 2 W, or Pi 3B+. A plain Pi Zero
  works for everything except WiFi; the Pi 3A+ is expected to work but is
  not formally tested.
- An SD card, and 5V power for the Pi (often taken from the BBC).

WiFi additionally needs the brcmfmac firmware blobs for the Pi's chip
under `/Pi1MHz/wifi/` on the SD card (the standard firmware set includes
them).

## Getting started

Copy the contents of `firmware/` to the root of the SD card, insert it,
connect the interface, and power on. Then, from BASIC:

    X%=0 : CALL &FC88

should show the help screen - proof the Pi is alive on the bus. (On a
fast machine with a slow SD card, an extra CTRL-BREAK may be needed.)

The full walk-through is in the [Getting started](docs/user/getting-started.md)
guide.

## Documentation

- **[User guide](docs/user/README.md)** - getting started, the full
  `Pi1MHz.cfg` reference, and a page per feature (hard discs, MMFS,
  sound, RAM, screen, WiFi, WebDAV, Econet/AUN, teletext, USB).
- **[Advanced / programming reference](docs/advanced.md)** - the
  memory-mapped registers and command protocols for writing 6502 software
  that talks to Pi1MHz directly (JIM RAM, the SD/FAT services port, the
  helper calling convention, `kernel.now`/`reboot.now`).
- **[WiFi / webserver internals](src/wifi/README.md)** - per-board WiFi
  firmware mapping and the webserver/WebDAV implementation.
- **[Credits](CREDITS.md)** - third-party sources, licences and
  contributors.

## Notes

- PCB space is limited for a dedicated serial debug connector on some
  builds; a custom 3-pin header (0V, TX, RX) can be fitted under a Pi Zero.

## Credits and acknowledgements

Pi1MHz builds on FatFs, lwIP, TinyUSB, VICE's FastSID, BeebSCSI,
PiTubeDirect, PicoWi and others, and ships third-party firmware and ROMs.
See [CREDITS.md](CREDITS.md) for the full list with sources and licences.

## Donations

Donations are welcome, especially from commercial kit/board sellers using
this project.

## SAST tools

- [PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source): static analyzer for C, C++, C#, and Java.

## License

Pi1MHz is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version.

Pi1MHz is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details.

You should have received a copy of the GNU General Public License along
with Pi1MHz. If not, see http://www.gnu.org/licenses/.
