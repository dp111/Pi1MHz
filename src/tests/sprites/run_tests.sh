#!/bin/sh -e
# Host tests for the VDU 23,27 sprite store in framebuffer/primitives.c: the
# real primitives.c against a fake screen, under ASan/UBSan.  The sprite code
# runs in the VDU drain (IRQ context), so the test also counts every heap
# call it makes (--wrap) and requires zero.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

gcc -std=gnu2x -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC/framebuffer" \
    -Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc \
    -o "$B/t" "$HERE/test_sprites.c" "$SRC/framebuffer/primitives.c" -lm
"$B/t"
echo "SPRITE TESTS PASSED"
