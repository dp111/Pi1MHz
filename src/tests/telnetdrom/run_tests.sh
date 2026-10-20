#!/bin/sh -e
# Host checks for the telnet auto-start ROM (beeb/telnetd-rom/): it assembles to
# a 16 KiB image that matches the one shipped in firmware/Pi1MHz/, and on a
# 6502 (py65) its reset handler runs helper 20 and keeps the service-call rules.
#
# The ROM is not part of the firmware build, so nothing else would notice it
# stop assembling.  Needs beebasm and py65, so it is left out of the default
# run_all.sh list (SKIP), as wifirom is.
set -e
skipped=0
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
PY=${PYTHON:-python3}
beebasm=${BEEBASM:-beebasm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if command -v "$beebasm" >/dev/null 2>&1; then
    cp "$ROOT/beeb/telnetd-rom/telnetd.asm" "$tmp/"
    (cd "$tmp" && "$beebasm" -i telnetd.asm)
    size=$(wc -c < "$tmp/telnetd.rom" | tr -d ' ')
    [ "$size" -eq 16384 ] || { echo "telnetd.rom is $size bytes, expected 16384" >&2; exit 1; }
    echo "ROM BUILD OK: $size bytes"
    cmp "$tmp/telnetd.rom" "$ROOT/firmware/Pi1MHz/telnetd.rom" \
        || { echo "firmware/Pi1MHz/telnetd.rom does not match beeb/telnetd-rom/telnetd.asm" >&2; exit 1; }
    echo "the shipped image matches its source"
else
    echo "beebasm not found (set BEEBASM): skipped the ROM assembly check"
    [ -z "$CI" ] || exit 1
    skipped=1
fi

if "$PY" -c "import py65" 2>/dev/null; then
    "$PY" "$HERE/check_rom.py" "$ROOT/firmware/Pi1MHz/telnetd.rom"
else
    echo "py65 not found (pip install py65): skipped the 6502 checks"
    [ -z "$CI" ] || exit 1
    skipped=$((skipped + 2))
fi
case $skipped in
    0) echo "TELNETD ROM TESTS PASSED" ;;
    3) echo "TELNETD ROM TESTS SKIPPED (no beebasm, no py65): nothing was checked" ;;
    *) echo "TELNETD ROM TESTS PASSED, PARTLY: some checks were skipped (see above)" ;;
esac
