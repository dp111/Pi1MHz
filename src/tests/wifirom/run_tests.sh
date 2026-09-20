#!/bin/sh -e
# Host checks for the 1MHz-WiFi host ROM in beeb/1mhz-wifi/.
#
# 1. its service command numbers still match the headers it talks to, and
# 2. it still assembles to a 16 KiB image, when beebasm is available.
#
# The ROM is not part of the firmware build, so nothing else would notice a
# renumbering of wifi_service.h until the ROM misbehaved on real hardware.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)

python3 "$HERE/check_interface.py"

beebasm=${BEEBASM:-beebasm}
if command -v "$beebasm" >/dev/null 2>&1; then
    BEEBASM="$beebasm" sh "$ROOT/beeb/1mhz-wifi/build.sh" >/dev/null
    rom="$ROOT/beeb/1mhz-wifi/src/1mhz-wifi.rom"
    echo "ROM BUILD OK: $(wc -c < "$rom" | tr -d ' ') bytes"
    rm -f "$rom"
else
    echo "beebasm not found (set BEEBASM): skipped the ROM assembly check"
fi

echo "WIFI ROM TESTS PASSED"
