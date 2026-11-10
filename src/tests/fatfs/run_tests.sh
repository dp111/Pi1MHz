#!/bin/sh -e
# Host tests for the Pi1MHz fixes to FatFs R0.16 (ffconf.h PI1MHZ_FF_*):
# f_rename into its own subtree, f_expand across the volume wrap, the exFAT
# f_mkdir unwind and the exFAT mount checks.  Real FatFs with the firmware's
# configuration (plus f_mkfs) on a RAM disk.  Under ASan/UBSan.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

mkdir -p "$B/BeebSCSI/fatfs"
cp "$SRC"/BeebSCSI/fatfs/ff.c "$SRC"/BeebSCSI/fatfs/ff.h "$SRC"/BeebSCSI/fatfs/ffunicode.c \
   "$SRC"/BeebSCSI/fatfs/diskio.h "$B/BeebSCSI/fatfs/"
# The firmware's FatFs configuration, plus f_mkfs to format the RAM disk.
sed 's/^#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
   "$SRC"/BeebSCSI/fatfs/ffconf.h > "$B/BeebSCSI/fatfs/ffconf.h"

gcc -std=gnu2x -Wall -Wextra -g -O1 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -I"$B/BeebSCSI/fatfs" -o "$B/t" \
    "$HERE/test_fatfs.c" "$B/BeebSCSI/fatfs/ff.c" "$B/BeebSCSI/fatfs/ffunicode.c"
"$B/t"
