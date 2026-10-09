# USB Flash Drive

The Beeb's discs and files can live on a USB flash drive instead of the
SD card. Set `storage=usb` in `/Pi1MHz/Pi1MHz.cfg`, plug a drive into
the Pi's USB port, and the Beeb uses the drive for:

- its ADFS hard discs (`/BeebSCSI0`, `/BeebSCSI1`, ...)
- the VFS / LaserDisc discs (`/BeebVFS0`, ...), video included
- files it reads and writes through the Pi: MMFS (`BEEB.MMB`), MMFS2,
  the [SD explorer](sd-explorer.md) and the FAT transfer directory
- [FujiNet](fujinet.md)'s files and Music 5000 recordings

The SD card still holds `Pi1MHz.cfg`, the ROMs, the firmware and the
WiFi settings. The [web interface](web-interface.md) and
[USB file access](usb-file-access.md) show the SD card, not the drive.

## Setting it up

1. **Format the drive** FAT32 or exFAT (FAT16 works too) and copy the
   folders across in the same layout as on the SD card - the drive's
   root holds `BeebSCSI0`, `BeebVFS0`, `BEEB.MMB` and so on.
2. **Set `storage=usb`** in `/Pi1MHz/Pi1MHz.cfg`.
3. **Keep the port a host** - the default (`usb_mode=host`). A mouse
   and a drive can be used together through a USB hub. On a Pi Zero use
   the inner micro-USB socket with an OTG adapter.
4. **Plug the drive in and power up.**

## When the drive is used

- **At power-on** the Pi waits up to about 5 seconds for the drive to
  appear before the Beeb's first disc access is answered, so the Beeb
  starts from the drive. If no drive appears, the SD card is used.
- **A drive plugged in later** is used from the next BREAK - never
  swapped in under a running program.
- **A drive pulled out** gives disc errors until the next BREAK, which
  goes back to the SD card. Don't pull it out while the Beeb is writing
  to it.

The status page shows a **Beeb storage** row (`SD card` or
`USB drive`) and a **USB drive** row: the drive's vendor:product ID,
its size and its file system, or `none`.

## Limits

- The drive must have 512-byte sectors - nearly all do. One that does
  not is listed as unusable.
- Only the first drive found is used.
- A file of 4 GB or more (exFAT only) shows a wrong size to the Beeb's
  tools; nothing on a Beeb needs one.
