# SCSI Drive Descriptors (`scsi.cfg` / `defscsi.cfg`)

Each emulated hard-disc drive has a small text file that gives it its
**identity and low-level SCSI parameters** - the INQUIRY strings the Beeb
sees, the block size, and the SCSI "mode pages" a period drive would
report. It replaces the old `.dsc` geometry descriptor files.

**Most people never touch this.** The shipped default emulates a 21 MB
Rodime RO652 (the drive in an Acorn FileStore E20), which is what almost
all BeebSCSI/ADFS software expects. You only need this page if you want to
make a drive *identify* as something specific, change its title, or adjust
its geometry.

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
| `Inquiry` | up to 101 | The full SCSI **INQUIRY** response: device type, vendor/product identification (`BEEBSCSI` / `GENERIC HD`), revision, version descriptors and the copyright notice. |

The shipped `defscsi.cfg` carries a byte-by-byte map of this field in its
comments - the easiest way to change the vendor/product strings is to edit
those ASCII bytes in place.

### Capacity and mode-sense header

| Key | Bytes | Meaning |
|---|---|---|
| `ModeParamHeader` | 4 | SCSI MODE SENSE (6) parameter header. The mode-data-length byte is recalculated by the firmware. |
| `LBADescriptor` | 8 | Block descriptor. The **block size** is the last three bytes (`…000100` = 256 bytes, the ADFS standard). |

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
| `WritPage3` | 23 | The Format-Device bytes returned to a MODE SELECT (write) - the drive can report different values when written vs read. |
| `ModePage4` | 6 | Rigid Disc Drive Geometry (cylinders / heads - mostly cosmetic to modern software) |
| `ModePage32` | 10 | Vendor specific (serial number) |
| `ModePage33` | 9 | Vendor specific |
| `ModePage35` | 3 | Vendor specific |
| `ModePage36` | 4 | Vendor specific |
| `ModePage37` | 6 | Vendor specific - includes `(C)A` |
| `ModePage38` | 6 | Vendor specific - includes `corn`, i.e. the Acorn copyright the FileStore ROM checks for |

You can add or remove `ModePageNN` lines; they must keep the
`ModePageNN` naming so Pi1MHz (and BeebEm) pick them up.

### VFS / LaserDisc (Domesday)

| Key | Type | Range | Meaning |
|---|---|---|---|
| `LDUserCode` | text | 5 chars | LaserDisc user code for VFS/Domesday volumes, including the `=` (e.g. `1=986`). |
| `LDVideoXoffset` | integer | -768 … 768 | Horizontal offset of the Domesday video overlay. |

## Editing safely

- Keep each hex value the **exact byte length** shown above - the parser
  rejects the wrong length, and a broken descriptor makes the drive fail
  to start.
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
