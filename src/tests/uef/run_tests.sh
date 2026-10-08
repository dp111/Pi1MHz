#!/bin/sh -e
# Host tests for the resumable UEF stream (uef_stream.c + vendored uzlib).
# Streams every UEF it can find and checks the bytes against gunzip, which is
# the only reference that matters: the Beeb must see exactly the tape a
# desktop tool would produce.  Runs under ASan/UBSan.
#
# Set UEF_CORPUS to a directory of .uef files.  Without one the test still
# runs, on UEFs generated here, but a real corpus is much better cover - the
# gzip headers real tools emit vary (FNAME, FEXTRA, no flags at all) and that
# is exactly where a hand-written header walk goes wrong.  The generated
# set therefore includes a gzip for each optional header field.
# NB: set -e here too - the shebang -e is ignored under "sh script.sh".
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

mkdir -p "$B/uzlib"
mkdir -p "$B/rpi"
cp "$SRC"/uef_stream.c "$SRC"/uef_stream.h "$B/"
cp "$SRC"/rpi/byteorder.h "$B/rpi/"
cp "$SRC"/uzlib/*.c "$SRC"/uzlib/*.h "$B/uzlib/"
cp "$HERE"/test_uef_stream.c "$HERE"/test_uef_service.c "$B/"
cp "$SRC"/uef_service.c "$SRC"/uef_service.h "$SRC"/wifi_service.h "$SRC"/services.h "$B/"
# uef_service.c calls the FILEV stamp repair in uef_repair.c.
cp "$SRC"/uef_repair.c "$SRC"/uef_repair.h "$SRC"/config.h "$B/"
cp "$HERE"/test_uef_filev.c "$B/"
cp -r "$HERE"/stubs/. "$B/"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

# uzlib is vendored and not warning-clean under our flags; it is built here
# the way the firmware builds it, with its own warnings silenced.
gcc -std=gnu2x -g $SAN -w -c -I"$B" -o "$B/tinflate.o" "$B/uzlib/tinflate.c"
gcc -std=gnu2x -g $SAN -w -c -I"$B" -o "$B/crc32.o"    "$B/uzlib/crc32.c"
gcc -std=gnu2x -g $SAN -w -c -I"$B" -o "$B/adler32.o"  "$B/uzlib/adler32.c"
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN -I"$B" -o "$B/t" \
    "$B/test_uef_stream.c" "$B/uef_stream.c" \
    "$B/tinflate.o" "$B/crc32.o" "$B/adler32.o"

# Build a corpus: a synthetic tape, raw and gzipped, so the test is useful
# even with no real UEFs to hand.
mkdir -p "$B/corpus"
printf 'UEF File!\0\012\000' > "$B/corpus/synthetic.uef"
# Repetitive content so DEFLATE actually emits back-references, which is what
# exercises the 32 KB history window across more than one output window.
i=0
while [ $i -lt 4000 ]; do
   printf '\020\001\014\000\000\000\052\125\052\125\052\125\052\125\052\125\052\125\052\125' \
      >> "$B/corpus/synthetic.uef"
   i=$((i + 1))
done
gzip -c "$B/corpus/synthetic.uef" > "$B/corpus/synthetic.uef.gz"
mv "$B/corpus/synthetic.uef.gz" "$B/corpus/synthetic_gz.uef"
# gzip(1) only ever writes FNAME (or no flags), so build the other optional
# header fields by hand: FEXTRA, FCOMMENT, FHCRC each alone, then all four
# together.  A walk that mis-skips any of them starts inflating in the
# header and fails the CRC, or differs from gunzip below.
python3 - "$B/corpus" <<'PY'
import struct, sys, zlib
d = sys.argv[1]
tape = open(f"{d}/synthetic.uef", "rb").read()
co = zlib.compressobj(9, zlib.DEFLATED, -15)
deflated = co.compress(tape) + co.flush()
trailer = struct.pack("<II", zlib.crc32(tape), len(tape) & 0xffffffff)
FHCRC, FEXTRA, FNAME, FCOMMENT = 2, 4, 8, 16
def gz(flags):
    h = bytes([0x1f, 0x8b, 8, flags]) + struct.pack("<I", 0) + bytes([0, 255])
    if flags & FEXTRA:      # two subfields, 0x1f 0x8b in the data on purpose
        sub = b"AP" + struct.pack("<H", 6) + b"\x1f\x8b\x08\x00\x00\x00"
        sub += b"UE" + struct.pack("<H", 300) + bytes(range(256)) + bytes(44)
        h += struct.pack("<H", len(sub)) + sub
    if flags & FNAME:
        h += b"tape.uef\0"
    if flags & FCOMMENT:
        h += b"a comment, with \x1f\x8b and high bytes \xa9\xff\0"
    if flags & FHCRC:
        h += struct.pack("<H", zlib.crc32(h) & 0xffff)
    return h + deflated + trailer
for name, flags in (("fextra", FEXTRA), ("fcomment", FCOMMENT),
                    ("fhcrc", FHCRC),
                    ("allflags", FEXTRA | FNAME | FCOMMENT | FHCRC)):
    open(f"{d}/hdr_{name}.uef", "wb").write(gz(flags))
PY
for f in fextra fcomment fhcrc allflags; do   # the reference must accept them
   gzip -t "$B/corpus/hdr_$f.uef"
done

if [ -n "$UEF_CORPUS" ] && [ -d "$UEF_CORPUS" ]; then
   find "$UEF_CORPUS" -iname '*.uef' -exec cp {} "$B/corpus/" \; 2>/dev/null || true
fi

gcc -std=gnu2x -Wall -Wextra -g $SAN -I"$B" -o "$B/svc" \
    "$B/test_uef_service.c" "$B/uef_service.c" "$B/uef_stream.c" \
    "$B/uef_repair.c" \
    "$B/tinflate.o" "$B/crc32.o" "$B/adler32.o"

gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN -I"$B" -o "$B/filev" \
    "$B/test_uef_filev.c" "$B/uef_service.c" "$B/uef_stream.c" \
    "$B/uef_repair.c" \
    "$B/tinflate.o" "$B/crc32.o" "$B/adler32.o"

echo "== UEF stream: bytes match gunzip, rewind repeats, CRC verifies =="
pass=0
fail=0
for f in "$B"/corpus/*.uef; do
   [ -e "$f" ] || continue
   if ! "$B/t" "$f" "$B/out.bin" > "$B/line" 2>&1; then
      cat "$B/line"; fail=$((fail + 1)); continue
   fi
   if [ "$(dd if="$f" bs=1 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "1f8b" ]; then
      gzip -dc "$f" > "$B/ref.bin"
   else
      cp "$f" "$B/ref.bin"
   fi
   if cmp -s "$B/out.bin" "$B/ref.bin"; then
      pass=$((pass + 1))
      sed "s#$B/corpus/##" "$B/line"
   else
      echo "  MISMATCH vs gunzip: $(basename "$f")"
      fail=$((fail + 1))
   fi
done

echo
echo "$pass ok, $fail bad"
[ "$fail" -eq 0 ]

# What a gzip or zip unwraps to must itself be a tape: a zip of a .uef.gz,
# or a gzip of one, is refused rather than streamed as noise.
echo
echo "== UEF stream: zip entries, and wrappers around a non-tape refused =="
gzip -c "$B/corpus/synthetic_gz.uef" > "$B/double.gz"
python3 - "$B" <<'PY'
import sys, zipfile
b = sys.argv[1]
for name, method, src in (("zip_deflated", zipfile.ZIP_DEFLATED, "synthetic.uef"),
                          ("zip_stored", zipfile.ZIP_STORED, "synthetic.uef"),
                          ("zip_of_gz", zipfile.ZIP_DEFLATED, "synthetic_gz.uef"),
                          ("zip_stored_gz", zipfile.ZIP_STORED, "synthetic_gz.uef")):
    with zipfile.ZipFile(f"{b}/{name}.zip", "w", method) as z:
        z.write(f"{b}/corpus/{src}", "tape.uef")
PY
for f in zip_deflated zip_stored; do
   "$B/t" "$B/$f.zip" "$B/out.bin" | sed "s#$B/##"
   cmp -s "$B/out.bin" "$B/corpus/synthetic.uef" || { echo "  MISMATCH: $f"; exit 1; }
done
for f in double.gz zip_of_gz.zip zip_stored_gz.zip; do
   if "$B/t" "$B/$f" "$B/out.bin" > "$B/line" 2>&1; then
      echo "  ACCEPTED, should be refused: $f"; exit 1
   fi
   grep -q INVALID "$B/line" || { cat "$B/line"; exit 1; }
   echo "  $f refused"
done

# The service protocol, driven the way the ROM drives it, against a fake JIM.
echo
for f in "$B"/corpus/*.uef; do
   [ -e "$f" ] || continue
   if [ "$(dd if="$f" bs=1 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "1f8b" ]; then
      gzip -dc "$f" > "$B/ref.bin"
   else
      cp "$f" "$B/ref.bin"
   fi
   "$B/svc" "$f" "$B/ref.bin"
done

echo
echo "== FILEV stamp repair across window boundaries =="
"$B/filev"
