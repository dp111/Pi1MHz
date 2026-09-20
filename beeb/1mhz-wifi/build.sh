#!/bin/sh
# Assemble the 1MHz-WiFi host ROM. Requires beebasm on PATH, or set BEEBASM.
set -eu

beebasm=${BEEBASM:-beebasm}
cd "$(CDPATH= cd -- "$(dirname -- "$0")/src" && pwd)"
"$beebasm" -i 1mhzwifi.asm

size=$(wc -c < 1mhz-wifi.rom)
if [ "$size" -ne 16384 ]; then
    echo "1mhz-wifi.rom is $size bytes, expected 16384" >&2
    exit 1
fi
echo "built $(pwd)/1mhz-wifi.rom"
