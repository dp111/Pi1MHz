#!/bin/sh
# install.sh - the BBC Micro look for a telnet session to TelnetD on Debian (bookworm, trixie): the console font,
# the TrueType font for desktop terminals, the telnet client, and the "beeb-telnet" command.
#   sh install.sh            install (asks for sudo for the console font and the telnet package)
#   sh install.sh --remove   remove what it installed
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FONTS="$HERE/.."
BIN="$HOME/.local/bin"
TTFDIR="$HOME/.local/share/fonts"
CONSOLE=/usr/share/consolefonts

if [ "$1" = "--remove" ]; then
    rm -f "$BIN/beeb-telnet" "$TTFDIR/BBCMicroMOS.ttf"
    command -v fc-cache >/dev/null && fc-cache -f "$TTFDIR" >/dev/null 2>&1 || true
    sudo rm -f "$CONSOLE/BBCMicroMOS.psf"
    echo "Removed beeb-telnet and the BBC Micro fonts."
    exit 0
fi

# the telnet client (Debian's "telnet" is inetutils telnet since trixie, netkit's before)
if ! command -v telnet >/dev/null; then
    echo "Installing the telnet client..."
    sudo apt-get install -y telnet
fi

# the console font, for the Linux console (setfont)
sudo install -m 644 "$FONTS/BBCMicroMOS.psf" "$CONSOLE/BBCMicroMOS.psf"
echo "Console font: $CONSOLE/BBCMicroMOS.psf"

# the TrueType font, for terminals on a desktop
mkdir -p "$TTFDIR"
install -m 644 "$FONTS/BBCMicroMOS.ttf" "$TTFDIR/BBCMicroMOS.ttf"
command -v fc-cache >/dev/null && fc-cache -f "$TTFDIR" >/dev/null 2>&1 || true
echo "TrueType font: $TTFDIR/BBCMicroMOS.ttf (\"BBC Micro MOS\")"

mkdir -p "$BIN"
install -m 755 "$HERE/beeb-telnet" "$BIN/beeb-telnet"
echo "Command: $BIN/beeb-telnet [host]   (host defaults to Pi1MHz)"
case ":$PATH:" in *":$BIN:"*) ;; *) echo "Add $BIN to your PATH (it is there after logging in again on Debian)." ;; esac
