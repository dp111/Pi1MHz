#!/bin/sh -e
# Host tests for the FujiNet device (src/fujinet) under ASan/UBSan: every
# device driven through fujibus_answer() as whole packets, with storage on
# a temporary directory (fn_store_host.c).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== fujinet device =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC/fujinet" -o "$B/t" \
    "$HERE/test_fujinet.c" "$HERE/fn_store_host.c" "$SRC"/fujinet/*.c -lm
"$B/t"
echo "FUJINET TESTS PASSED"
