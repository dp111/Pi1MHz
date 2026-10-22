#!/bin/sh
# install.sh - the BBC Micro look for a telnet session to TelnetD on Debian (bookworm, trixie): the console fonts,
# the TrueType fonts for desktop terminals (one of each for each width of the Beeb's screen), the telnet client, and
# the "beeb-telnet" command.
#   sh install.sh            install (asks for sudo for the console fonts and the telnet package)
#   sh install.sh --remove   remove what it installed
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FONTS="$HERE/.."
BIN="$HOME/.local/bin"
TTFDIR="$HOME/.local/share/fonts"
CONSOLE=/usr/share/consolefonts
NAMES="BBCMicroTeletext BBCMicroMOS BBCMicroMOS40 BBCMicroMOS20"

if [ "$1" = "--remove" ]; then
    rm -f "$BIN/beeb-telnet"
    for n in $NAMES; do
        rm -f "$TTFDIR/$n.ttf"
        sudo rm -f "$CONSOLE/$n.psf"
    done
    command -v fc-cache >/dev/null && fc-cache -f "$TTFDIR" >/dev/null 2>&1 || true
    echo "Removed beeb-telnet and the BBC Micro fonts."
    exit 0
fi

# the telnet client (Debian's "telnet" is inetutils telnet since trixie, netkit's before)
if ! command -v telnet >/dev/null; then
    echo "Installing the telnet client..."
    sudo apt-get install -y telnet
fi

# the console fonts, for the Linux console (setfont), and the TrueType fonts, for terminals on a desktop
mkdir -p "$TTFDIR"
for n in $NAMES; do
    sudo install -m 644 "$FONTS/$n.psf" "$CONSOLE/$n.psf"
    install -m 644 "$FONTS/$n.ttf" "$TTFDIR/$n.ttf"
done
command -v fc-cache >/dev/null && fc-cache -f "$TTFDIR" >/dev/null 2>&1 || true
echo "Console fonts: $CONSOLE/BBCMicro*.psf"
echo "TrueType fonts: $TTFDIR (\"BBC Micro Teletext\", \"BBC Micro MOS\", \"BBC Micro MOS 40\", \"BBC Micro MOS 20\")"

mkdir -p "$BIN"
install -m 755 "$HERE/beeb-telnet" "$BIN/beeb-telnet"
echo "Command: $BIN/beeb-telnet [-m MODE] [host]   (MODE 7 and host Pi1MHz if not given)"
case ":$PATH:" in *":$BIN:"*) ;; *) echo "Add $BIN to your PATH (it is there after logging in again on Debian)." ;; esac
