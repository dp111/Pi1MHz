# FujiNet (fn-rom)

Pi1MHz can be the **FujiNet device** for [fn-rom](https://github.com/markjfisher/fn-rom),
the BBC Micro filing system from the FujiNet project. fn-rom normally talks to
a FujiNet (an ESP32 board) over the RS423 port at 19200 baud. With its
**1MHz build**, it talks to the Pi over the 1MHz bus instead, and the Pi
itself answers:

- **disc images** from the SD card or from a **TNFS** server, mounted into
  fn-rom's drives, so `*CAT`, `*LOAD`, `*SAVE`, `*RUN` and so on work on them;
- **network channels**: `OPENIN "http://..."`, `https://` and `tcp://`, then
  `BGET#`/`BPUT#`, including POST and PUT bodies;
- **JSON queries** on an open channel with `*FJSON`, returning just the value
  you ask for.

It speaks the same FujiBus protocol as a real FujiNet (fujinet-nio), so
programs written for fn-rom work unchanged.

## What you need

- WiFi set up and working - see [WiFi setup](wifi.md).
- `net_enable=1` in `/Pi1MHz/Pi1MHz.cfg` for anything that uses the network
  (TNFS, http, https, tcp). SD-card disc images work without it.
- fn-rom built for the 1MHz bus. Pi1MHz ships both builds in `/Pi1MHz/`:
  `fujinetB.rom` for the BBC B and `fujinetM.rom` for the Master 128. They
  are separate images because fn-rom fixes its workspace addresses (and, on
  the Master, uses 65C02 code) when it is built.
- For `https://`: the CA certificate bundle `/cacert.pem` at the root of the
  SD card (it ships with the firmware - see [HTTPS](#https) below).

Load it with **helper 18**, which picks the right build for the machine it
runs on, then press CTRL-BREAK so it claims its workspace, and select it with
`*FUJI`:

```
X%=18:CALL &FC88
```

(or `*FX147,136,18` then `*GO FD00`). A `No SWR/ROM` error means there was no
free sideways RAM, or the ROM file is missing from the SD card - see
[Helpers and ROMs](helpers-and-roms.md).

To build fn-rom yourself: `make all BUILD_INTERFACE=1MHZ` (BBC B) or
`make all BUILD_INTERFACE=1MHZ BUILD_MACHINE=MASTER` (Master 128), from a
version that includes the 1MHz link, and copy the result over the shipped
file. The shipped ROMs and utilities discs are built from github.com/dp111/fn-rom
branch `pi1mhz-ship`, which also carries fixes (OSGBPB, and loading and
saving a file whose last sector is partial) not yet in fn-rom itself.

## Disc images

fn-rom keeps a **host** (where images are found), a catalogue of **slots**,
and a mapping of slots to its **drives**. The Pi stores all three on the SD
card (under `/FujiNet/`), so they survive power-offs.

```
*FHOST SD0:/GAMES            images from the SD card's /games directory
*FHOST TNFS://192.168.1.10/  or from a TNFS server
*FIN 1 ELITE.SSD             put an image into slot 1
*FMOUNT 1 0                  mount slot 1 as drive 0
*CAT
```

- SSD images (single-sided DFS, 40 or 80 track), DSD images (double-sided
  DFS) and raw images are supported. A DSD mounted as drive 0 or 1 shows its
  second side as drive 2 or 3, as a double-sided drive does: `*CAT 2`.
- Writes (`*SAVE`, `*DELETE`, `*COPY` ...) go straight into the image, on the
  card or on the TNFS server.
- A plain BREAK keeps the mounts. CTRL-BREAK starts a new session and
  unmounts them, as a real FujiNet does.
- An image that is mounted is locked against being replaced or deleted over
  WiFi (WebDAV answers 423 Locked) until it is unmounted.
- `fujinet_boot=` in `Pi1MHz.cfg` names the image fn-rom's `*FBOOT` mounts,
  read only - normally fn-rom's utilities disc (below).

### The utilities disc

Some fn-rom commands - `*FLS`, `*FCD`, `*FSLOTS`, `*FNEW`, `*FOUT`,
`*FUMOUNT`, `*FORM`, `*COPY`, `*ACCESS`, `*RENAME`, `*TITLE`, `*WIPE`,
`*DESTROY`, `*MAP`, `*FREE` - are not in the ROM but on its utilities disc,
loaded when you type them. Pi1MHz ships the disc for each ROM in
`/FujiNet/`:

| Machine | ROM | Utilities disc |
|---|---|---|
| BBC B | `fujinetB.rom` | `FN-BOOTB.ssd` |
| Master 128 | `fujinetM.rom` | `FN-BOOTM.ssd` |

Name yours in `Pi1MHz.cfg` - on a Master, for example:

```
fujinet_boot=sd0:/FujiNet/FN-BOOTM.ssd
```

then `*FBOOT` mounts it. The disc's programs call into the ROM at fixed
addresses, so use the disc that came with your ROM: a disc from another
fn-rom build can crash. On a Master they load at &0E00, which is PAGE, so
running one wipes a BASIC program in memory - save it first.

### TNFS servers

Any TNFS server works, such as FujiNet's own `tnfsd`. If the server machine
has **two network interfaces on the same subnet** (WiFi and Ethernet, say),
use the address of the one it answers from - usually the wired one. A server
reached on its other address answers from the wrong one, and the Pi, like any
TNFS client, ignores the replies.

## Network channels

Open a URL as a file:

```
H%=OPENIN("http://example.com/data.txt")
REPEAT:PRINT CHR$BGET#H%;:UNTIL EOF#H%
CLOSE#H%
```

- `OPENIN` reads (GET). `OPENUP` sends a body with POST, `OPENOUT` with PUT;
  set the body's length first with OSWORD &78 reason &01, then `BPUT#` the
  bytes. `*OPT 6,1` sends each `BPUT#` at once instead of every 256 bytes.
  The server's reply is then read with `BGET#` on the same channel.
- `tcp://host:port` opens a raw TCP connection; `BGET#` returns 254 while no
  data has arrived yet.
- Up to five channels can be open at once.
- For more than a few bytes, read with OSGBPB rather than a `BGET#` loop: in
  BASIC each `BGET#` costs a few milliseconds whatever the filing system, and
  OSGBPB moves a block in one call - about ten times faster on a disc image.

See fn-rom's `docs/fnnet-api.md` for the OSWORD &78 calls (long URLs, body
length, content type, JSON paths).

### JSON

```
H%=OPENIN("https://api.chucknorris.io/jokes/random")
*FJSON 21 /value
REPEAT:PRINT CHR$BGET#H%;:UNTIL EOF#H%
```

`*FJSON <channel> <path>` makes the channel return only the value at the JSON
Pointer `<path>`. You can query several paths on one channel; the response
is fetched once and kept. Values come back as fujinet-nio formats them:
strings as text, numbers as numbers (`12.5`), `TRUE`/`FALSE`/`NULL`, an array
one element per line, an object as `key` and `value` lines.

## HTTPS

`https://` connections are encrypted (TLS 1.2) and the server's certificate
is checked against the certificate authorities in **`/cacert.pem`** on the SD
card, including that it was issued for the host you asked for. A server whose
certificate does not check out - self-signed, for another name, or from an
unknown authority - is refused.

- The firmware ships `cacert.pem`, Mozilla's list as published by the curl
  project. To update it, download a new copy from
  <https://curl.se/ca/cacert.pem> and copy it over the old one.
- Certificate **expiry dates are not checked**: the Pi has no clock at the
  time it connects. An expired certificate is accepted.
- Without `/cacert.pem`, every `https://` connection fails.
- The 1MHz-WiFi ROM's `*WGET` can fetch `https://` URLs too.

A failed secure connection shows as `Network error &31` from `*WGET`.

## Write protection

`Beeb_write_protect=1` (see [MMFS](mmfs.md#write-protecting-the-card-from-the-beeb))
covers FujiNet too: writes to images and fn-rom's saved settings on the SD
card are ignored and reported as successful. VFS volumes (`/BeebVFS*`) are
never written. Images on a TNFS server are not affected.

## Not supported yet

- XML and RSS translation (fujinet-nio does not support them either).
- HTTP response headers and status (fn-rom's Info calls), and request headers
  other than Content-Type.
- Request bodies of unknown length.
- fn-rom's WiFi-configuration device - Pi1MHz's WiFi is set in `Pi1MHz.cfg`.
