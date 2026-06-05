#!/bin/sh -e
# Build the lockstep harness against the REAL econet sources and run
# the full suite. Override ECO_SRC to point at the Pi1MHz src tree
# (defaults to three levels up, i.e. running from tests/econet/lockstep).
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${ECO_SRC:-$HERE/../../..}
B=$(mktemp -d)
cp "$SRC"/econet_aun.c "$SRC"/econet_aun.h \
   "$SRC"/econet_config.c "$SRC"/econet_config.h \
   "$SRC"/econet_emulator.c "$SRC"/econet_emulator.h "$B"
cp "$HERE"/main.c "$HERE"/Pi1MHz.h "$B"
mkdir -p "$B"/rpi "$B"/wifi "$B"/lwip
cp "$HERE"/rpi/*.h "$B"/rpi/
cp "$HERE"/wifi/*.h "$B"/wifi/
cp "$HERE"/lwip/*.h "$B"/lwip/
gcc -std=gnu2x -Wall -Wextra -I"$B" -o "$B"/harness \
    "$B"/main.c "$B"/econet_emulator.c "$B"/econet_aun.c "$B"/econet_config.c
ECO_HARNESS="$B"/harness python3 "$HERE"/lockstep.py
rm -rf "$B"
