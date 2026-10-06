#!/bin/sh -e
# Host tests for the Music 5000 recording flush against card remounts
# (Review 2026-10-06 C1): a BBC reset or a jukebox runs filesystemReset()
# while the sliced WAV flush holds its FIL open.  Real M5000_emulator.c,
# real BeebSCSI/filesystem.c and real FatFs on a RAM disk; only the
# platform around them (audio sink, config, poll table) is stubbed.
# Under ASan/UBSan.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: filesystem.c includes "../rpi/rpi.h" and friends
# relative to itself.
mkdir -p "$B/rpi" "$B/BeebSCSI/fatfs"
cp "$SRC"/M5000_emulator.c "$SRC"/M5000_emulator.h "$SRC"/config.h \
   "$SRC"/videoplayer.h "$B/"
cp "$SRC"/rpi/audio.h "$SRC"/rpi/gpio.h "$SRC"/rpi/info.h "$SRC"/rpi/byteorder.h \
   "$SRC"/rpi/base.h "$SRC"/rpi/mailbox.h "$SRC"/rpi/rpi.h "$SRC"/rpi/systimer.h \
   "$SRC"/rpi/fileparser.h "$B/rpi/"
cp "$SRC"/BeebSCSI/filesystem.c "$SRC"/BeebSCSI/filesystem.h "$SRC"/BeebSCSI/debug.h \
   "$SRC"/BeebSCSI/scsi.h "$SRC"/BeebSCSI/cpuspecific.h "$B/BeebSCSI/"
cp "$SRC"/BeebSCSI/fatfs/ff.c "$SRC"/BeebSCSI/fatfs/ff.h "$SRC"/BeebSCSI/fatfs/ffunicode.c \
   "$SRC"/BeebSCSI/fatfs/diskio.h "$B/BeebSCSI/fatfs/"
# The firmware's FatFs configuration, plus f_mkfs to format the RAM disk.
sed 's/^#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
   "$SRC"/BeebSCSI/fatfs/ffconf.h > "$B/BeebSCSI/fatfs/ffconf.h"
# The shared struct lives at a fixed low address on the Pi; on the host it
# is an ordinary object the test owns.
sed 's/^static Pi1MHz_t \* const Pi1MHz = (Pi1MHz_t \*) 0x100;/extern Pi1MHz_t * const Pi1MHz;/' \
   "$SRC"/Pi1MHz.h > "$B/Pi1MHz.h"
grep -q '^extern Pi1MHz_t \* const Pi1MHz;' "$B/Pi1MHz.h"
cp "$HERE"/test_m5000rec.c "$B/"

# shift-base: music5000_to_s16 left-shifts a negative int64 (well defined in
# GCC, which the firmware is built with); not what this suite is about.
gcc -std=gnu2x -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" \
    "$B/test_m5000rec.c" "$B/M5000_emulator.c" "$B/BeebSCSI/filesystem.c" \
    "$B/BeebSCSI/fatfs/ff.c" "$B/BeebSCSI/fatfs/ffunicode.c"
"$B/t"
