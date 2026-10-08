#!/bin/sh -e
# Host tests for the serial modem (serial_modem.c) under ASan/UBSan: the AT
# command set, dial strings, dialling outcomes, online data and the +++
# escape, against fakes of the serial redirect and net_service's C API.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== serial modem =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -o "$B/t" "$HERE/test_modem.c" "$SRC/serial_modem.c"
"$B/t"
echo "MODEM TESTS PASSED"
