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

There are further pages for developers chasing a fault; they are listed in
`docs/dev/diagnostics.md` in the source tree.

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

## kernel.now over the network

`kernel.now` - restart into a firmware image from memory, without
changing the SD card - works over the network too, as it does over
[USB](usb-file-access.md#a-special-file-kernelnow):

- copy a firmware image to the WebDAV share's root as `kernel.now`, or
- upload it as `kernel.now` with the form on the root folder's page, or
- from a command line: `curl -T kernel7.img http://pi1mhz.local/kernel.now`
  (add `--digest -u user:password` if the server has a password).

Use `kernel7.img` on a Pi 2/3/Zero 2 and `kernel.img` on a Pi 1/Zero.
The image is held in memory, never saved; the Pi answers, then restarts
into it. If the video player is running it is shut down first. Only a
basic check is made - the image must start like an ARM kernel - so a
file that fails it is refused (422) and the Pi carries on, but the
wrong kernel for your Pi model is not caught and needs a power cycle.
A power cycle always goes back to the card's kernel. The first install
of a build with the current kernel.now layout has to go on the SD card
(see `docs/dev/chainboot-kernel-now.md`); after that kernel.now works.

kernel.now never changes the program the Pi's VideoCore runs to serve
the 1MHz bus: that is loaded only at a cold boot. A build whose
VideoCore code (`vidcore/Pi1MHzvc.s`) has changed must be copied to the
SD card as `kernel.img` and the Pi power-cycled or rebooted; under
kernel.now it runs with the old VideoCore code.

## Password protection

Set both `webdav_user=` and `webdav_password=` in `Pi1MHz.cfg` to
require a login on every page and WebDAV operation. With either one
missing, the server is open to anyone on your network. See
[WiFi setup](wifi.md#password-protection).

Setting `webdav_user=` and `webdav_password=` is the real protection.
Without them, one narrower guard still applies: a change (an upload,
delete, rename, `kernel.now` or reboot) sent by a **web browser** is
refused with 403 Forbidden unless

- the browser addressed the Pi by its IP address, its hostname or
  `hostname.local` (with or without a port) - so use one of those in
  the address bar to change files from the browser; and
- the browser says the request came from the Pi's own pages (or was
  typed in), not from a page on another web site.

This stops a web page you happen to visit from writing to the card or
restarting the Pi behind your back, including by pointing a name of its
own at the Pi's address. It does nothing about other programs on your
network: WebDAV clients, `curl` and scripts send no browser headers and
are not checked, so anyone on the network can still change files unless
a password is set. This guard never refuses reading pages or files.

## Speed expectations

This is a Pi Zero doing WiFi in software; expect file transfers of
very roughly a couple of megabytes per second. Fine for disc images
and ROMs, slow for gigabytes.
