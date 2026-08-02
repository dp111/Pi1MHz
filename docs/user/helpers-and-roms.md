# Loading ROMs and the Helper Functions

Pi1MHz includes a set of "helper functions" - small jobs the Beeb can
ask the Pi to do, the most useful of which is loading a filing-system
ROM straight into sideways RAM. No EPROMs, no ROM board: the ROM images
ship on the Pi's SD card in the `/Pi1MHz` directory.

## Calling a helper

From BASIC:

```
X%=n : CALL &FC88
```

where `n` is the helper number. Two equivalent forms, usable from the
command line:

```
*FX147,136,n
*GO FD00
```

(on a Master, `*GOIO FD00`). `*FX147,136,n` pokes the helper number
into `&FC88` and `*GO FD00` runs the helper code that Pi1MHz supplies
through its JIM window.

The helpers need the file `/Pi1MHz/6502code.bin` on the SD card - it is
part of the standard firmware set.

## The helpers

| n | What it does | ROM file used |
|---|---|---|
| 0 | Shows the help screen (version, build date, Pi model, temperature, and this list) | - |
| 1 | Status - **not implemented yet** | - |
| 2 | Enables the screen redirector: everything the Beeb prints also appears on the Pi's HDMI output. See [Screen and video](screen-and-video.md) | - |
| 3 | Loads ADFS into sideways RAM | `/Pi1MHz/ADFS.rom` |
| 4 | Loads MMFS into sideways RAM | `/Pi1MHz/SWMMFS.rom` |
| 5 | Loads MMFS2 into sideways RAM | `/Pi1MHz/SWMMFS2.rom` |
| 6 | Loads the BeebSCSI helper ROM (BeebSCSI utility commands) | `/Pi1MHz/BSrom.rom` |
| 7 | Loads the ATS teletext software ROM | `/Pi1MHz/ATS.rom` |
| 8 | Loads the Econet-over-WiFi filing system for the BBC B | `/Pi1MHz/AUNFSbeeb.rom` |
| 9 | Loads the Econet-over-WiFi filing system for the Master 128 | `/Pi1MHz/AUNFSM128.rom` |
| 10-15 | Loads a ROM of your own into sideways RAM | `/Pi1MHz/ROM10.rom` ... `/Pi1MHz/ROM15.rom` |

After loading a ROM press **CTRL-BREAK** so the OS notices it.

## Your own ROMs

Copy any 16K sideways ROM image onto the SD card as `/Pi1MHz/ROM10.rom`
(up to `ROM15.rom`) and load it with helper 10-15. This is an easy way
to try ROM software without hardware.

## Requirements and limits

- ROM loading needs writable sideways RAM in the target machine (a
  Master has it built in; a BBC B needs a sideways RAM board or
  fitted RAM).
- The loader scans the sideways slots from 15 downwards, skips any that
  already hold a ROM image, and loads into the first empty writable
  slot it finds. `No SWR` means it found no free sideways RAM;
  `No ROM` means the ROM file was missing from the SD card. Note that
  loading the same ROM twice fills two slots.

## The VFS ROM

`/Pi1MHz/VFS.rom` (for Domesday/VFS volumes) is also shipped in the
`/Pi1MHz` directory, but it is not wired to a helper number - install
it as a normal ROM, or load it as one of the user ROMs by copying it to
`ROM10.rom`.
