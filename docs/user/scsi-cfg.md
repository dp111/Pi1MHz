# SCSI Drive Descriptors (`scsi.cfg` / `defscsi.cfg`)

Each emulated hard-disc drive has a small text file that gives it its
**identity and low-level SCSI parameters** - the INQUIRY strings the Beeb
sees, the block size, and the SCSI "mode pages" a period drive would
report. It replaces the old `.dsc` geometry descriptor files.

**Most people never touch this.** The shipped default is a 10 MB drive
with the geometry of a Rodime RO652 (306 cylinders, 4 heads) at 33 sectors
per track, the shape Adaptec ACB-4000 and Acorn SuperForm software expects.
A real RO652 (the drive in an Acorn FileStore E20) has 61 sectors per track
and is 21 MB; formatting tools set that through MODE SELECT page 3. You only need this page if you want to
make a drive *identify* as something specific, change its title, or adjust
its geometry. How much of it a host sees depends on the controller set by
`DriveController=` in `Pi1MHz.cfg`: the default (`Mixed`) and `SCSI` answer
the INQUIRY data and mode pages; behind an Adaptec ACB-4000 only the geometry,
block size and title matter.

## Where the files live, and which wins

| File | Scope |
|---|---|
| `/Pi1MHz/defscsi.cfg` | The **template / default** for every drive. |
| `/BeebSCSIn/scsiN.cfg` | Per-drive override (disc set `n`, drive `N` = 0-7). |

When a drive is first created (for example when you format a new one),
Pi1MHz copies `defscsi.cfg` to that drive's own `scsiN.cfg`. From then on
the drive uses **its own** `scsiN.cfg`. So:

- Edit **`defscsi.cfg`** to change the defaults for drives you make in
  future.
- Edit a drive's own **`scsiN.cfg`** to change just that one drive.

VFS / Domesday volumes (`/BeebVFSn`) are read-only and do not get writable
`.cfg` files.

## File format

- Plain text, one `Key=value` per line. Maximum line length 256 characters.
- `#` starts a comment (whole line or after a value).
- Two kinds of value:
  - **Text** - `Title`, `Description`, `LDUserCode`.
  - **Hex byte string** - everything else: two hex digits per byte, no
    spaces, e.g. `LBADescriptor=0000000000000100`. These are the *raw SCSI
    response bytes* the drive reports, so their length matters (each key
    below lists its byte count).

## The keys

### Identity

| Key | Type | Max | Meaning |
|---|---|---|---|
| `Title` | text | 39 | Drive title, shown by disc tools. |
| `Description` | text | 255 | Free-text description. |

### INQUIRY data

