#!/bin/sh -e
# Host test of a custom (VDU 23,22) mode's colour set-up: white, the flash
# filter and the flash bank.  Builds the real src/framebuffer/* against the
# vdutest stubs, with screen_set_palette wrapped to record the bank.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
ROOT=$SRC/..
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
D=$SRC/framebuffer
gcc -O1 -g -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I"$D" -I"$SRC" -Wl,--wrap=screen_set_palette -o "$B/test_fbmode" \
    "$HERE/test_fbmode.c" "$ROOT/tools/vdutest/stubs.c" \
    "$D/framebuffer.c" "$D/screen_modes.c" "$D/primitives.c" "$D/fonts.c" "$D/teletext.c" -lm
"$B/test_fbmode"
