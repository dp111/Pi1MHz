#!/bin/sh -e
# Host tests for the services port and the FAT service interlock.
# Mirrors the firmware layout in a temp tree (real services_emulator.c,
# fat_service.c and services.h; stub Pi1MHz/FatFs headers) and runs the
# suite under ASan/UBSan.
set -e            # also when invoked as "bash run_tests.sh" (shebang flags are ignored then)
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/services_emulator.c "$SRC"/fat_service.c "$SRC"/services.h "$B/"
cp "$SRC"/config.c "$SRC"/config.h "$B/"
cp "$HERE"/test_services.c "$HERE"/test_config.c "$HERE"/fuzz_fat.c "$B/"
cp -r "$HERE"/stubs/. "$B/"

echo "== services port + FAT interlock =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" \
    "$B/test_services.c" "$B/services_emulator.c" "$B/fat_service.c" "$B/config.c"
"$B/t"

echo "== config parser =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/tc" \
    "$B/test_config.c" "$B/config.c"
"$B/tc"

echo "== fuzz: FAT commands (ASan/UBSan) =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/f" \
    "$B/fuzz_fat.c" "$B/services_emulator.c" "$B/fat_service.c" "$B/config.c"
"$B/f"

echo "SERVICES TESTS PASSED"
