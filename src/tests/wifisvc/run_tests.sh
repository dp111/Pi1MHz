#!/bin/sh -e
# Host tests for the ElkWiFi service (wifi_service.c): the WiFi.profile it
# reads at init and writes for *JOIN and *LAPOPT.  The real service and the
# real key=value parser, over an in-memory card and fakes of the WiFi driver,
# net_service and the UEF service.  Run under ASan/UBSan.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

mkdir -p "$B/wifi" "$B/rpi" "$B/scripts"
cp "$SRC"/tests/services/stubs/Pi1MHz.h "$SRC"/tests/services/stubs/ram_emulator.h "$B/"
cp "$SRC"/tests/net/stubs/scripts/*.h "$B/scripts/"
cp -r "$SRC"/tests/fileparser/stubs/. "$B/"
cp "$SRC"/tests/net/stubs/rpi/systimer.h "$B/rpi/"
cp "$SRC"/rpi/fileparser.c "$SRC"/rpi/fileparser.h "$B/rpi/"
cp "$SRC"/wifi/wifi.h "$SRC"/wifi/sdio.h "$SRC"/wifi/sdio_host.h "$B/wifi/"
cp "$SRC"/wifi_service.c "$SRC"/wifi_service.h "$SRC"/uef_service.h \
   "$SRC"/services.h "$SRC"/config.h "$SRC"/net_service.h "$B/"
cp -r "$HERE"/stubs/. "$B/"
cp "$HERE"/test_wifisvc.c "$B/"

echo "== ElkWiFi service: WiFi.profile =="
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" \
    "$B/test_wifisvc.c" "$B/wifi_service.c" "$B/rpi/fileparser.c"
"$B/t"
echo "WIFISVC TESTS PASSED"
