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
# INCLUDE_WICFS=1 merges the cassette filing system into this bank. Its sources
# are not in this tree, so WICFS_SRC must point at them; build-merged.sh
# fetches them and calls this script.
wicfs=${INCLUDE_WICFS:-0}
out=$(pwd)
if [ "$wicfs" -ne 0 ]; then
    : "${WICFS_SRC:?INCLUDE_WICFS=1 needs WICFS_SRC (see build-merged.sh)}"
    # Assembled outside the repository, never in it: the inherited sources
    # must not end up as untracked files here, one git add -A from being
    # committed into a GPL tree they may not live in.
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    cp ./*.asm "$tmp/"
    for f in wicfs.asm wicfs_errors.asm wicfs_messages.asm \
             wicfs_catalogue.asm uef.asm host_launch.asm; do
        cp "$WICFS_SRC/$f" "$tmp/$f"
    done
    cd "$tmp"
    rom=1mhz-wicfs.rom
else
    rom=1mhz-wifi.rom
fi
"$beebasm" -i 1mhzwifi.asm -D INCLUDE_RAMDISK=$ramdisk -D INCLUDE_PDUMP=$pdump \
    -D HELP_BRIEF=$brief -D INCLUDE_WICFS=$wicfs

size=$(wc -c < "$rom")
if [ "$size" -ne 16384 ]; then
    echo "$rom is $size bytes, expected 16384" >&2
    exit 1
fi
# Nothing is installed into firmware/Pi1MHz/ from here. What Pi1MHz would
# serve is the merged image, and that one is a derived work of three parties
# who have granted no licence for it - see CREDITS.md and README.md. Until
# that is settled this builds for development and for the tests only.
if [ "$wicfs" -ne 0 ]; then
    cp "$rom" "$out/$rom"       # out of the temporary tree, into src/
fi
echo "built $out/$rom"
