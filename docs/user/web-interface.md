# The Web Interface and WebDAV

With [WiFi](wifi.md) set up, Pi1MHz runs a small web server. Point a
browser at the Pi - `http://pi1mhz.local/`, `http://Pi1MHz/` or its IP
address - and you get a home page linking to everything below. All the
pages (including the disc viewer) follow your browser or operating
system's light/dark theme preference automatically.

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
images (`.ssd`, `.dsd`, `.mmb`, `.adf`/`.adm`/`.adl`, `scsi*.dat`).
It opens a viewer that runs entirely in your browser: list catalogues,
extract files (with `.inf` sidecars), view BASIC listings, hex dumps
and 65C02 disassembly, export to `.zip` - and edit DFS discs in
place, including adding files to discs inside an MMB and inserting
whole MMB slots. It doubles as a paged hex viewer for any SD file.
See [the disc image viewer and editor](disc-viewer.md) for the full
guide.

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
