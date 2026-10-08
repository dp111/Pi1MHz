#!/bin/sh -e
# Host tests for the FujiNet device (src/fujinet) under ASan/UBSan: every
# device driven through fujibus_answer() as whole packets, with storage on
# a temporary directory (fn_store_host.c).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== fujinet device =="
# cJSON is vendored (fujinet-nio's pinned commit): built on its own, without
# our warning set, and with the firmware's nesting limit (src/CMakeLists.txt).
CJSON_DEFS=-DCJSON_NESTING_LIMIT=64
gcc -std=gnu2x -w -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    $CJSON_DEFS -c "$SRC/fujinet/cJSON/cJSON.c" -o "$B/cJSON.o"
gcc -std=gnu2x -w -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    $CJSON_DEFS -c "$SRC/fujinet/cJSON/cJSON_Utils.c" -o "$B/cJSON_Utils.o"
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC/fujinet" -I"$HERE" -o "$B/t" "$B/cJSON.o" "$B/cJSON_Utils.o" \
    "$HERE/test_fujinet.c" "$HERE/fn_store_host.c" "$HERE/fake_tnfs.c" "$HERE/fake_net.c" "$SRC/net_tnfs.c" \
    $(ls "$SRC"/fujinet/*.c | grep -v fn_store_fatfs.c) -lm
"$B/t"
echo "FUJINET TESTS PASSED"
