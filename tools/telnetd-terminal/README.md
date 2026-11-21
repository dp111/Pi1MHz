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
| BBC Micro Teletext | `BBCMicroTeletext.ttf` | MODE 7 | 40 x 25 | 27:32 |
| BBC Micro MOS | `BBCMicroMOS.ttf` | MODE 0 and 3 | 80 x 32 | 1:2 |
| BBC Micro MOS 40 | `BBCMicroMOS40.ttf` | MODE 1, 4 and 6 | 40 x 32 | 1:1 |
| BBC Micro MOS 20 | `BBCMicroMOS20.ttf` | MODE 2 and 5 | 20 x 32 | 2:1 |

The Beeb starts in MODE 7, so MODE 7 is the default everywhere. A window opened for
one mode still shows the others, in its own font.

The fonts are made from the Pi1MHz framebuffer's own character sets, so a telnet
session shows the same characters as the Pi's screen:

- The three MOS fonts are the framebuffer's BBC Micro character set
  (`src/framebuffer/fonts/bbc.fnt.h`), dot for dot. They include ASCII, the pound
  sign, and the characters TelnetD sends for MODE 7's special ones:
  `← → ↑ ½ ¼ ¾ ‖ ― ÷` and the solid block. The MOS has no glyphs for these, so
  `make-fonts.py` draws them itself, the only glyphs it does.
- The teletext font is the framebuffer's teletext chip (SAA5050) character set
  (`src/framebuffer/fonts/saa5050.fnt.h`). Each 6 x 10 dot character is smoothed to
  12 x 20 pixels by the chip's "character rounding", which adds a half dot wherever
  two dots meet only at a corner, the way the framebuffer does it. As on the chip,
  it has `£ ← ½ → ↑ # ― ¼ ‖ ¾ ÷` and the block in place of some ASCII characters.

Every TrueType font also has Unicode's 60 sextant characters (U+1FB00-1FB3B) and the
full and half blocks, filling the whole cell. TelnetD uses these to draw MODE 7's
mosaic graphics, so in Windows Terminal, PuTTY or a desktop terminal the graphics
join up as on the Beeb. They are the chip's own mosaics, from the same table.

`make-fonts.py` writes all eight font files from those tables (the TrueType fonts
need `fontTools`: `apt install python3-fonttools` or `pip install fonttools`). The
font files are committed, so installing them needs neither. After a change to
either table, run `python3 make-fonts.py`. `python3 make-fonts.py --check` fails if
the committed fonts are out of date (a different fontTools version can write the
same glyphs to a different `.ttf` file; these were made with fontTools 4.67).

Each font also comes as a PSF2 console font for the Linux console (`*.psf`): 12 x 20
for MODE 7, and 8 x 16, 16 x 16 and 32 x 16 for the others. The Linux console
supports only the Basic Multilingual Plane, so the console fonts have no sextants.
On the console a mosaic is shown as the nearest block character instead.

## Another set of fonts: `telnetd-fonts/`

`telnetd-fonts/` holds a second set of the four TrueType fonts, made by its own
generator, `make-fonts-telnetd.py`. It has the same file and family names as the
set above, so it installs the same way: copy its four `.ttf` files over the ones
in this folder before running an installer, or install them by hand. The console
fonts (`*.psf`) are the ones above; this set has none of its own. `make-fonts.py`
and its fonts are not changed by it.

This set carries the characters that the coming TelnetD changes send (the line
and shell modes, the characters a program defines, and one window for every MODE
with `telnetd_font=bbc`). Today's TelnetD sends none of these, so for it the two
sets show the same thing. The set is here to compare with the one above.

