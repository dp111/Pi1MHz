#!/bin/sh -e
# Host tests for filesystemWriteFileSafe() - the verified write-and-swap that
# every .cfg rewrite goes through.  Real function, stub FatFs card, faults
# injected at each step.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: the file includes "fatfs/ff.h" and "../rpi/rpi.h"
# relative to itself.
mkdir -p "$B/BeebSCSI/fatfs" "$B/rpi"
cp "$SRC"/BeebSCSI/filesystem_safewrite.c "$B/BeebSCSI/"
cp "$HERE"/stubs/fatfs/ff.h      "$B/BeebSCSI/fatfs/"
cp "$HERE"/stubs/filesystem.h    "$B/BeebSCSI/"
cp "$HERE"/stubs/rpi/rpi.h       "$B/rpi/"
cp "$HERE"/test_safewrite.c "$B/"

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -I"$B/BeebSCSI" -o "$B/t" \
    "$B/test_safewrite.c" "$B/BeebSCSI/filesystem_safewrite.c"
"$B/t"
