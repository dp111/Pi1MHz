#!/bin/sh
# Assemble the helper loader that Pi1MHz serves through its JIM window.
# Requires beebasm on PATH, or set BEEBASM (the in-tree clone is ../../beebasm).
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
beebasm=${BEEBASM:-}
if [ -z "$beebasm" ]; then
    if command -v beebasm >/dev/null 2>&1; then beebasm=beebasm; else beebasm=../../beebasm/beebasm; fi
fi
"$beebasm" -i 6502code.asm
echo "built $(cd .. && pwd)/firmware/Pi1MHz/6502code.bin"
