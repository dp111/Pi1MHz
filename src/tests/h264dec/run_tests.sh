#!/bin/sh -e
# Host tests for rpi/h264dec.c: the order in which h264dec_shutdown lets go
# of the VideoCore before a kernel.now, and what it keeps back when a step
# fails.  Real file with the real vchiq.h/vcsm.h/mmal_vc.h/h264dec.h, stub
# clients.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/rpi/h264dec.c "$SRC"/rpi/h264dec.h "$SRC"/rpi/vchiq.h \
   "$SRC"/rpi/vcsm.h "$SRC"/rpi/mmal_vc.h "$B/"
cp "$HERE"/h264dec_stubs.h "$B/"
for h in rpi.h systimer.h; do
   echo '#include "h264dec_stubs.h"' > "$B/$h"
done

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_h264dec.c" "$B/h264dec.c"
"$B/t"
