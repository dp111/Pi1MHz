#!/bin/sh -e
# Host tests for the CYW43 NVRAM condenser in src/wifi/sdio.c (Review
# 2026-10-06 D3): the buffer the firmware allocates must hold the condensed
# image for every input, including a last line with no newline.
#
# sdio.c cannot be compiled whole on the host, so extract.awk (shared with
# the webserver tests) pulls the condenser and the allocation-size helper
# out VERBATIM; the extraction fails loudly if either is no longer found.
# The test mallocs exactly what the firmware mallocs, so ASan sees any
# write past it.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

awk -v fns="sdio_cyw43_condense_nvram,sdio_cyw43_condensed_nvram_capacity" \
    -f "$SRC/tests/webserver/extract.awk" "$SRC/wifi/sdio.c" > "$B/nvram.inc"

gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/test_nvram" "$HERE/test_nvram.c"
"$B/test_nvram"
echo "NVRAM TESTS PASSED"
