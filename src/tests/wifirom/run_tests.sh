#!/bin/sh -e
# Host checks for the 1MHz-WiFi host ROM in beeb/1mhz-wifi/.
#
# 1. its service command numbers still match the headers it talks to,
# 2. it only writes to memory a sideways ROM owns, and
# 3. its command table stays walkable and its help list stays in step, and
# 4. it still assembles to a 16 KiB image, when beebasm is available, and
# 5. on a 6502 (py65) its reset, unrecognised-command and *HELP paths keep
#    the service-call rules, in sideways RAM and in a read-only bank: the
#    shipped image always, both *HELP builds when beebasm is available.
#
# The ROM is not part of the firmware build, so nothing else would notice a
# renumbering of wifi_service.h until the ROM misbehaved on real hardware.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)

python3 "$HERE/check_interface.py"
python3 "$HERE/check_memory.py"
python3 "$HERE/check_help_table.py"

PY=${PYTHON:-python3}
images="shipped=$ROOT/firmware/Pi1MHz/1mhz-wicfs.rom"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

beebasm=${BEEBASM:-beebasm}
if command -v "$beebasm" >/dev/null 2>&1; then
    rom="$ROOT/beeb/1mhz-wifi/src/1mhz-wifi.rom"
    for brief in 0 1; do
        HELP_BRIEF=$brief BEEBASM="$beebasm" sh "$ROOT/beeb/1mhz-wifi/build.sh" >/dev/null
        echo "ROM BUILD OK (HELP_BRIEF=$brief): $(wc -c < "$rom" | tr -d ' ') bytes"
        mv "$rom" "$tmp/help$brief.rom"
        images="$images help-brief-$brief=$tmp/help$brief.rom"
    done
else
    echo "beebasm not found (set BEEBASM): skipped the ROM assembly check"
fi

# shellcheck disable=SC2086 # $images is a list of LABEL=PATH words
"$PY" "$HERE/check_service.py" $images

echo "WIFI ROM TESTS PASSED"
