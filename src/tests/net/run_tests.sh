#!/bin/sh -e
# Host tests for the IP/net service (net_service.c).  Mirrors the firmware
# layout in a temp tree - the REAL net_service.c/net_service.h/services.h plus
# stub Pi1MHz/lwIP/wifi headers - and runs under ASan/UBSan.
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/net_service.c "$SRC"/net_service.h "$SRC"/services.h "$B/"
cp "$SRC"/net_tnfs.c "$SRC"/net_tnfs.h "$B/"
cp "$HERE"/test_net.c "$HERE"/test_tnfs.c "$B/"
cp -r "$HERE"/stubs/. "$B/"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "== net service: full command lifecycle =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN \
    -I"$B" -o "$B/t" \
    "$B/test_net.c" "$B/net_service.c" "$B/net_tnfs.c"
"$B/t"

echo "== TNFS wire codec =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN \
    -I"$B" -o "$B/tnfs" \
    "$B/test_tnfs.c" "$B/net_tnfs.c"
"$B/tnfs"
