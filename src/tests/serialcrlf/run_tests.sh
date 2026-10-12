#!/bin/sh -e
# Host test for outbyte() in src/rpi/armc-cstubs.c: the serial line is a raw
# terminal, so a bare "\n" must go out as "\r\n" (and "\r\n" must not become
# "\r\r\n").
#
# armc-cstubs.c cannot be compiled whole on the host, so outbyte() is pulled
# out VERBATIM; the run fails loudly if it is no longer found.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

tr -d '\r' < "$SRC/rpi/armc-cstubs.c" \
   | awk '/^void outbyte\(char b\)/ { on = 1 } on { print } on && /^}/ { exit }' > "$B/outbyte.inc"
[ -s "$B/outbyte.inc" ] || { echo "outbyte() not found in armc-cstubs.c" >&2; exit 1; }

gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/test_outbyte" "$HERE/test_outbyte.c"
"$B/test_outbyte"
echo "SERIAL CRLF TESTS PASSED"
