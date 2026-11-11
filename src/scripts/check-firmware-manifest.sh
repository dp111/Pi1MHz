#!/bin/sh
# (The manifest lives here, not in firmware/, so release.sh does not ship it.)
# Check the committed binary blobs under firmware/ against src/scripts/firmware.sha256.
#
#   sh src/scripts/check-firmware-manifest.sh            check (exit 1 on any difference)
#   sh src/scripts/check-firmware-manifest.sh --update   regenerate src/scripts/firmware.sha256
#
# Covered: GPU firmware, third-party ROMs, WiFi blobs, FujiNet boot discs.
# Not covered, on purpose: kernel*.img (builds are not reproducible),
# 6502code.bin / 1mhz-wicfs.rom / telnetd.rom (rebuilt and compared by
# beeb-roms.yml), and user-editable config/text (config.txt, Pi1MHz.cfg,
# defscsi.cfg, disc.html, LICENCE.broadcom.txt).
# A file under firmware/ that is neither listed nor excluded here fails the
# check as "extra"; a listed file that is gone fails as "missing".
# POSIX sh; needs sha256sum.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FW=${FIRMWARE_DIR:-$ROOT/firmware}
MANIFEST=${MANIFEST:-$ROOT/src/scripts/firmware.sha256}

cd "$FW" || exit 1

# every candidate file, relative to firmware/, sorted for a stable manifest
files() {
   find . -type f \
      ! -name 'kernel*.img' \
      ! -name 6502code.bin ! -name 1mhz-wicfs.rom ! -name telnetd.rom \
      ! -name config.txt ! -name Pi1MHz.cfg ! -name defscsi.cfg \
      ! -name disc.html ! -name 'LICENCE*' \
      | sed 's|^\./||' | LC_ALL=C sort
}

if [ "$1" = "--update" ]; then
   files | while IFS= read -r f; do sha256sum "$f"; done > "$MANIFEST"
   echo "wrote $MANIFEST ($(wc -l < "$MANIFEST" | tr -d ' ') files)"
   exit 0
fi

[ -f "$MANIFEST" ] || { echo "no manifest: $MANIFEST" >&2; exit 1; }
rc=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# differing or missing
while IFS= read -r line; do
   [ -n "$line" ] || continue
   f=${line#*  }
   if [ ! -f "$f" ]; then echo "MISSING  $f"; rc=1
   elif [ "$(printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f")" != "$line" ]; then
      echo "DIFFERS  $f"; rc=1
   fi
done < "$MANIFEST"

# extra: present, not in the manifest
sed 's/^[0-9a-f]*  //' "$MANIFEST" | LC_ALL=C sort > "$tmp/listed"
files > "$tmp/present"
for f in $(LC_ALL=C comm -13 "$tmp/listed" "$tmp/present" | tr ' ' '?'); do
   echo "EXTRA    $f (not in the manifest)"; rc=1
done

[ $rc -eq 0 ] && echo "firmware manifest OK ($(wc -l < "$MANIFEST" | tr -d ' ') files)"
[ $rc -eq 0 ] || echo "regenerate on purpose with: sh src/scripts/check-firmware-manifest.sh --update" >&2
exit $rc
