#!/bin/sh -e
# Host tests for chainboot.c: the image check, the hand-over (padding,
# capacity, replacement) and the main-loop steps up to the jump - the
# settle, USB off, the card ejected, the last refusal check, the video shut
# down, the audio DMA stopped.  Real file, stub platform; the jump is
# caught.  Under ASan/UBSan.
#
# Twice: as built, and with chainboot_refusal weakened in a copy of the
# object so the test can supply the refusal (-DREFUSAL_HOOK) and drive the
# give-up path, which nothing in the firmware triggers at present.  -O0 is
# what lets that work - chainboot.c's own calls then go through the symbol -
# and the give-up cases fail, not pass, if it ever stops working.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: one stub header stands in at every path the file
# includes, relative to itself.
mkdir -p "$B/usb" "$B/rpi" "$B/wifi" "$B/BeebSCSI"
cp "$SRC"/chainboot.c "$SRC"/chainboot.h "$B/"
cp "$HERE"/chainboot_stubs.h "$B/"
for h in Pi1MHz.h videoplayer.h usb/mtp_fs.h rpi/asm-helpers.h rpi/cache.h \
         rpi/rpi.h rpi/exceptions.h rpi/systimer.h rpi/h264dec.h rpi/audio.h wifi/sdio.h BeebSCSI/filesystem.h; do
   echo '#include "chainboot_stubs.h"' > "$B/$h"
done

# -Dfree: the test sees every buffer chainboot gives back.
gcc -std=gnu2x -Wall -Wextra -g -O0 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -c -Dfree=test_free -o "$B/chainboot.o" "$B/chainboot.c"
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_chainboot.c" "$B/chainboot.o"
"$B/t"
objcopy --weaken-symbol=chainboot_refusal "$B/chainboot.o" "$B/chainboot_weak.o"
gcc -std=gnu2x -Wall -Wextra -g -DREFUSAL_HOOK \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t_refuse" "$HERE/test_chainboot.c" "$B/chainboot_weak.o"
"$B/t_refuse"