| Font | `make-fonts.py` | `make-fonts-telnetd.py` | What the second set adds |
|---|---|---|---|
| BBC Micro Teletext | 159 | 245 | separated sextants (63), quadrants (12), U+2592, `[ \ ] ^ _ { \| } ~` and `` ` `` (10) |
| BBC Micro MOS | 169 | 2,805 | separated sextants, quadrants, U+2592, and 2,560 private-use pieces for `telnetd_font=bbc` |
| BBC Micro MOS 40 | 169 | 245 | separated sextants, quadrants, U+2592 |
| BBC Micro MOS 20 | 169 | 245 | separated sextants, quadrants, U+2592 |

What they are for:

- **Separated sextants** (U+1CE51-1CE8F, Unicode 16): MODE 7's separated graphics,
  which today go out as contiguous ones.
- **Quadrants** (U+2580, U+2584, U+2596-259F): a character a program defines with
  VDU 23, drawn as 2 x 2 blocks when the terminal has no BBC font.
- **U+2592**, the medium shade: the marker for a graphics colour code in shell mode.
- **The ten ASCII characters in the teletext font**: the chip shows its own symbols
  in their place, but a teletext window also shows the other MODEs.
- **Private-use pieces** (BBC Micro MOS only): with `telnetd_font=bbc`, TelnetD draws
  each Beeb character as 1, 2 or 4 cells of an 80-column window, so every MODE keeps
  its own shape in one window. The pieces are the MODE 1/4/6 halves (U+E900), the
  MODE 2/5 quarters (U+E100), and the MODE 7 halves for normal, top and bottom
  double height (U+E300). The VDU 23 characters are 2 x 4 blocks (U+EA00). Nothing
  is at U+E000-E0FF, where terminals draw the Powerline symbols themselves.

Where the shared glyphs differ:

- **The shapes are the same.** On each glyph's own grid (8 x 8 for MOS, 12 x 20 for
  teletext) all 169 MOS glyphs and all 159 teletext glyphs match `make-fonts.py`'s
  dot for dot.
  - The MOS table in `make-fonts-telnetd.py` is the MOS 1.20 ROM's character set.
    `--check` confirms it is the same as `bbc.fnt.h`.
  - Its teletext characters were measured from a real BBC Micro's screen. They come
    out the same as the SAA5050 table after rounding, which is an independent check
    of that table.
- **The teletext cell is wider**: 922 units, against 864: a 9:10 cell against 27:32, as
  measured on a real Beeb's screen.
- **The outlines are built differently.** `make-fonts-telnetd.py` draws each row's
  runs of dots separately. Where the row below is lit, a run reaches 8 units (1/128
  em) into that row. This stops the hairline seam between rows that some terminals
  show when they draw a font anti-aliased at a fractional size. `make-fonts.py`
  merges rows into taller rectangles instead.
  - Because of this, the outline points differ in 147 of 169 glyphs in each MOS font
    and in 158 of 159 teletext glyphs.
  - The overlap also reaches under dots that are not lit in the row below. On a fine
    grid (96 x 160 a cell), that shows as a sliver of at most 1/128 em in 92 MOS and
    83 teletext glyphs.
  - `telnetd-fonts/sheets/diff-*.png` shows each of those glyphs as three cells:
    `make-fonts.py`'s, `make-fonts-telnetd.py`'s, and the difference. The other
    sheets show the added groups.

Where every glyph comes from: the data tables in `make-fonts-telnetd.py` are
marked MEASURED or INFERRED.

- MEASURED: the MOS ROM's characters, and the MODE 7 characters measured on a real
  Beeb.
- INFERRED: MODE 7's own symbols drawn in the MOS style, and the ten ASCII
  characters drawn in the chip's style.

Everything else comes from rules in the script: the rounding, the sextants and the
separated sextants (in the proportions measured on the Beeb), the quadrants, the
shade, the VDU 23 blocks, and the pieces cut from the base glyphs.

    python3 make-fonts-telnetd.py           # writes telnetd-fonts/*.ttf
    python3 make-fonts-telnetd.py --check   # fails if they are out of date

Which to choose:

- For today's TelnetD the two sets look alike. The only visible difference is the
  slightly wider MODE 7 cell.
- With the coming TelnetD changes, a terminal using the set above shows missing
  characters for the groups in the table. Using `telnetd-fonts/` shows them all.
- A third way is to take the new groups into `make-fonts.py` from its own tables.

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
