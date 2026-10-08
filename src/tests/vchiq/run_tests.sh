#!/bin/sh -e
# Host tests for rpi/vchiq.c: the kernel.now hand-over of the VideoCore
# connection (vchiq_handover / vchiq_adopt) and the close handshake, against
# a simulated VideoCore in shared memory.  Real file (included by the test),
# stub platform.  Under ASan/UBSan.  x86-64 Linux only: the block must sit
# below 4 GB (MAP_32BIT), since the client keeps addresses in 32 bits.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/rpi/vchiq.c "$SRC"/rpi/vchiq.h "$B/"
cp "$HERE"/vchiq_stubs.h "$B/"
for h in base.h rpi.h mailbox.h systimer.h asm-helpers.h screen.h; do
   echo '#include "vchiq_stubs.h"' > "$B/$h"
done

gcc -std=gnu2x -Wall -Wextra -g -Wno-unused-function \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_vchiq.c"
"$B/t"
