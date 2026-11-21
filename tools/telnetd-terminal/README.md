# A BBC Micro look for TelnetD sessions

These files make a telnet session to the Pi1MHz's telnet daemon (TelnetD, see
`docs/user/telnetd.md`) look like the Beeb's own screen. They include the BBC
Micro's character sets as fonts, the BBC's eight colours, and ready-made settings
for Windows Terminal, PuTTY and the Debian console or desktop. TelnetD works with
any terminal. These files only change how it looks.

Every setting uses the host name `Pi1MHz`. If your Pi has a different hostname,
or your network does not resolve it, use its IP address instead.

## The fonts: one for each width of the Beeb's screen

A terminal cannot change its font when the Beeb changes MODE. The Beeb's
characters change shape from mode to mode, though, so there is one font for each
width of the screen. The cells of each font have the shape that mode's characters
have on a 4:3 screen:

| Font | File | For | Window | Cell (w:h) |
|---|---|---|---|---|
| BBC Micro Teletext | `BBCMicroTeletext.ttf` | MODE 7 | 40 x 25 | 59:80 |
| BBC Micro MOS | `BBCMicroMOS.ttf` | MODE 0 and 3 | 80 x 32 | 1:2 |
| BBC Micro MOS 40 | `BBCMicroMOS40.ttf` | MODE 1, 4 and 6 | 40 x 32 | 1:1 |
| BBC Micro MOS 20 | `BBCMicroMOS20.ttf` | MODE 2 and 5 | 20 x 32 | 2:1 |

MODE 7's cell is the teletext chip's own: the SAA5050 data sheet gives a character
1 µs of the line wide and 10 TV lines (20 interlaced) high, which on a 4:3 screen is
59:80, and makes the 40 x 25 page the Beeb's 1.18:1.

The Beeb starts in MODE 7, so MODE 7 is the default everywhere. A window opened for
one mode still shows the others, in its own font.

The fonts are made from the Pi1MHz framebuffer's own character sets, so a telnet
session shows the same characters as the Pi's screen:

- The three MOS fonts are the framebuffer's BBC Micro character set
  (`src/framebuffer/fonts/bbc.fnt.h`), dot for dot. They include ASCII, the pound
  sign, and the characters TelnetD sends for MODE 7's special ones:
  `← → ↑ ½ ¼ ¾ ‖ ― ÷` and the solid block. The MOS has no glyphs for these, so
  `make-fonts.py` draws them itself.
- The teletext font is the framebuffer's teletext chip (SAA5050) character set
  (`src/framebuffer/fonts/saa5050.fnt.h`). Each 6 x 10 dot character is smoothed to
  12 x 20 pixels by the chip's "character rounding", which adds a half dot wherever
  two dots meet only at a corner, the way the framebuffer does it. As on the chip,
  it has `£ ← ½ → ↑ # ― ¼ ‖ ¾ ÷` and the block in place of some ASCII characters.

Every TrueType font also has Unicode's 60 sextant characters (U+1FB00-1FB3B) and the
full and half blocks, filling the whole cell. TelnetD uses these to draw MODE 7's
mosaic graphics, so in Windows Terminal, PuTTY or a desktop terminal the graphics
join up as on the Beeb. They are the chip's own mosaics, from the same table.

The TrueType fonts also carry the characters TelnetD's line, shell and font modes
send (made by rule from the same tables, except where noted):

- **Separated sextants** (U+1CE51-1CE8F, Unicode 16), in every font: MODE 7's
  separated graphics, the chip's own from `saa5050.fnt.h`.
- **Quadrants** (U+2580, U+2584, U+2596-259F), in every font: a character a program
  defines with VDU 23, drawn as 2 x 2 blocks when the terminal has no BBC font.
- **U+2592**, the medium shade, in every font: the marker for a graphics colour code.
- **The ten ASCII characters** `[` `\` `]` `^` `_` `` ` `` `{` `|` `}` `~` in the teletext font: the chip shows its own symbols
  in their place, but a teletext window also shows the other MODEs. No chip has
  these, so `make-fonts.py` draws them in the chip's style and rounds them as the
  chip would (drawn by Mark Usher).
- **Private-use pieces**, in BBC Micro MOS only: with `telnetd_font=bbc`, TelnetD
  draws each Beeb character as 1, 2 or 4 cells of an 80-column window, so every MODE
  keeps its own shape in one window. The pieces are the MODE 1/4/6 halves (U+E900),
  the MODE 2/5 quarters (U+E100), the MODE 7 halves for normal, top and bottom
  double height (U+E300), and the VDU 23 characters as 2 x 4 blocks (U+EA00).
  Nothing is at U+E000-E0FF, where terminals draw the Powerline symbols themselves.

