#!/bin/sh -e
# Host tests for BeebSCSI/fcode.c: the VP overlay modes leave the player's
# plane to the player.  Real file (with the real fcode.h and videoplayer.h),
# stub platform.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: one stub header stands in at every path fcode.c
# includes, relative to itself.
mkdir -p "$B/BeebSCSI" "$B/rpi"
cp "$SRC"/BeebSCSI/fcode.c "$SRC"/BeebSCSI/fcode.h "$B/BeebSCSI/"
cp "$SRC"/videoplayer.h "$B/"
cp "$HERE"/fcode_stubs.h "$B/"
for h in harddisc_emulator.h config.h rpi/systimer.h rpi/screen.h \
         BeebSCSI/uart.h BeebSCSI/debug.h BeebSCSI/filesystem.h; do
   echo '#include "fcode_stubs.h"' > "$B/$h"
done

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -I"$B/BeebSCSI" -include stddef.h -o "$B/t" \
    "$HERE/test_fcode.c" "$B/BeebSCSI/fcode.c"
"$B/t"
