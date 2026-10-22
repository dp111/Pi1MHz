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
| BBC Micro Teletext | `BBCMicroTeletext.ttf` | MODE 7 | 40 x 25 | 5:6 |
| BBC Micro MOS | `BBCMicroMOS.ttf` | MODE 0 and 3 | 80 x 32 | 1:2 |
| BBC Micro MOS 40 | `BBCMicroMOS40.ttf` | MODE 1, 4 and 6 | 40 x 32 | 1:1 |
| BBC Micro MOS 20 | `BBCMicroMOS20.ttf` | MODE 2 and 5 | 20 x 32 | 2:1 |

The Beeb starts in MODE 7, so MODE 7 is the default everywhere. A window opened for
one mode still shows the others, in its own font.

The three MOS fonts are drawn pixel for pixel from the character set in the BBC
Micro's MOS 1.20 ROM, which is out of copyright. They include ASCII, the pound
sign, and the characters TelnetD sends for MODE 7's special ones: `← → ↑ ½ ¼ ¾ ‖ ― ÷`
and the solid block.

The teletext font is our own drawing in the style of the teletext chip (SAA5050).
Each character is drawn as a 5 x 9 dot matrix. The chip's "character rounding" then
smooths it to 12 x 20 pixels by adding a half dot wherever two dots meet only at a
corner. As on the chip, it has `£ ← ½ → ↑ # ― ¼ ‖ ¾ ÷` and the block in place of
some ASCII characters.

Every TrueType font also has Unicode's 60 sextant characters (U+1FB00-1FB3B) and the
full and half blocks, filling the whole cell. TelnetD uses these to draw MODE 7's
mosaic graphics, so in Windows Terminal, PuTTY or a desktop terminal the graphics
join up as on the Beeb.

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