| Key | Bytes | Meaning |
|---|---|---|
| `Inquiry` | up to 101 | The full SCSI **INQUIRY** response: device type, vendor/product identification (`BEEBSCSI` / `GENERIC HD`), revision, version descriptors and the copyright notice. Only the bytes given are sent (cut to the host's allocation length, never padded), as a drive stops when its data ends. |

The shipped `defscsi.cfg` carries a byte-by-byte map of this field in its
comments - the easiest way to change the vendor/product strings is to edit
those ASCII bytes in place.

### Capacity and mode-sense header

| Key | Bytes | Meaning |
|---|---|---|
| `ModeParamHeader` | 4 | SCSI MODE SENSE (6) parameter header. The mode-data-length byte is recalculated by the firmware. |
| `LBADescriptor` | 8 | Block descriptor. The **block size** is the last three bytes (`…000100` = 256 bytes, the ADFS standard), 128 to 2048 bytes. A size outside that is used as 256 (READ CAPACITY, MODE SENSE and the data all use 256); the `.cfg` itself is not changed. A `.dsc` block size is treated the same way. The VFS / Domesday volumes always use 256. |

The drive's **total capacity is taken from the size of its `.dat` image
file**, not from this descriptor - so to make a bigger drive you make a
bigger `.dat`, you do not edit a block count here.

### Mode pages

Each `ModePageNN` key is a raw SCSI **mode page**, and the first byte of
its value is the page code (so `ModePage4` starts `04…`). Standard SCSI
meanings:

| Key | Bytes | SCSI mode page |
|---|---|---|
| `ModePage0` | 10 | Vendor-unique / unit attention parameters |
| `ModePage1` | 5 | Read/Write Error Recovery |
| `ModePage3` | 23 | Format Device |
| `ModePage4` | 6 | Rigid Disc Drive Geometry (cylinders / heads - mostly cosmetic to modern software) |
| `MaskPage1`, `3`, `4`, `32` ... `38` | as the page | Which bits of that page a MODE SELECT may change (see below); `WritPage1` and `WritPage3` are the old names, still read when there is no `MaskPage` line |
| `FDefPage1`, `3`, `4`, `32` ... `38` | as the page | The page's factory default values |
| `ModePage32` | up to 32 | Vendor specific (serial number) |
| `ModePage33` | up to 32 | Vendor specific |
| `ModePage35` | up to 32 | Vendor specific |
| `ModePage36` | up to 32 | Vendor specific |
| `ModePage37` | up to 32 | Vendor specific - includes `(C)A` |
| `ModePage38` | up to 32 | Vendor specific - includes `corn`, i.e. the Acorn copyright the FileStore ROM checks for |

**Write masks** (`MaskPageNN`). Without one, a MODE SELECT replaces the page it names. With one, each bit set
in the mask may be changed by the host and each bit clear keeps its stored value, byte for byte, counting the page code
and length bytes. `MaskPage1=0000000000` makes page 1 read-only, which is what the Acorn FileStore formatter needs: it
sends page 1 back with its last byte zero, which would otherwise change the error-correction span the formatter
checks for. The MODE SELECT still answers GOOD.

**Current, changeable and factory values.** A host can ask MODE SENSE for three kinds of values. The current values are
the `ModePageNN` lines (a MODE SELECT updates them). The changeable values are the `MaskPageNN` mask; a page without one
reports a full-length all-zero mask (nothing changeable), with the page code and length the page's own. The factory
values are the `FDefPageNN` lines; a page without one reports its current values. Saved values are the current
values, since the `.cfg` is where they are kept.

**`PageControl`** says how a host asks for them:

| Value | MODE SENSE | Drives |
|---|---|---|
| `SCSI` (or no line) | page control 1 = changeable, 2 = factory, 3 = saved; page 3F = every page | CCS rev 4 and later, SCSI-2 |
| `CCS` | page control 1-3 refused; page 3D = every page's factory values, 3E = every page's mask, 3F = every page's current values | early CCS drives with no page control field, e.g. the Rodime RO652 (firmware 5.46, measured) |

A page the drive does not hold is refused at once (CHECK CONDITION, sense 24h, invalid argument). Page 0 is never part
of an all-pages reply.

**Block descriptor.** It is sent only when byte 3 of `ModeParamHeader` (the descriptor length) asks for it: a drive that
sends none, such as the RO652, has `ModeParamHeader=00000000`. `LBADescriptor` still sets the block size either way.

Each page's three lines sit together, in the order `FDefPageNN`, `MaskPageNN`, `ModePageNN`, above the
comment that describes the page's bytes. All three hold the whole page as the drive sends it, page code and
length bytes included, so a line can be compared directly with what MODE SENSE returns; in a mask those two
bytes are ignored (the page's own are used).

You can add or remove `ModePageNN` lines; they must keep the
`ModePageNN` naming so Pi1MHz (and BeebEm) pick them up.

**Which of this a host sees depends on `DriveController=`** in `Pi1MHz.cfg`
(see [Configuration](configuration.md#drive-controllers)). Under the default,
`Mixed`, the pages work as they always have: MODE SENSE answers them, MODE
SELECT merges them in (a 22-byte list with a block descriptor is the
Adaptec's drive parameter list, `ModePage0`), and they take effect at once.
Behind an Adaptec board (`ACB-4000` or `ACB-4070`) there are no mode pages and
no INQUIRY: MODE SENSE returns the Adaptec's flat list - `ModeParamHeader`
bytes 1-2 with byte 3 = 8, the 8-byte `LBADescriptor` and the 10-byte
`ModePage0` - whatever page is asked for, and MODE SELECT takes only that list
(22 bytes, or 12 with no drive parameters; density 0, block size 256, 512 or
1024, cylinders 1-2048, heads 1-16); its block size is laid down at the next
FORMAT.

### MODE SELECT on a SCSI drive

With `DriveController=SCSI` a drive takes MODE SELECT as the Rodime RO652's
firmware (ROM 5.02) does:

- **Header.** A drive with no block descriptor (`ModeParamHeader` byte 3 =
  0, as the RO652) wants an all-zero 4-byte header: anything in bytes 1-3 is
  refused (CHECK CONDITION, sense key 5, ASC 22h). A drive with one takes an
  8-byte block descriptor; its block length is laid down at the next FORMAT.
- **Pages.** A page code with bit 6 set, a page the drive's `.cfg` does not
  hold, or a page sent longer than the drive's own (its length byte) is
  refused (ASC 26h). Page code bit 7 with a page length of 0 puts the page back
  to its factory values (`FDefPageNN`). Otherwise the page is merged in under
  its `MaskPageNN`: the mask decides which bytes count, as on the drive, and a
  page cut short keeps the rest (FServFmt sends its pages two bytes short:
  `len+4` where a whole page needs `len+6`). A page with no mask line takes
  the bytes as sent, except that a zero in a numeric field of page 3 or 4
  keeps the drive's value (the formatter zeroes the read-only fields). The
  stored page keeps its own page code byte, so its PS bit stays set.
- **Pages 3 and 4 wait for FORMAT.** A new block size, sectors per track,
  cylinders or heads shows in MODE SENSE and READ CAPACITY only after the
  next FORMAT (the RO652 keeps them on the disc meanwhile; Pi1MHz keeps them
  in memory, so a BREAK, a jukebox or a card change between the MODE SELECT
  and the FORMAT drops them). They are checked
  first: a field the mask lets the host change must fit in the mask (the
  RO652's bytes-per-sector mask `07FF` refuses 2048 and 4096), and the block
  size must be 128-2048 bytes; otherwise ASC 22h. Other pages take effect at
  once.
- The whole list is checked before any of it is used.
- **Page 0** is an ordinary page on a SCSI drive (the RO650 spec's Unit
  Attention page: a real RO3085S holds `80 02 00 00`, an RO652 has none and
  refuses it). The geometry comes from page 4. On the Adaptec and SASI
  boards `ModePage0` is the drive parameter list instead.

### Block size

The block size is the last three bytes of `LBADescriptor`, and bytes 12-13
of `ModePage3` on a SCSI drive. READ, WRITE, VERIFY, READ CAPACITY and the
image size all use it: an image holds cylinders x heads x sectors per track
x block size bytes, block n at byte n x block size. It is set the way the
controller sets it - the Adaptec's MODE SELECT list (256, 512 or 1024), or a
SCSI drive's page 3 or block descriptor (128-2048); either from the next
FORMAT. The VFS / Domesday volumes always use 256-byte blocks.

### Sectors per track

Bytes 10-11 of `ModePage3` are the **sectors per track** (33, `0021`, by
default). FORMAT sets them as the controller does, and saves them here, so
the disc keeps them (a `.cfg` without a `ModePage3` line keeps the default
33):

| `DriveController=` | Sectors per track at 256 / 512 / 1024 bytes |
|---|---|
| `Mixed` | 32 (interleave 1) or 33 if the `.cfg` has 32 or 33; any other value is kept |
| `ACB-4000` | 32 (interleave 1) or 33, 17 or 18, 9 - if the `.cfg` has one of these for its block size (or 32/33); a value set by hand (17 or 63 at 256 bytes, say) is kept |
| `ACB-4070` | 46 or 47, 25 or 26, 14 - the same rule |
| `SASI` | as the `.cfg` says |
| `SCSI` | from `TrackFormat=` below; without one, as the `.cfg` says |

FORMAT's interleave 0 means 2. SuperForm's interleave keeps 33. At 32 sectors per track the largest ADFS disc (1024 cylinders by 64
heads) uses every sector ADFS can address. A SCSI drive also keeps the
interleave in `ModePage3` bytes 14-15.

### `TrackFormat` (SCSI drives)

| Key | Type | Meaning |
|---|---|---|
| `TrackFormat` | text `T,O,S` | The drive's track layout, three decimal numbers: track bytes, overhead, spare sectors. Used by FORMAT under `DriveController=SCSI`. |

- physical sectors per track = track bytes / (bytes per sector + overhead),
  rounded down; sectors per track = physical - spare sectors.
- **track bytes** (20384 on the RO652): usable bytes on one track - the drive
  has 20,484 raw bytes per revolution; the firmware keeps the gap at the index
  pulse and a margin for speed variation.
- **overhead** (70): bytes each sector takes on the disc besides its data -
  sync field, address mark, ID field (cylinder, head, sector, check bytes),
  gap, data sync and address mark, the 4-byte ECC, closing gap.
- **spare sectors** (1): kept in reserve on every track so a bad sector can be
  remapped on the same track; formatted but never given a block number.
- Example, `TrackFormat=20384,70,1` (Rodime RO652): 128/256/512/1024 bytes ->
  101/61/34/17 sectors per track; at 256, 20384/326 = 62 physical, 61 used,
  306 x 4 x 61 = 74,664 blocks.
- No `TrackFormat` line: sectors per track stay as page 3 says.

The interleave must be below the physical sectors per track - 1 (61 at 256
bytes on the RO652), else FORMAT is refused (ASC 24h).

### VFS / LaserDisc (Domesday)

| Key | Type | Range | Meaning |
|---|---|---|---|
| `LDUserCode` | text | 5 chars | LaserDisc user code for VFS/Domesday volumes, including the `=` (e.g. `1=986`). |
| `LDVideoXoffset` | integer | -768 … 768 | Horizontal alignment of the Domesday video on the Beeb's picture, in Beeb MODE 0 pixels (1/640 of the line); positive moves the video right. Absent = `-5`. |
| `LDVideoYoffset` | integer | -288 … 288 | Vertical alignment, in Beeb rows (1/256 of the screen); positive moves the video down. Absent = `-2`. Both are read whenever the side is opened or jukeboxed to, so they are per disc side. Where the video already fills the screen in that direction (the height on every mode, the width on a 4:3 or 5:4 monitor) the picture is moved inside its own overscan, in steps of two Beeb pixels horizontally, and by nothing at all at 576p where there is no overscan. |

## Editing safely

- Keep each of the other hex values the **exact byte length** shown above
  - the parser rejects the wrong length, and a broken descriptor makes the
  drive fail to start. The vendor pages (`ModePage32` to `ModePage38`) may be
  any length up to 32 bytes, as real drives differ (a Rodime RO3000S's pages
  37 and 38 are 8 bytes each: `2506284329410D00` and `2606636F726E0D00`); give
  each the length its own second byte says.
- The shipped `/Pi1MHz/defscsi.cfg` is the best starting point: it
  documents every byte of the INQUIRY data, the mode header and each mode
  page inline. Copy it and edit against those comments.
- After changing a drive's `scsiN.cfg`, restart ADFS on the Beeb
  (`*BYE` then re-access, or CTRL-BREAK) so it re-reads the drive.

## Reference documents

The parameters above are period SCSI/SASI drive data. The specifications
`defscsi.cfg` cites:

- Adaptec ACB-4000 User's Manual
- ANSI X3.131-1986 (SCSI-1)
- SASI Design Specification
- Rodime RO650 series product specification
- Seagate SCSI Command Reference Manual

See also [Hard discs (ADFS / BeebSCSI)](hard-discs.md) for the disc images
these descriptors belong to.
