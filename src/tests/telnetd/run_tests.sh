#!/bin/sh -e
# Host tests for the telnet daemon's byte translators (telnetd_proto.c) under
# ASan/UBSan: what a telnet client's keys become on the Beeb's keyboard, the
# negotiation, and what the Beeb's VDU stream becomes on a terminal.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== telnetd protocol =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -o "$B/t" "$HERE/test_telnetd_proto.c" "$SRC/telnetd_proto.c"
"$B/t"
echo "TELNETD TESTS PASSED"
