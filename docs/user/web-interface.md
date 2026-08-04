# The Web Interface and WebDAV

With [WiFi](wifi.md) set up, Pi1MHz runs a small web server. Point a
browser at the Pi - `http://pi1mhz.local/`, `http://Pi1MHz/` or its IP
address - and you get a home page linking to everything below.

## Pages

| Address | What it does |
|---|---|
| `/` | Home page with links |
| `/status` | WiFi and network details (network name, addresses, signal strength, link rate, traffic counters) and SD card free space |
| `/files/` | Browse the SD card: download files, upload files, create and delete |
| `/framebuffer` | A live snapshot of the Pi's HDMI screen (see [Screen and video](screen-and-video.md)); refresh the page for a new one |
| `/framebuffer.bmp` | The same snapshot as a plain BMP image you can save |
| `/reboot` | Reboot the Pi (asks for confirmation first). The BBC does not need to be switched off, but anything using Pi1MHz will pause while it restarts |
| `/aun` | Diagnostic counters for [Econet over WiFi](econet-aun.md) |
| `/bench.bin` | A dummy large download for testing your network speed to the Pi |

Any other address is treated as a path on the SD card, so
`http://pi1mhz.local/BeebSCSI0/scsi0.dat` downloads that file
directly. Downloads support HTTP Range requests (`curl -r`,
download-manager resume, media seeking), which is also what makes the
disc image viewer below fast.

## Looking inside disc images

The file browser shows a **[view contents]** link next to Acorn disc
images: `.ssd` and `.dsd` (DFS floppies), `.mmb` (MMFS bundles),
`.adf`/`.adm`/`.adl` (ADFS old-map) and `scsi*.dat` (BeebSCSI
hard-disc images). It opens `/Pi1MHz/disc.html`, a viewer that runs
entirely in your browser:

- list a disc's catalogue - files, load/exec addresses, sizes, locked
  flags, boot option; for an MMB, the whole slot list, and each
  formatted slot's catalogue a click deeper; for a hard disc, walk the
  ADFS directory tree
- download an individual file straight out of the image (with an
  optional `.inf` sidecar carrying its addresses), view it as text -
  tokenised BBC BASIC programs are shown as a proper LISTing - or peek
  at it as a hex dump
- export a whole catalogue (a DFS disc, an MMB slot, or an ADFS folder
  tree) as a `.zip` of the files with their `.inf` sidecars
- download one MMB slot as a standalone `.ssd`, or save one side of a
  `.dsd` as a plain `.ssd`

Because the browser fetches only the byte ranges it needs, listing the
catalogue of even a 500 MB `scsi.dat` moves a few hundred bytes over
the WiFi, and extracting a file costs just that file's size. The
viewer is read-only - to change an image, download it, edit it with
your usual tools, and upload it back.

The page itself lives on the SD card at `/Pi1MHz/disc.html`, so it can
be updated by copying a new file there - no reflash needed. You can
also open it directly and type any image path into its form.

Uploading through `/files/` is the everyday way to get a disc image or
ROM onto the card without pulling it out of the Pi. The server refuses
to overwrite, delete or move any file the Beeb currently has open -
a started hard-disc image, or an [MMFS](mmfs.md) disc image / `BEEB.MMB`
open through the FAT service. Release it on the Beeb first (`*BYE` for a
hard disc; for MMFS see [that page](mmfs.md#changing-images-from-another-computer)).

## WebDAV: the SD card as a network drive

The same server speaks WebDAV, so you can mount the SD card as a
folder on your computer and drag files around normally. The share is
the root of the SD card.

- **Windows**: File Explorer → This PC → right-click → "Add a network
  location" → `http://Pi1MHz/`
- **macOS**: Finder → Go → Connect to Server (Cmd-K) →
  `http://pi1mhz.local/`
- **Linux**: `sudo mount.davfs http://pi1mhz.local/ /mnt` (or use your
  file manager's "connect to server")
- **iOS/iPadOS**: Files app → "Connect to Server"

Copying, renaming, deleting and creating folders all work, including
deleting a whole folder tree in one operation. Two limitations to know
about: copying a **whole folder tree** in one operation is not
supported by the server (your computer's file manager usually walks the
tree itself, in which case it works anyway), and transfers are plain
unencrypted HTTP.

File date-stamps shown over WebDAV are in UTC unless you set your
timezone, e.g. `webdav_utc_offset_minutes=60` in `Pi1MHz.cfg`.

## Password protection

Set both `webdav_user=` and `webdav_password=` in `Pi1MHz.cfg` to
require a login on every page and WebDAV operation. With either one
missing, the server is open to anyone on your network. See
[WiFi setup](wifi.md#password-protection).

## Speed expectations

This is a Pi Zero doing WiFi in software; expect file transfers of
very roughly a couple of megabytes per second. Fine for disc images
and ROMs, slow for gigabytes.
