#!/bin/sh -e
# Host tests for the serial redirect (serial_redirect.c) under ASan/UBSan: flow control and
# the escape, BREAK and idle trips, driven through the stub's FRED write callbacks.
# SER_TRACE_BASE=<git rev> also runs the same steps against that revision's file and
# compares the two traces of stub writes (a refactor must leave them identical).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== serial redirect =="
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -o "$B/t" "$HERE/test_serial_redirect.c" "$SRC/serial_redirect.c"
"$B/t"
if [ -n "$SER_TRACE_BASE" ]; then
   git -C "$SRC" show "$SER_TRACE_BASE:src/serial_redirect.c" > "$B/old.c"
   gcc -std=gnu2x -g -I"$SRC" -o "$B/old" "$HERE/test_serial_redirect.c" "$B/old.c"
   "$B/t" trace > "$B/new.txt"; "$B/old" trace > "$B/old.txt" || true
   if cmp -s "$B/old.txt" "$B/new.txt"; then echo "trace identical to $SER_TRACE_BASE"
   else diff "$B/old.txt" "$B/new.txt" | head -40; echo "TRACE DIFFERS from $SER_TRACE_BASE"; exit 1; fi
fi
echo "SERIAL REDIRECT TESTS PASSED"
