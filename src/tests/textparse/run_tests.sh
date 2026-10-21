#!/bin/sh -e
# Host tests for the shared text parsers (src/lib/textparse.h): the port and
# strict dotted-quad parsers several services use.  Run under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

gcc -std=gnu2x -Wall -Wextra -Wconversion -Wsign-conversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -o "$B/t" "$HERE/test_textparse.c" "$SRC/lib/textparse.c"
"$B/t"