Where a lit dot has a lit dot below it, the shape reaches 1/128 em into the row
below, over those dots only. Without that, a terminal drawing the font anti-aliased
at a fractional size can show a hairline seam between rows.

`make-fonts.py` writes all eight font files from those tables (the TrueType fonts
need `fontTools`: `apt install python3-fonttools` or `pip install fonttools`). The
font files are committed, so installing them needs neither. After a change to
either table, run `python3 make-fonts.py`. `python3 make-fonts.py --check` fails if
the committed fonts are out of date (a different fontTools version can write the
same glyphs to a different `.ttf` file; these were made with fontTools 4.68).

Each font also comes as a PSF2 console font for the Linux console (`*.psf`): 12 x 20
for MODE 7, and 8 x 16, 16 x 16 and 32 x 16 for the others. The Linux console
supports only the Basic Multilingual Plane, so the console fonts have no sextants.
On the console a mosaic is shown as the nearest block character instead.

## Windows

TelnetD sends the pound sign and the teletext graphics as UTF-8. Windows' own
telnet client (`telnet.exe`) cannot show UTF-8, so in it they come out as the
wrong characters. Everything else works, including the keys. For the full picture
on Windows, use either of these:

- **PuTTY** (see below);
- **the telnet client in WSL**, for example Debian with `sudo apt install telnet`.

Double-click `windows\install.cmd`. It installs, for the current user only (no
administrator rights needed):

- the four fonts;
- a Windows Terminal colour scheme called "BBC Micro";
- four Windows Terminal profiles, one for each width: "BBC Micro MODE 7",
  "BBC Micro MODE 0/3", "BBC Micro MODE 1/4/6" and "BBC Micro MODE 2/5". Each one
  telnets to `Pi1MHz` with WSL's telnet if there is one, and with `telnet.exe` if not.

Then do either of these:

- choose one of the profiles from Windows Terminal's new-tab menu;
- double-click `windows\BBC-Micro-telnet.cmd`. It opens a MODE 7 window, 40 x 25.
  You can give a host and a MODE: `BBC-Micro-telnet.cmd 192.168.1.50 0` opens an
  80 x 32 window for MODE 0.

Windows' telnet client is an optional feature. To turn it on, run this once from an
administrator command prompt:

    dism /online /Enable-Feature /FeatureName:TelnetClient

To change a profile's host, edit it in Windows Terminal's settings. To undo the
install, run `powershell -ExecutionPolicy Bypass -File windows\install.ps1 -Remove`.

## PuTTY

Install the fonts first with `windows\install.cmd`, or install the `.ttf` files by
hand. Then double-click `putty\BBC-Micro-telnet.reg`. This adds four saved sessions,
"BBC Micro MODE 7", "BBC Micro MODE 0-3", "BBC Micro MODE 1-4-6" and
"BBC Micro MODE 2-5", with these settings:

- telnet to `Pi1MHz`, port 23;
- that mode's window size and font, and the BBC's colours;
- UTF-8, needed for the pound sign and the teletext graphics;
- function keys sent as `ESC [ n ~`, so F12 is BREAK.

Load a session in PuTTY, change the Host Name if you need to, and save it again.
`windows\BBC-Micro-putty.cmd [host] [mode]` opens a session in one double-click.

## Debian (bookworm and trixie)

    sh linux/install.sh

The installer:

- installs the `telnet` package if it is missing;
- installs the console fonts in `/usr/share/consolefonts` (it asks for `sudo` for this);
- installs the TrueType fonts in `~/.local/share/fonts`;
- installs the `beeb-telnet` command in `~/.local/bin`.

To connect, run `beeb-telnet`, or `beeb-telnet -m MODE host`. What it does depends
on where you run it:

- **On the Linux console** (a text login, not a desktop): it loads that MODE's
  console font and the BBC's eight colours for the session. When telnet ends, it
  puts back your own font and colours. The console must be big enough for the
  mode in that font. For example, MODE 0 needs 80 x 32, which an 8 x 16 font gives
  on a 640 x 512 or larger framebuffer.
- **In a desktop terminal** (GNOME Terminal, Konsole, xterm and so on): it just runs
  telnet, and says which font and window size to choose in the terminal's own
  preferences. In xterm you can do it all on the command line:
  `xterm -fa 'BBC Micro Teletext' -fs 12 -geometry 40x25 -e telnet Pi1MHz`.

To remove everything it installed, run `sh linux/install.sh --remove`.

Character mode and line mode (`mode line` in Debian's telnet) both work the same
in every one of these.
