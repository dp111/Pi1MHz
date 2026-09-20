#!/bin/sh
# Build the merged image: this ROM plus the WiCFS cassette filing system, in
# one 16K bank.
#
# The filing system is not in this tree and must not be committed to it. Two
# of its three authors have never stated any licence, and the third states
# that none is granted, so this fetches the sources at build time instead:
#
#   ElkWiFi 0.23      wicfs.asm, from Roland Leurs, itself deriving from
#                     Martin Barr's UPCFS. The pinned commit is unreferenced
#                     by any branch but is still served by SHA.
#   1mhzWifi          Peter Clarke's 45 patches to that file, and the five
#                     sources around it that are his own work.
#
# See CREDITS.md. Nothing here is installed into firmware/Pi1MHz/: the
# merged image may not be redistributed until those terms are settled.
set -eu

ELKWIFI_URL=https://github.com/AtomicRoland/ElkWiFi
ELKWIFI_SHA=7bf366c97bec18bd238963c95e6f2aa6893cdb3a
DOWNSTREAM_URL=https://github.com/peteclarke-del/1mhzWifi
DOWNSTREAM_SHA=e9d9b9b287ef7d517f4fbe578c40ab02530d2678

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cache=${WICFS_CACHE:-${TMPDIR:-/tmp}/pi1mhz-wicfs}
beebasm=${BEEBASM:-beebasm}

fetch() {   # url sha dir
    if [ ! -d "$3/.git" ]; then
        mkdir -p "$3"
        git -C "$3" init -q
        git -C "$3" remote add origin "$1"
    fi
    # By SHA, not by branch: the ElkWiFi commit this is based on is not on one.
    git -C "$3" fetch -q --depth 1 origin "$2"
    git -C "$3" checkout -q -f FETCH_HEAD
}

echo "fetching the inherited filing system into $cache"
fetch "$ELKWIFI_URL" "$ELKWIFI_SHA" "$cache/elkwifi"
fetch "$DOWNSTREAM_URL" "$DOWNSTREAM_SHA" "$cache/1mhzwifi"

work=$cache/wicfs-src
rm -rf "$work"
mkdir -p "$work"

# Patch order is significant: each one is a zero-context diff that assumes the
# ones before it. Take the order from the upstream build script rather than
# keeping a second copy of the list here, so the two cannot drift.
patches=$cache/1mhzwifi/rom-side/inherited/patches
order=$(sed -n 's/^for patch_name in \(.*\); do$/\1/p' \
    "$cache/1mhzwifi/rom-side/build_rom.sh")
[ -n "$order" ] || { echo "cannot read the patch order from build_rom.sh" >&2; exit 1; }
applied=0
for patch in $order; do
    [ -f "$patches/$patch" ] || { echo "missing patch: $patch" >&2; exit 1; }
    # Upstream wicfs.asm is CRLF; the patches are not.
    # In the checkout, where the paths the patches carry resolve. The fetch
    # above reset it, so this is the same every run.
    git -C "$cache/elkwifi" apply --ignore-space-change --ignore-whitespace \
        --unidiff-zero "$patches/$patch"
    applied=$((applied + 1))
done
echo "applied $applied patches to wicfs.asm"
cp "$cache/elkwifi/rom/wicfs.asm" "$work/"

# The rest of the filing system is Peter Clarke's own work, alongside it.
for f in wicfs_errors.asm wicfs_messages.asm wicfs_catalogue.asm \
         uef.asm host_launch.asm; do
    cp "$cache/1mhzwifi/rom-side/1mhz-wifi/src/$f" "$work/"
done

# The one configuration that fits: the RAM disc and *UEF LOAD both stay, and
# *PRD and the descriptive *HELP are what pay for them.
INCLUDE_WICFS=1 WICFS_SRC=$work \
INCLUDE_RAMDISK=${INCLUDE_RAMDISK:-1} \
INCLUDE_PDUMP=${INCLUDE_PDUMP:-0} \
HELP_BRIEF=${HELP_BRIEF:-1} \
BEEBASM=$beebasm sh "$here/build.sh"

rom=$here/src/1mhz-wicfs.rom
WICFS_SRC=$work BEEBASM=$beebasm \
    python3 "$here/../../src/tests/wifirom/check_help_table.py"
echo "sha256 $(sha256sum "$rom" | cut -d' ' -f1)"
