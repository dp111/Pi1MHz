#!/bin/sh -e
# Host tests for the MTP session lifecycle in usb/mtp_fs.c (Review
# 2026-10-06 U3): what survives an unplug, a re-enumeration, a Device Reset
# and a card swap - the open session, an unfinished upload's ".part", the
# LUN host lock, the kernel.now buffer, and the object cache.  Real file,
# real FatFs on a RAM disk, real TinyUSB headers; the driver calls and the
# filesystem layer around FatFs are stubbed.  Under ASan/UBSan.
#
# Needs the TinyUSB submodule (git submodule update --init src/usb/tinyusb)
# for the MTP class headers.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
TUSB=$SRC/usb/tinyusb/src
if [ ! -f "$TUSB/class/mtp/mtp.h" ]; then
   echo "FAIL: $TUSB is empty - git submodule update --init src/usb/tinyusb" >&2
   exit 1
fi
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: one stub header stands in at every project path
# the file includes, relative to itself.
mkdir -p "$B/usb" "$B/rpi" "$B/wifi" "$B/scripts" "$B/bsp" "$B/BeebSCSI/fatfs"
cp "$SRC"/usb/mtp_fs.c "$SRC"/usb/mtp_fs.h "$SRC"/usb/tusb_config.h "$B/usb/"
cp "$HERE"/mtp_stubs.h "$B/"
for h in BeebSCSI/filesystem.h rpi/byteorder.h services.h wifi/sdio.h \
         rpi/asm-helpers.h rpi/systimer.h Pi1MHz.h rpi/cache.h rpi/rpi.h \
         rpi/exceptions.h wifi/webserver.h chainboot.h scripts/gitversion.h \
         bsp/board_api.h; do
   echo '#include "mtp_stubs.h"' > "$B/$h"
done
cp "$SRC"/BeebSCSI/fatfs/ff.c "$SRC"/BeebSCSI/fatfs/ff.h "$SRC"/BeebSCSI/fatfs/ffunicode.c \
   "$SRC"/BeebSCSI/fatfs/diskio.h "$B/BeebSCSI/fatfs/"
# The firmware's FatFs configuration, plus f_mkfs to format the RAM disks.
sed 's/^#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
   "$SRC"/BeebSCSI/fatfs/ffconf.h > "$B/BeebSCSI/fatfs/ffconf.h"

# -Dmalloc/-Dfree/-Df_readdir in mtp_fs.c only: the test sees the
# kernel.now buffer, and counts the directory reads of a cache walk.
# TinyUSB's headers are third-party: -isystem, as in the firmware build.
CFLAGS="-std=gnu2x -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all"
gcc $CFLAGS -I"$B" -I"$B/usb" -isystem "$TUSB" \
    -Dmalloc=test_malloc -Dfree=test_free -Df_readdir=test_f_readdir \
    -c -o "$B/mtp_fs.o" "$B/usb/mtp_fs.c"
gcc $CFLAGS -I"$B" -I"$B/usb" -isystem "$TUSB" -o "$B/t" \
    "$HERE/test_mtp_session.c" "$B/mtp_fs.o" \
    "$B/BeebSCSI/fatfs/ff.c" "$B/BeebSCSI/fatfs/ffunicode.c"
"$B/t"
