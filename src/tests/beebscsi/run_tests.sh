#!/bin/sh -e
# Host tests for the BeebSCSI LUN layer (Review 2026-10-06 S1, S7): FORMAT
# sizing and handle hygiene, the jukebox's started-LUN guard, and where the
# Beeb's storage lives (storage=usb: a second RAM disk as the USB drive).  Real
# BeebSCSI/scsi.c, BeebSCSI/filesystem.c, rpi/fileparser.c and real FatFs
# on a RAM disk; only the bus, the F-code layer and the platform around
# them are stubbed.  f_open/f_close are wrapped to count handles left open.
# Under ASan/UBSan.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: filesystem.c and scsi.c include "../rpi/rpi.h",
# "../videoplayer.h" and friends relative to themselves.
mkdir -p "$B/rpi" "$B/BeebSCSI/fatfs"
cp "$SRC"/config.h "$SRC"/videoplayer.h "$SRC"/harddisc_emulator.h "$SRC"/usb_storage.h "$B/"
cp "$SRC"/rpi/gpio.h "$SRC"/rpi/info.h "$SRC"/rpi/byteorder.h "$SRC"/rpi/base.h \
   "$SRC"/rpi/mailbox.h "$SRC"/rpi/rpi.h "$SRC"/rpi/systimer.h "$SRC"/rpi/lowmem.h \
   "$SRC"/rpi/fileparser.c "$SRC"/rpi/fileparser.h "$B/rpi/"
cp "$SRC"/BeebSCSI/filesystem.c "$SRC"/BeebSCSI/filesystem.h "$SRC"/BeebSCSI/debug.h \
   "$SRC"/BeebSCSI/filesystem_safewrite.c "$SRC"/BeebSCSI/filesystem_storage.c "$SRC"/BeebSCSI/scsi.c "$SRC"/BeebSCSI/scsi.h \
   "$SRC"/BeebSCSI/cpuspecific.h \
   "$SRC"/BeebSCSI/hostadapter.h "$SRC"/BeebSCSI/statusled.h "$SRC"/BeebSCSI/fcode.h \
   "$B/BeebSCSI/"
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
cp "$HERE"/test_beebscsi.c "$B/"
# The shipped default descriptor, for the new-disc MODE SELECT case.
cp "$SRC"/../firmware/Pi1MHz/defscsi.cfg "$B/"

gcc -std=gnu2x -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Wl,--wrap=f_open -Wl,--wrap=f_close \
    -I"$B" -o "$B/t" \
    "$B/test_beebscsi.c" "$B/BeebSCSI/scsi.c" "$B/BeebSCSI/filesystem.c" \
    "$B/BeebSCSI/filesystem_safewrite.c" "$B/BeebSCSI/filesystem_storage.c" "$B/rpi/fileparser.c" \
    "$B/BeebSCSI/fatfs/ff.c" "$B/BeebSCSI/fatfs/ffunicode.c"
"$B/t" "$B/defscsi.cfg"
