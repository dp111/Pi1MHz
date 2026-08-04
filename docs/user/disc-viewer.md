# The Disc Image Viewer and Editor

Pi1MHz can look inside Acorn disc images - and edit DFS ones - straight
from a web browser, without downloading the image or pulling the SD
card. The viewer is a single self-contained page that runs entirely in
your browser: the Pi only serves the exact bytes asked for, so listing
the catalogue of even a 500 MB hard-disc image moves a few hundred
bytes over the WiFi, and most operations feel instant despite the
Pi Zero's modest network speed.

## Opening it

In the [file browser](web-interface.md) (`/files/`), every recognised
disc image has a **[view contents]** link next to it. You can also open
`http://pi1mhz.local/Pi1MHz/disc.html` directly and type an SD-card
path into its form.

Recognised images:

| Extension | Format |
|---|---|
| `.ssd` | Acorn DFS, single sided |
| `.dsd` | Acorn DFS, double sided (both sides shown) |
| `.mmb` | [MMFS](mmfs.md) disc bundle - the whole slot list, and each formatted disc a click deeper |
| `scsi*.dat` | [BeebSCSI hard-disc image](hard-discs.md) (ADFS) - walk the directory tree |
| `.adf` / `.adm` / `.adl` | ADFS floppy images (old map) |

Any **other** file opens as a raw hex dump instead - see
[hex viewer](#the-hex-viewer) below.

## Browsing and extracting

A DFS catalogue shows its title, size, boot option and files with
their load/exec addresses, lengths and locked flags; an ADFS view
walks directories with a breadcrumb trail. Next to every file:

- **download** - the file itself, extracted from the image
- **.inf** - a sidecar file in the standard archive format
  (`$.NAME load exec length [L]`, addresses in hex) that emulators
  and archive tools use to carry the Beeb attributes
- **text** - view as text; a tokenised BBC BASIC program is
  automatically detokenised into a proper LISTing
- **hex** - a hex/ASCII dump
- **asm** - a 65C02 disassembly starting at the file's load address
  (the full CMOS instruction set including the Rockwell bit
  instructions; bytes that aren't valid opcodes appear as `EQUB`)

Whole-catalogue exports:

- **download all (.zip)** on a DFS disc or MMB slot, and
  **[download folder as .zip]** on an ADFS directory, pack every file
  plus its `.inf` sidecar into a zip (ADFS folders recursively)
- **download .ssd** saves one MMB slot as a standalone disc image
- **this side as .ssd** saves one side of a `.dsd` as a single-sided
  image, de-interleaved

## Editing DFS discs

With current firmware the viewer can change DFS images in place.
Each operation patches only the affected bytes, so adding a file to a
disc inside a 100 MB MMB takes a fraction of a second:

- **Add a file** to a `.ssd`, a `.dsd` side, or a disc inside an
  `.mmb`: choose the file in the "Add a file" form - optionally
  together with its `.inf` sidecar, which fills in the name and
  addresses automatically - and adjust the DFS name (`DIR.NAME`, up
  to 7 characters), load/exec addresses and locked flag.
- **delete** removes a file from the catalogue (its sectors become
  free space; replace a file by deleting it first).
- **insert… / replace…** on an MMB slot writes a whole disc into the
  bundle from a local `.ssd` - or from either side of a local `.dsd`,
  de-interleaved automatically - with a slot name of your choice.
  Replacing asks for confirmation, because the old contents are
  overwritten as the new ones arrive.

What the editor protects you from:

- **The Beeb comes first.** Writes are refused - before and
  continuously during the transfer - while the Beeb has the image
  open (a started hard disc, or MMFS's `BEEB.MMB`). Release it with
  `*BYE`, or CTRL-BREAK out of MMFS, and retry.
- **Interruptions are safe by ordering.** File data is written before
  the catalogue entry that points at it, and an MMB slot is marked
  unformatted before its data is replaced and only marked usable
  afterwards - so a dropped connection leaves an unformatted slot or
  an unchanged catalogue, never a valid-looking half-written disc.
- **Concurrent edits are caught.** If the disc changed since the page
  read it (another browser tab, for instance), the edit is refused
  with a "reload and try again" message rather than applied blindly.
- **Watford 62-file discs are refused.** Their extended catalogue
  lives in sectors a standard catalogue edit would hand out as free
  space, so editing them here would corrupt them.
- **Truncated images are detected.** Images trimmed short of what
  their catalogue claims get a clear message instead of a confusing
  error when the free space doesn't physically exist.

ADFS images (`scsi*.dat`, `.adf`, `.adm`, `.adl`) are deliberately
**read-only**: editing the ADFS free-space map carries a real risk of
corrupting a hard-disc image, so it is not offered. To change one,
download it, edit it with your usual tools, and upload it back.

The editing controls only appear when the firmware supports in-place
writes; on older firmware the viewer is read-only (and can still
browse small images by downloading them whole).

## The hex viewer

Opening any file the viewer doesn't recognise as a disc image shows a
raw hex/ASCII dump instead, paged 64 KB at a time - each page is one
small request, so jumping around a huge file is instant. For disc
images, the **[raw hex]** link next to the title (or `&view=hex` on
the URL) forces the same raw view.

## For scripts and power users

The two HTTP features underneath the viewer are useful on their own
(both respect the [password protection](web-interface.md#password-protection)
and the Beeb-busy interlocks):

- Downloads accept single-range `Range:` headers, so
  `curl -r 512-1023 http://pi1mhz.local/discs/games.ssd` fetches two
  sectors, and interrupted downloads can resume.
- `PUT` with `?offset=N` writes the request body into an **existing**
  file at that byte offset, in place - it never creates, truncates or
  extends a file, and the write must fit inside the file's current
  size:
  `curl -T patch.bin "http://pi1mhz.local/discs/games.ssd?offset=512"`

## Updating the viewer

The page lives on the SD card at `/Pi1MHz/disc.html`, so it can be
updated by copying a newer file there - no firmware reflash needed.
The in-place write support used for editing is firmware-side, so
editing needs a current `kernel.img` too.
