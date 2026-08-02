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
removing the card - but do it carefully. The web server, WebDAV and
USB/MTP all refuse to overwrite, delete or move a file the Beeb
currently holds open through the FAT service (the same protection the
hard disc images have), so the worst accidents are blocked. `BEEB.MMB`
gets extra protection: MMFS reads it by raw sector number, not through
a file handle, so once MMFS has touched the card the MMB is treated as
in use - replacing it mid-session would corrupt it even with nothing
formally "open".

The Pi releases these locks when MMFS lets go: it drops all of them on
a filing-system re-mount, on a **Beeb reset** (BREAK restarts the
firmware's services and clears the tracking), or on a Pi reboot. The
catch is that MMFS *also* restarts on BREAK and re-reads `BEEB.MMB` the
moment you next touch the card from the Beeb, which re-locks it. So the
rule is: leave the filing system down while you upload.

The procedure that works:

1. **On the Beeb, finish with the disc first.** Close any open files
   (`*CLOSE`), then press CTRL-BREAK and select another filing system
   (`*TAPE` is always there). BREAK clears the Pi's lock; selecting
   another filing system keeps MMFS from immediately re-taking it.
2. Upload or replace the image / `BEEB.MMB` now, while MMFS is down.
3. **Then go back into MMFS** (`*MMFS`, re-mount the disc). It re-reads
   everything from the card - which is exactly what you want, because a
   catalogue cached from before the upload would otherwise write old
   sector maps over your new image.

Adding a *new* image file (rather than replacing one in use) is safe
at any time; the Beeb just cannot see it until the disc/catalogue is
re-read.

If an upload keeps being refused with "in use by the Beeb" and nothing
on the Beeb seems to be using the file, MMFS is still holding it (it
re-locks on any card access). Drop to another filing system as in step 1
and try again; a Pi reboot from the web interface clears everything as a
last resort.

## Notes

- Both shipped ROMs are BBC Model B sideways-RAM builds; they need
  writable sideways RAM (see [Loading ROMs](helpers-and-roms.md)).
- MMFS and the [hard disc emulation](hard-discs.md) coexist happily:
  DFS software from MMFS images, ADFS on the hard discs.
- The MMFS project documents the full command set, MMB management and
  the differences between MMFS and MMFS2 in detail:
  <https://github.com/hoglet67/MMFS>.
