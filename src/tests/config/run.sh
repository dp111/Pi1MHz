#!/bin/sh -e
# Host build + run of the Pi1MHz.cfg parser test. From tests/config/.
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
mkdir -p "$B/rpi" "$B/BeebSCSI"

printf '#pragma once\n#define LOG_DEBUG(...) ((void)0)\n#define LOG_INFO(...) ((void)0)\n' > "$B/rpi/rpi.h"
printf '#pragma once\n#include <stdint.h>\nuint32_t filesystemReadFile(const char *f, uint8_t **a, unsigned int m);\n' > "$B/BeebSCSI/filesystem.h"

cp "$SRC/config.c" "$SRC/config.h" "$B/"
cp "$HERE/test_config.c" "$B/"

echo "== compile-check config.c (strict) =="
gcc -std=gnu2x -I"$B" -c "$B/config.c" -o "$B/config.o" \
   -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual \
   -Wpointer-arith -Wstrict-prototypes -Wundef -Wredundant-decls

echo "== functional test =="
gcc -std=gnu2x -I"$B" -Wall -Wextra -o "$B/t" "$B/test_config.c" "$B/config.c"
"$B/t"

echo "CONFIG TESTS PASSED"
