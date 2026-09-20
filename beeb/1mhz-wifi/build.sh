#!/bin/sh
# Assemble the 1MHz-WiFi host ROM. Requires beebasm on PATH, or set BEEBASM.
set -eu

beebasm=${BEEBASM:-beebasm}
cd "$(CDPATH= cd -- "$(dirname -- "$0")/src" && pwd)"
# INCLUDE_RAMDISK=0 drops the RAM disc, which is what makes room for a second
# ROM merged into this bank: the RAM disc exists to get a program into the
# machine without a filing system, so it is redundant beside a cassette FS.
ramdisk=${INCLUDE_RAMDISK:-1}
pdump=${INCLUDE_PDUMP:-1}      # *PRD, a paged-RAM dump for debugging
brief=${HELP_BRIEF:-0}         # *HELP WIFI: names from the command table only
"$beebasm" -i 1mhzwifi.asm -D INCLUDE_RAMDISK=$ramdisk -D INCLUDE_PDUMP=$pdump -D HELP_BRIEF=$brief

size=$(wc -c < 1mhz-wifi.rom)
if [ "$size" -ne 16384 ]; then
    echo "1mhz-wifi.rom is $size bytes, expected 16384" >&2
    exit 1
fi
# Keep the image the Pi serves (helper 16) regenerable from this source.
install=../../../firmware/Pi1MHz/1mhz-wifi.rom
cp 1mhz-wifi.rom "$install"
echo "built $(pwd)/1mhz-wifi.rom -> $(cd "$(dirname "$install")" && pwd)/1mhz-wifi.rom"
