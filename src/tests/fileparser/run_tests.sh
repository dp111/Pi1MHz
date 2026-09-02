#!/bin/sh -e
# Host tests for the shared key=value parser (src/rpi/fileparser.c).
# Real parser, stub card: the suite supplies the file it reads and inspects
# whatever it writes back.  Run under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

mkdir -p "$B/rpi"
cp "$SRC"/rpi/fileparser.c "$SRC"/rpi/fileparser.h "$B/rpi/"
cp -r "$HERE"/stubs/. "$B/"
cp "$HERE"/test_fileparser.c "$B/"

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$B/test_fileparser.c" "$B/rpi/fileparser.c"
"$B/t"
