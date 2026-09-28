# The 1MHz-WiFi ROM and WiCFS

Helper 16 loads a sideways ROM that puts the Pi's WiFi at the Beeb's
command line: join a network, fetch files from the web with `*WGET`, and
run cassette games straight from UEF files with **WiCFS**, the UEF cassette
filing system from Roland Leurs' ElkWiFi. One image works on the BBC B,
B+, Master 128 and the Electron (through a Plus 5's 1MHz connector).

## What you need

- WiFi set up on the Pi - see [WiFi setup](wifi.md).
- In `/Pi1MHz/Pi1MHz.cfg`:

  ```
  wifi_service_enable=1
  net_enable=1
  ```

  `wifi_service_enable` answers the WiFi commands (`*JOIN`, `*LAP`,
  `*PING`, `*DATE` ...); `net_enable` is needed as well for `*WGET`,
  `*NSLOOK` and `*DISCONNECT`. Without them a command waits and then
  reports "No response from device" or a `Network error`. `*HELP WIFI`
  and `*VERSION` always work: the ROM answers those itself.
- Free sideways RAM. The ROM keeps its workspace inside its own bank, so it
  must run from sideways RAM, not an EPROM.

## Loading it

```
X%=16:CALL &FC88
```

(or `*FX147,136,16` then `*GO FD00`), then CTRL-BREAK. `*HELP WIFI` lists
the commands, four to a line; `*VERSION` shows the ROM's and the Pi's
versions. See [Helpers and ROMs](helpers-and-roms.md).

## Network commands

| Command | What it does |
|---|---|
| `*JOIN <ssid> [password]` | Join a network. Leave out the password to be asked for it (typed as `*`). The Pi saves the network in `/Pi1MHz/WiFi.profile`. `*JOIN ?` shows the network in use. |
| `*LEAVE` | Leave the network. |
| `*LAP` | List the access points in range. |
| `*LAPOPT 7` / `*LAPOPT 127` | What `*LAP` shows: 7 = security, name and signal; 127 (the default) adds the BSSID and channel. |
| `*IFCFG` | Show the IP address and MAC address. |
| `*ONLINE` | One line on whether the network is ready, e.g. `OFFLINE WIFI OFF`. |
| `*WIFI ON` / `*WIFI OFF` | Turn the radio on or off. |
| `*MODE 1` / `*MODE ?` | Station mode, the only mode there is; `?` shows it. |
| `*PING <host>` | Five pings to a host name or IP address. ESCAPE stops it. |
| `*NSLOOK <host>` | Look up a host name's IPv4 address. |
| `*DATE`, `*TIME` | The date and time from the internet (UTC), shifted by `wifi_service_utc_offset_minutes` in `Pi1MHz.cfg` - e.g. `60` for British Summer Time. On a Master, `*TIME` is the MOS's own command and reads the Master's battery clock instead; `*DATE` still comes from here. |
| `*DISCONNECT` | Close the ROM's raw network connection. |

Most of the time none of these are needed: the Pi joins the network set in
`Pi1MHz.cfg` by itself when it boots.

## Fetching files: *WGET

```
*WGET <url> <file>            save to a file on the current filing system
*WGET -T <url>                show it on the screen as text
*WGET -X <url>                the same, for Unix text (LF line ends)
*WGET -U <url>                load it into the Pi's JIM RAM, for WiCFS
*WGET -S <url> <bank>         load a 16K ROM image into sideways RAM bank <bank>
```

- Options go **before** the URL. `*WGET <url> -U` takes `-U` as a file
  name and fails with "Bad filing system name".
- `http://` and `https://` both work; `https://` needs `/cacert.pem` on the
  SD card - see [FujiNet](fujinet.md#https).
- `<file>` can be any name the current filing system accepts, up to 63
  characters: `*WGET http://example.com/prog.bas PROG` on DFS, or a path on
  ADFS.
- `-U` expands `.gz` and `.zip` downloads on the Pi and reports
  `WGET GZIP OK &nnnn bytes in JIM` (or `ZIP` / `RAW`). The limit is &FFFE
  bytes after expanding.
- `-S` takes the bank number in hex (`*WGET -S http://host/rom.bin 5`). The
  bank must be sideways RAM and not the one this ROM is in; press
  CTRL-BREAK afterwards so the machine sees the new ROM.
- ESCAPE stops a download. Errors come back as `Network error &xx`, followed
  by `HTTP status &nnnn` when the server refused. A download that stalls
  for about 50 seconds ends with `Network timeout`.
- Large files take a while: the data crosses the 1MHz bus a few hundred
  bytes at a time.

## Playing tape games: WiCFS

WiCFS makes a UEF file - the usual format for BBC and Electron cassette
images - look like a tape in the cassette deck, so `*CAT`, `*LOAD`,
`CHAIN""` and `*RUN""` read it. The UEF lives in the Pi's JIM RAM, not in
the Beeb.

**From the web:**

```
*WGET -U http://example.com/games/elite.uef
*WICFS
CHAIN""
```

`*WICFS` switches to the tape filing system (`*TAPE`), sets `PAGE=&E00`,
types `NEW` and connects the tape to the UEF in JIM - so it clears any BASIC
program in memory. Then use it like a tape: `*CAT` to list it, `CHAIN""`
or `*RUN""` for the next program. `*REWIND` goes back to the start.

**From a disc:** `*UEF LOAD <file>` reads a UEF (or `.gz` / `.zip`) from the
current filing system - DFS, ADFS, MMFS - into JIM and then runs its first
program by itself, `CHAIN""` for BASIC and `*RUN""` for anything else:

```
*UEF LOAD ELITE
```

With a second processor active, `*UEF` switches to the host's BASIC first
so the game runs in the Beeb itself.

Errors you may see: `Invalid UEF, gzip or ZIP file`, `Buffer full` (the
expanded UEF is over &FFFE bytes), `UEF file not found`. `WiCFS state
invalid; power cycle` means its saved state is damaged - switch the Beeb
off and on.

`*QUPRUN` (`*QR`), `*QAUTO`, `*QHOST` and `*QUPCFS` also appear in
`*HELP WIFI`: they are steps `*UEF` and `*WICFS` type for you, not commands
to use yourself.

## The RAM disc

A small RAM disc in the Pi's JIM RAM: up to 15 files, 65,024 bytes, names
up to 7 characters, addresses in hex.

| Command | What it does |
|---|---|
| `*RDINIT` | Create or clear the RAM disc. Nothing else frees space. |
| `*RDCAT` | List it, with the pages used. |
| `*RDSAVE <name> <start> <end> [exec]` | Save memory from `start` up to (not including) `end`. |
| `*RDLOAD <name> [addr]` | Load a file, at `addr` if given. |
| `*RDRUN <name>` | Load a file and run it. |

The RAM disc shares its JIM RAM with `*WGET -U`, `*WGET -S` and `*UEF LOAD`:
any of those wipes it (`*RDCAT` then says `No RAM disk; use *RDINIT`). Use it
for scratch files, not for anything you want to keep.

## Things to know

- If a `*WGET <url> <file>` fails because the filing system refused the
  file (a bad file name, for example), the web connection stays open and the
  next `*NSLOOK` reports `Network error &21`. `*DISCONNECT` - or any
  successful `*WGET` - closes it.

- Using the RAM disc, WiCFS or `*WGET -U`/`-S` pages the JIM window, so do
  not run the [SD card explorer](sd-explorer.md) with WiCFS as the current
  filing system.
- The ROM builds its error messages at &0100, the bottom of the 6502 stack.
  Programs that keep data there can see it changed after an error.
- The image is **not** under Pi1MHz's GPL-3.0 licence: WiCFS derives from
  Roland Leurs' ElkWiFi and Martin Barr's UPCFS and carries Roland's
  non-commercial licence - see `CREDITS.md`. The WiFi half, by Peter Clarke,
  is in `beeb/1mhz-wifi/`.
