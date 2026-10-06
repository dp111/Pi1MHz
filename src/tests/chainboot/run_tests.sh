#!/bin/sh -e
# Host tests for chainboot.c: the image check, the hand-over (padding,
# capacity, replacement) and the main-loop steps up to the jump - the
# settle, USB off, the card ejected, the last refusal check.  Real file,
# stub platform; the jump is caught.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: one stub header stands in at every path the file
# includes, relative to itself.
mkdir -p "$B/usb" "$B/rpi" "$B/wifi" "$B/BeebSCSI"
cp "$SRC"/chainboot.c "$SRC"/chainboot.h "$B/"
cp "$HERE"/chainboot_stubs.h "$B/"
for h in Pi1MHz.h videoplayer.h usb/mtp_fs.h rpi/asm-helpers.h rpi/cache.h \
         rpi/rpi.h rpi/systimer.h rpi/h264dec.h wifi/sdio.h BeebSCSI/filesystem.h; do
   echo '#include "chainboot_stubs.h"' > "$B/$h"
done

# -Dfree: the test sees every buffer chainboot gives back.
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -c -Dfree=test_free -o "$B/chainboot.o" "$B/chainboot.c"
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_chainboot.c" "$B/chainboot.o"
"$B/t"
