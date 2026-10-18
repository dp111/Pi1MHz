#!/bin/sh -e
# Host tests for the FAT service's per-slot directories, kept files and
# directory blocks (fat_service.c commands 2-6, 12, 19, 20, 21, 22): the file
# API the Pi1MHz builds of MMFS and MMFS2 use instead of raw card sectors.
# Real fat_service.c and services_emulator.c, driven through the services
# port as the Beeb drives it, on real FatFs over a RAM disk - formatted
# FAT32, then exFAT (no short names: the made-up 8.3 names).  The bus and
# the rest of the firmware are stubbed.  Under ASan/UBSan.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

mkdir -p "$B/BeebSCSI/fatfs" "$B/rpi" "$B/wifi"
S="$HERE/../services/stubs"
cp "$S/Pi1MHz.h" "$S/ram_emulator.h" "$B/"
cp "$S/BeebSCSI/filesystem.h" "$B/BeebSCSI/"
cp "$S"/rpi/*.h "$B/rpi/"
cp "$S/wifi/webserver.h" "$B/wifi/"
cp "$SRC"/services_emulator.c "$SRC"/fat_service.c "$SRC"/services.h \
   "$SRC"/M5000_emulator.h "$SRC"/fujibus_service.h "$SRC"/config.c "$SRC"/config.h "$B/"
cp "$SRC"/BeebSCSI/filesystem_storage.c "$B/BeebSCSI/"
cp "$SRC"/BeebSCSI/fatfs/ff.c "$SRC"/BeebSCSI/fatfs/ff.h "$SRC"/BeebSCSI/fatfs/ffunicode.c \
   "$SRC"/BeebSCSI/fatfs/diskio.h "$B/BeebSCSI/fatfs/"
# The firmware's FatFs configuration, plus f_mkfs to format the RAM disk.
sed 's/^#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
   "$SRC"/BeebSCSI/fatfs/ffconf.h > "$B/BeebSCSI/fatfs/ffconf.h"
cp "$HERE"/test_fatfiles.c "$B/"

gcc -std=gnu2x -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" \
    "$B/test_fatfiles.c" "$B/services_emulator.c" "$B/fat_service.c" \
    "$B/BeebSCSI/filesystem_storage.c" "$B/config.c" \
    "$B/BeebSCSI/fatfs/ff.c" "$B/BeebSCSI/fatfs/ffunicode.c"
"$B/t"
