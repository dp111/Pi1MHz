#!/bin/sh -e
# Host tests for the USB host mouse (usb_mouse.c): the steps of 4 the Beeb
# reads, the clamp and packing, and which mouse is read.  Real file, stub
# TinyUSB HID host.  Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: the file includes "usb/tusb_config.h" and
# "rpi/asm-helpers.h" relative to itself, and <tusb.h> from the path.
mkdir -p "$B/usb" "$B/rpi"
cp "$SRC"/usb_mouse.c "$SRC"/usb_mouse.h "$B/"
cp "$HERE"/stubs/usb/tusb_config.h "$B/usb/"
cp "$HERE"/stubs/rpi/asm-helpers.h "$B/rpi/"
cp "$HERE"/stubs/tusb.h "$B/"

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$HERE/test_usb_mouse.c" "$B/usb_mouse.c"
"$B/t"
