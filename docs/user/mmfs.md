# MMFS and MMFS2: DFS Disc Images

MMFS is the popular DFS-compatible filing system that serves ordinary
DFS disc images, so classic disc-based software runs unmodified - no
floppy drive, no floppies. It is a separate project by Martin Mather
and others (<https://github.com/hoglet67/MMFS>); Pi1MHz ships its
Pi1MHz-specific sideways-RAM builds, which read their disc images from
the **Pi's** SD card through the Pi1MHz FAT service instead of a
separate SD card wired to the user port.

Two generations are included, loaded with helper 4 or 5
(see [Loading ROMs](helpers-and-roms.md)):

| Helper | File on the SD card | What it is |
|---|---|---|
| 4 | `/Pi1MHz/SWMMFS.rom` | MMFS 1.60, Model B sideways-RAM build. Serves discs from a `BEEB.MMB` image store. |
| 5 | `/Pi1MHz/SWMMFS2.rom` | MMFS2 1.60, Model B sideways-RAM build. Serves individual `.ssd`/`.dsd` disc-image files - no MMB container. |

Load one, press **CTRL-BREAK**, and select it with `*MMFS` (or
`*DISC`/`*CARD` - see the MMFS documentation for the full command
set). The ordinary DFS commands then work as on a real DFS: `*CAT`,
`LOAD`/`SAVE`, `*DRIVE`, `*INFO`, `*BUILD` and so on.

## The files on the SD card

### MMFS: `BEEB.MMB`

MMFS keeps its discs inside one container file, `BEEB.MMB`, in the
**root of the Pi's SD card**. An MMB file is a simple bundle of
standard 200K DFS disc images plus a catalogue; ready-made ones are
widely available (games archives are often distributed this way), and
the standard MMB tools (`mmbutils`, BeebEm's MMB support and similar)
create and edit them. Inside MMFS, discs in the MMB are selected by
number/name with the `*D...` disc-management commands, then accessed
as normal DFS drives.

### MMFS2: plain disc-image files

MMFS2 does away with the container: each disc is an ordinary
`.ssd`/`.dsd` file on the Pi's SD card, mounted by name. That makes
single discs trivial to add, rename or back up - they are just files.
See the MMFS project's documentation for its disc-selection commands
and where it looks for the image files.

MMFS2 opens those files by name through the Pi1MHz FAT service. Each
drive has its own directory on the Pi (`*DDIR`), and `*DCAT` lists it.
MMFS2 shows 8.3 names: a long name appears as its short name, the way
DOS shows it (`Long Name Game.ssd` as `LONGNA~1.SSD`), and that name
works in `*DIN`. On an exFAT volume, which has no short names, the Pi
makes them up from the first four characters, a `~` and three hex
digits.

`*DACCESS <image> L` locks a disc image by making the file read-only, so
a PC sees the lock too; `*SAVE` to a locked disc gives "Disc read only".
`*DRENAME` renames the file.

## Changing images from another computer

You can look inside `BEEB.MMB` and the disc images - browse slots,
extract files, add files to a disc, insert whole discs into slots -
from a web browser: see [the disc image viewer](disc-viewer.md).

Because everything lives on the Pi's SD card, disc images (or the
whole `BEEB.MMB`) can be added and replaced over
[WiFi/WebDAV](web-interface.md) or [USB](usb-file-access.md) without
removing the card - but do it carefully. The web server, WebDAV and
USB/MTP all refuse to overwrite, delete or move a file the Beeb
currently holds open through the FAT service (the same protection the
hard disc images have): `BEEB.MMB` while MMFS uses it, and each disc
image MMFS2 has in a drive.

A **BREAK** closes them, which lets an upload through, while MMFS keeps
its drives: the next disc access opens the file again by name. So:

1. **On the Beeb, finish with the disc.** Close any open files
   (`*CLOSE`), then press BREAK.
2. Upload or replace the image / `BEEB.MMB`.
3. **Re-mount the disc** (`*DIN` again, or `*MMFS`) before you write to
   it, so MMFS reads the new catalogue rather than writing one cached
   from before the upload over your new image.

Adding a *new* image file (rather than replacing one in use) is safe
at any time; the Beeb just cannot see it until the disc/catalogue is
re-read.

If an upload keeps being refused with "in use by the Beeb", MMFS has
used the disc again since the BREAK. BREAK and try again; a Pi reboot
from the web interface clears everything as a last resort.

## When the Pi restarts

A Pi restart (a reboot, a firmware update, a power cut to the Pi alone)
closes everything the Beeb had open. Press BREAK once the Pi is back.
MMFS then carries on, reopening `BEEB.MMB`. MMFS2 finds the Pi has
forgotten its drives and directories and starts again as after a
CTRL-BREAK: both drives empty, the root directory, so `*DDIR` and
`*DIN` your discs again.

The MMFS2 ROM and the Pi firmware go together: the ROM on the card
comes with the firmware. MMFS2 from a newer release on an older Pi
firmware stops with "Pi1MHz too old" rather than use commands the Pi
does not have; update the firmware (or use the ROM it came with).

## Write-protecting the card from the Beeb

Add **`Beeb_write_protect=1`** to `/Pi1MHz/Pi1MHz.cfg` to stop the Beeb
changing anything on the SD card (the Pi reads the file when it starts,
so restart the Pi after changing it - a BREAK is not enough). Every write the Beeb makes - MMFS/MMFS2
`*SAVE`/`*DELETE`, `*DACCESS`, `*DRENAME`, ADFS writes to the [hard disc images](hard-discs.md),
even a `*FORM`/format - is silently ignored and reported back as success,
so nothing on the card changes and the Beeb sees no error. Reads are
unaffected, so all your discs still load normally.

This only blocks the **Beeb**. Uploads over
[WiFi/WebDAV](web-interface.md) and [USB](usb-file-access.md) still write
the card as usual - so you can curate the images from a PC while the Beeb
can only read them. The key is off by default (the Beeb can write). It has
no effect on the read-only [VFS](hard-discs.md) discs, which never take
Beeb writes anyway.

## Notes

- Both shipped ROMs are BBC Model B sideways-RAM builds; they need
  writable sideways RAM (see [Loading ROMs](helpers-and-roms.md)).
- MMFS and the [hard disc emulation](hard-discs.md) coexist happily:
  DFS software from MMFS images, ADFS on the hard discs.
- The MMFS project documents the full command set, MMB management and
  the differences between MMFS and MMFS2 in detail:
  <https://github.com/hoglet67/MMFS>.
