# Pi1MHz User Guide

Pi1MHz turns a Raspberry Pi into a multi-function expansion for the
BBC Micro and BBC Master. It plugs into the 1MHz bus via a buffer board and
provides, all at once:

- **Hard discs** for ADFS (BeebSCSI-compatible SCSI emulation, up to 8
  drives from image files on the SD card), plus read-only VFS/LaserDisc
  volumes for Domesday-style software
- **Music 5000 / Music 3000** synthesiser emulation, and an optional
  **SID chip (BeebSID)**
- **RAM expansion** - hundreds of megabytes of JIM paged RAM
- **HDMI output** - a screen redirector so the Beeb's output can appear
  on a modern monitor, with a mouse pointer overlay
- **WiFi** (on WiFi-equipped Pis) with a built-in web interface, so you
  can manage the SD card from another computer without removing it
- **Econet over WiFi** (AUN), letting the Beeb talk to Econet fileservers
  across your home network
- **Teletext** - an Acorn Teletext Adapter fed from internet teletext
  streams
- **USB file access** - the Pi shows up on a PC as a portable device for
  copying files to and from the SD card

Everything is driven from image files and a single plain-text
configuration file on the Pi's SD card. You never need to compile
anything.

## The guides

Start here:

- [Getting started](getting-started.md) - what you need, preparing the
  SD card, first boot, and checking it works
- [Configuration reference](configuration.md) - every option in
  `Pi1MHz.cfg`, with defaults and examples

Then one guide per feature:

- [Hard discs (ADFS / BeebSCSI)](hard-discs.md)
- [SCSI drive descriptors (scsi.cfg)](scsi-cfg.md)
- [MMFS and MMFS2 (DFS disc images)](mmfs.md)
- [Loading ROMs and the helper functions](helpers-and-roms.md)
- [Sound: Music 5000/3000 and BeebSID](sound.md)
- [RAM expansion (JIM)](ram-expansion.md)
- [HDMI screen, mouse pointer and video](screen-and-video.md)
- [WiFi setup](wifi.md)
- [The web interface and WebDAV](web-interface.md)
- [The disc image viewer and editor](disc-viewer.md)
- [Econet over WiFi (AUN)](econet-aun.md)
- [Teletext](teletext.md)
- [USB file access (MTP)](usb-file-access.md)

And when things go wrong:

- [Troubleshooting](troubleshooting.md)

For programming Pi1MHz directly from 6502 code (registers and command
protocols), see the [Advanced / programming reference](../advanced.md).

Pi1MHz is free software (GPL-3.0) and builds on the work of many other
projects - see [CREDITS.md](../../CREDITS.md) for the full acknowledgements.

## Conventions used in these guides

- `&FC00`-style numbers are hexadecimal addresses, as on the Beeb.
- `*commands` are typed at the BASIC prompt.
- "SD root" means the top-level directory of the Pi's SD card.
- Paths like `/Pi1MHz/Pi1MHz.cfg` are files on the SD card, not on the
  Beeb's discs.

## Which machines does it work with?

Pi1MHz is designed for machines with a 1MHz bus: the BBC B, B+ and the
BBC Master series. The project-tested Raspberry Pi models are the
Pi Zero W, Pi Zero 2 W and Pi 3B+ (a plain Pi Zero works too, minus
WiFi; the Pi 3A+ is expected to work but is not formally tested).

The Acorn Electron has no 1MHz bus of its own. Electron expansions that
provide one have not been tested with Pi1MHz, so nothing here should be
taken as a promise that they work.
