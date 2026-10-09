#!/bin/sh -e
# Host tests for the USB flash drive (usb_storage.c): the drive taken, the
# read and write paths through the real diskio.c and FatFs as volume "1:",
# chunking, a drive that never answers, one pulled out mid-transfer, block
# sizes FatFs can't use, and the power-on wait for a drive.  Fake drives behind a stub TinyUSB MSC host; the SD
# card is a stub that is never there.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: usb_storage.c includes "usb/tusb_config.h",
# "rpi/systimer.h" and "BeebSCSI/fatfs/ff.h"; diskio.c includes
# "../../rpi/sdcard.h" and "../../usb_storage.h".
mkdir -p "$B/usb" "$B/rpi" "$B/BeebSCSI/fatfs"
cp "$SRC"/usb_storage.c "$SRC"/usb_storage.h "$B/"
cp "$SRC"/BeebSCSI/fatfs/*.c "$SRC"/BeebSCSI/fatfs/*.h "$B/BeebSCSI/fatfs/"
cp "$SRC"/rpi/sdcard.h "$SRC"/rpi/block.h "$B/rpi/"
cp "$HERE"/stubs/usb/tusb_config.h "$B/usb/"
cp "$HERE"/stubs/rpi/systimer.h "$B/rpi/"
cp "$HERE"/stubs/tusb.h "$HERE"/stubs/usb.h "$HERE"/stubs/watchdog.h "$B/"

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_usb_storage.c" "$B/usb_storage.c" \
    "$B/BeebSCSI/fatfs/ff.c" "$B/BeebSCSI/fatfs/ffunicode.c" \
    "$B/BeebSCSI/fatfs/diskio.c"
"$B/t"
