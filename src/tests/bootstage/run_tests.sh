#!/bin/sh -e
# Host tests for rpi/bootstage.c's chain-boot marker: accepted only for a
# jump that has just happened with no reset in between.  Real file, stub
# hardware.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: the real rpi.h, one stub header at every other
# path bootstage.c includes.
mkdir -p "$B/rpi"
cp "$SRC"/rpi/bootstage.c "$SRC"/rpi/rpi.h "$B/rpi/"
cp "$HERE"/bootstage_stubs.h "$B/"
for h in base.h cache.h lowmem.h systimer.h; do
   echo '#include "bootstage_stubs.h"' > "$B/rpi/$h"
done

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -c -o "$B/bootstage.o" "$B/rpi/bootstage.c"
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_bootstage.c" "$B/bootstage.o"
"$B/t"
