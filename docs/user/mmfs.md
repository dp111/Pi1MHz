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

## Changing images from another computer

Because everything lives on the Pi's SD card, disc images (or the
whole `BEEB.MMB`) can be added and replaced over
[WiFi/WebDAV](web-interface.md) or [USB](usb-file-access.md) without
removing the card - but do it carefully. Unlike the hard disc images
(which the web server refuses to overwrite while the Beeb has them
open), there is **no interlock** on the files MMFS uses: nothing stops
a file being replaced while MMFS has it open, and the Beeb keeps its
own cached idea of the catalogue either way. Replacing an image under
a live filing system is a recipe for a corrupted disc image, on either
side.

The procedure that works:

1. **On the Beeb, finish with the disc first.** Close any open files
   (`*CLOSE`), and put the filing system down - select another one
   (`*TAPE` is always there) or just press CTRL-BREAK.
2. Upload or replace the image / `BEEB.MMB`.
3. **On the Beeb, restart the filing system** so it re-reads
   everything instead of trusting stale state: CTRL-BREAK, select
   MMFS again, and re-mount the disc. Don't skip this - a catalogue
   cached from before the upload will happily write old sector maps
   over your new image.

Adding a *new* image file (rather than replacing one in use) is safe
at any time; the Beeb just cannot see it until the disc/catalogue is
re-read.

## Notes

- Both shipped ROMs are BBC Model B sideways-RAM builds; they need
  writable sideways RAM (see [Loading ROMs](helpers-and-roms.md)).
- MMFS and the [hard disc emulation](hard-discs.md) coexist happily:
  DFS software from MMFS images, ADFS on the hard discs.
- The MMFS project documents the full command set, MMB management and
  the differences between MMFS and MMFS2 in detail:
  <https://github.com/hoglet67/MMFS>.
