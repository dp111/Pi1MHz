#!/bin/sh -e
# Host tests for the WiFi ROM's clock arithmetic (wifi_time.c): calendar
# conversion and the time_shift shift.  The real wifi_time.c,
# under ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/wifi_time.c "$SRC"/wifi_time.h "$HERE"/test_wifitime.c "$B/"

echo "== WiFi clock: calendar and time shift =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" "$B/test_wifitime.c" "$B/wifi_time.c"
"$B/t"
echo "WIFITIME TESTS PASSED"
