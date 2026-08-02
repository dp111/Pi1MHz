# Hard Discs (ADFS / BeebSCSI)

Pi1MHz emulates the classic Acorn SCSI hard disc arrangement - the same
thing a BeebSCSI board or a period Winchester drive provides. To ADFS
it looks exactly like real hardware, so it works with the standard ADFS
ROM on a Master, and with ADFS in sideways RAM or ROM on a BBC B.

Discs are just files on the Pi's SD card, so you can back them up, copy
them, and download ready-made images.

## Where the disc images live

ADFS drives 0-7 come from a directory in the SD card root called
`/BeebSCSI0`:

```
/BeebSCSI0/scsi0.dat      drive 0 disc image
/BeebSCSI0/scsi0.dsc      drive 0 geometry descriptor
/BeebSCSI0/scsi0.cfg      drive 0 extra settings (optional)
/BeebSCSI0/scsi1.dat      drive 1 disc image
...up to scsi7.dat
```

- The `.dat` file is the disc itself - the raw contents, byte for byte.
- The `.dsc` file describes the drive's shape (cylinders and heads).
  Formatting tools create it; ready-made image sets include it.
- The `.cfg` file is optional and overrides low-level SCSI details for
  that one drive. Defaults for all drives come from
  `/Pi1MHz/defscsi.cfg`, which emulates a 21MB Rodime RO652 (the drive
  in an Acorn FileStore E20). Most people never touch either file.

You only need the drives you actually use - a single `scsi0.dat` is a
perfectly good start. `/BeebSCSI0` is created automatically if missing.

A good ready-made starter set (with the BeebSCSI utilities on it) is at
<https://www.domesday86.com/wp-content/uploads/2019/03/BeebSCSI_Quickstart_LUN_2_5.zip>,
and the BeebSCSI documentation at <https://www.domesday86.com/?page_id=400>
covers creating and formatting images in depth.

## Using it

On a Master:

```
*ADFS
*CAT
```

On a BBC B/B+, get ADFS into sideways RAM first
(see [Loading ROMs](helpers-and-roms.md)):

```
X%=3 : CALL &FC88
```

then CTRL-BREAK and `*ADFS`.

New, empty images can be formatted from the Beeb with the usual
BeebSCSI/Acorn tools (e.g. SuperForm); creating a fresh drive writes a
new `.dat` on the SD card.

If the machine boots faster than the Pi (fast Master, slow SD card),
the first CTRL-BREAK may not find the disc - press CTRL-BREAK again.

## Jukeboxes: more than 8 discs

You can keep many complete sets of discs on one card:

```
/BeebSCSI0/    disc set 0 (the default)
/BeebSCSI1/    disc set 1
/BeebSCSI2/    disc set 2
...
```

Switch sets ("jukeboxing") from the Beeb:

```
*FX147,65,1
```

selects `/BeebSCSI1` (the last number is the set you want). Pi1MHz
shows this command, with the correct register number for your setup, on
the help screen (`X%=0 : CALL &FC88`).

To make a different set the power-on default, put this in
`/Pi1MHz/Pi1MHz.cfg`:

```
SCSIJUKE=1
```

## VFS / Domesday volumes

Drives 8-15 are VFS volumes - the read-only LaserDisc filing system
used by the Domesday system. They live in `/BeebVFS0` (and `/BeebVFS1`
etc., selected with `VFSJUKE`), with the same `scsi0.dat`-style naming:

```
/BeebVFS0/scsi0.dat       VFS volume 0
/BeebVFS0/scsi0.dsc
...
```

VFS volumes are strictly read-only: Pi1MHz will never create or write
them. You need the VFS ROM on the Beeb to use them
(see [Loading ROMs](helpers-and-roms.md)) - then `*VFS` and `*CAT` as
on a real system. The video side of Domesday (the LaserDisc player
emulation) also uses the Pi's HDMI output - see
[Screen and video](screen-and-video.md); parts of that are still a work
in progress.

## Two adapters on a Master

`SCSIID=n` in `Pi1MHz.cfg` makes the emulation answer only that SCSI
ID, for setups with more than one adapter. The default `0` answers
every ID, which is what you want in the normal one-adapter case.

## Managing images without pulling the card

With [WiFi](wifi.md) set up you can download, upload, rename and
delete disc images from another computer through the
[web interface or a WebDAV mount](web-interface.md), or over
[USB](usb-file-access.md) - the drive the Beeb is using lives at
`/BeebSCSI0/scsi0.dat` like any other file. Backing up a drive is
simply downloading its `.dat` (and `.dsc`) files.

All three routes respect the same interlock: an image belonging to a
**started** LUN - one the Beeb currently has open - cannot be
overwritten, deleted or moved, and neither can a folder containing
one. The upload is refused with "in use by the Beeb" (WebDAV clients
see a 423 Locked; MTP reports the device busy). A long upload is also
re-checked at the end: if the Beeb starts the LUN while the transfer
is still streaming, the partial file is discarded rather than swapped
in under a running filing system.

To release a LUN so its image can be replaced:

- **`*BYE`** (in ADFS) parks the drives and stops the LUN - the clean
  way before swapping an image.
- A stopped LUN starts again automatically the next time the Beeb
  accesses it (the next `*CAT` or CTRL-BREAK + `*ADFS`), so there is
  nothing to "re-enable" afterwards - but do get the upload finished
  before poking the drive on the Beeb, or the re-check above will
  refuse the swap.
- Selecting a different disc set (`*FX147,65,n` / `SCSIJUKE`) also
  releases the previous set's images.

Images in disc sets the Beeb is *not* currently using, and brand-new
files, can be managed freely at any time.
