#!/bin/sh
# Assemble telnetd.rom, the sideways ROM that runs helper 20 on every reset, and
# install it where the helpers look for it.  Requires beebasm on PATH, or set
# BEEBASM.
set -eu

beebasm=${BEEBASM:-beebasm}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here"
"$beebasm" -i telnetd.asm
size=$(wc -c < telnetd.rom)
if [ "$size" -ne 16384 ]; then
    echo "telnetd.rom is $size bytes, expected 16384" >&2
    exit 1
fi
mv telnetd.rom ../../firmware/Pi1MHz/telnetd.rom
echo "built $(cd ../.. && pwd)/firmware/Pi1MHz/telnetd.rom"
