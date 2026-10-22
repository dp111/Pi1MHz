# A BBC Micro look for TelnetD sessions

These files make a telnet session to the Pi1MHz's telnet daemon (TelnetD, see
`docs/user/telnetd.md`) look like the Beeb's own screen. They include the BBC
Micro's character set as a font, the BBC's eight colours, and ready-made settings
for Windows Terminal, PuTTY and the Debian console or desktop. TelnetD works with
any terminal. These files only change how it looks.

Every setting uses the host name `Pi1MHz`. If your Pi has a different hostname,
or your network does not resolve it, use its IP address instead.

## The fonts

| File | For | What it is |
|---|---|---|
| `BBCMicroMOS.ttf` | Windows, desktop Linux, PuTTY | TrueType font "BBC Micro MOS" |
| `BBCMicroMOS.psf` | the Linux console (`setfont`) | PSF2 console font, 8 x 16, 256 glyphs |

Both fonts are drawn pixel for pixel from the character set in the BBC Micro's
MOS 1.20 ROM, which is out of copyright. They include:

- ASCII 32-126;
- the pound sign;
- the characters the teletext chip (SAA5050) shows in place of some ASCII ones:
  `← → ↑ ½ ¼ ¾ ‖ ― ÷` and the solid block;
- the full, left-half and right-half blocks.

The TrueType font also has Unicode's 60 sextant characters (U+1FB00-1FB3B). TelnetD
uses these to draw MODE 7's mosaic graphics, so in Windows Terminal, PuTTY or a
desktop terminal you see MODE 7 as the Beeb draws it. The Linux console supports
only the Basic Multilingual Plane, so the console font has no sextants. On the
console a mosaic is shown as the nearest block character instead.

## The window size

The Beeb's text screen is 80 x 32 in MODE 0 and 3, 40 x 32 in MODE 1, 4 and 6, 20 x 32
in MODE 2 and 5, and 40 x 25 in MODE 7. A terminal 80 columns by 32 rows fits them all.
Every setting here opens at that size.

## Windows

TelnetD sends the pound sign and the teletext graphics as UTF-8. Windows' own
telnet client (`telnet.exe`) cannot show UTF-8, so in it they come out as the
wrong characters. Everything else works, including the keys. For the full picture
on Windows, use either of these:

- **PuTTY** (see below);
- **the telnet client in WSL**, for example Debian with `sudo apt install telnet`.

Double-click `windows\install.cmd`. It installs, for the current user only (no
administrator rights needed):

- the font;
- a Windows Terminal colour scheme called "BBC Micro";
- two Windows Terminal profiles in that font and those colours:
  - "BBC Micro (WSL telnet)", which runs `wsl.exe -e telnet Pi1MHz`;
  - "BBC Micro (telnet)", which runs Windows' `telnet.exe Pi1MHz`.

Then do either of these:

- choose one of the profiles from Windows Terminal's new-tab menu;
- double-click `windows\BBC-Micro-telnet.cmd`. It opens an 80 x 32 window, using
  WSL's telnet if there is one and `telnet.exe` if not. You can name a host instead
  of the default: `BBC-Micro-telnet.cmd 192.168.1.50`.

Windows' telnet client is an optional feature. To turn it on, run this once from an
administrator command prompt:

    dism /online /Enable-Feature /FeatureName:TelnetClient

To change a profile's host, edit it in Windows Terminal's settings. To undo the
install, run `powershell -ExecutionPolicy Bypass -File windows\install.ps1 -Remove`.

## PuTTY

Install the font first with `windows\install.cmd`, or install `BBCMicroMOS.ttf` by
hand. Then double-click `putty\BBC-Micro-telnet.reg`. This adds a saved session
called "BBC Micro telnet" with these settings:

- telnet to `Pi1MHz`, port 23;
- 80 x 32;
- the BBC Micro MOS font and the BBC's colours;
- UTF-8, needed for the pound sign and the teletext graphics;
- function keys sent as `ESC [ n ~`, so F12 is BREAK.

Load the session in PuTTY, change the Host Name if you need to, and save it again.
`windows\BBC-Micro-putty.cmd [host]` opens the session in one double-click.

## Debian (bookworm and trixie)

    sh linux/install.sh

The installer:

- installs the `telnet` package if it is missing;
- installs the console font in `/usr/share/consolefonts` (it asks for `sudo` for this);
- installs the TrueType font in `~/.local/share/fonts`;
- installs the `beeb-telnet` command in `~/.local/bin`.

To connect, run `beeb-telnet` (or `beeb-telnet <host>`). What it does depends on
where you run it:

- **On the Linux console** (a text login, not a desktop): it loads the BBC Micro
  console font and the BBC's eight colours for the session. When telnet ends, it
  puts back your own font and colours. The console must be at least 80 x 32
  characters to fit MODE 0. A 640 x 512 or larger framebuffer gives that with an
  8 x 16 font.
- **In a desktop terminal** (GNOME Terminal, Konsole, xterm and so on): it just runs
  telnet. Choose "BBC Micro MOS" as the font, and an 80 x 32 window, in the terminal's
  own preferences. In xterm you can do it all on the command line:
  `xterm -fa 'BBC Micro MOS' -fs 12 -geometry 80x32 -e telnet Pi1MHz`.

To remove everything it installed, run `sh linux/install.sh --remove`.

Character mode and line mode (`mode line` in Debian's telnet) both work the same
in every one of these.
