#!/bin/sh -e
# Host tests for src/chapters.c: chapters.txt parsing and lookup, with the
# real tables for British Garden Birds (CAV) and Domesday National B (CLV).
# Under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

gcc -std=gnu2x -Wall -Wextra -Wconversion -Wsign-conversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -DHERE="\"$HERE\"" -o "$B/t" \
    "$HERE/test_chapters.c" "$SRC/chapters.c" "$SRC/lib/textparse.c"
"$B/t"
