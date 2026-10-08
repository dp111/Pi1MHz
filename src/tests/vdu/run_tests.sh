#!/bin/sh -e
# Host test of VDU driver state the ROM-comparison suite cannot see: cell
# metrics against the cursors and the screen buffer, the teletext line
# state across a scroll, VDU 19/20, and the VDU 24/31 range checks (review
# 2026-10-06 V1-V4, V6-V8).  Builds the real src/framebuffer/* against the
# vdutest stubs, with the screen buffer guarded and palette writes recorded.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
ROOT=$SRC/..
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
D=$SRC/framebuffer
gcc -O1 -g -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I"$D" -I"$SRC" \
    -Wl,--wrap=screen_allocate_buffer,--wrap=screen_update_palette_entry -o "$B/test_vdu" \
    "$HERE/test_vdu.c" "$ROOT/tools/vdutest/stubs.c" \
    "$D/framebuffer.c" "$D/screen_modes.c" "$D/primitives.c" "$D/fonts.c" "$D/teletext.c" -lm
"$B/test_vdu"
