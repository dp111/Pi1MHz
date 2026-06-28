#!/bin/sh -e
# Build the lockstep harness against the REAL AUN sources and run the
# full suite. Mirrors the firmware layout (AUN/ subdir + stub headers)
# so the sources' "../" includes resolve to the stubs.
HERE=$(cd "$(dirname "$0")" && pwd)
AUN=${AUN_SRC:-$HERE/../../../AUN}
B=$(mktemp -d)
stubs() {
   mkdir -p "$1/rpi" "$1/wifi" "$1/lwip"
   cp "$HERE"/Pi1MHz.h "$1/"
   cp "$HERE"/config.h "$1/"
   cp "$HERE"/rpi/*.h  "$1/rpi/"
   cp "$HERE"/wifi/*.h "$1/wifi/"
   cp "$HERE"/lwip/*.h "$1/lwip/"
}
mkdir -p "$B/AUN"
cp "$AUN"/aun*.c "$AUN"/aun*.h "$B/AUN/"
cp "$HERE"/main.c "$B/"
stubs "$B"; stubs "$B/AUN"
gcc -std=gnu2x -Wall -Wextra -DAUN_LOCKSTEP_TEST -I"$B" -I"$B/AUN" -o "$B"/harness \
    "$B"/main.c "$B"/AUN/aun_emulator.c "$B"/AUN/aun.c "$B"/AUN/aun_config.c
ECO_HARNESS="$B"/harness python3 "$HERE"/lockstep.py
rm -rf "$B"
