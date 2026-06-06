#!/bin/sh -e
# Run the whole econet test stack on a PC. From tests/econet/.
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${ECO_SRC:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== unit: AUN engine =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -I"$SRC" -I"$HERE/lockstep" \
    -o "$B/t1" "$HERE/test_econet_aun.c" "$SRC/econet_aun.c"
"$B/t1"

echo "== unit: cmdline parsers =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -I"$SRC" -I"$HERE/lockstep" \
    -o "$B/t2" "$HERE/test_eco_config.c" "$SRC/econet_config.c"
"$B/t2"

echo "== fuzz: engine (ASan/UBSan) =="
gcc -std=gnu2x -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -I"$HERE/lockstep" -o "$B/f1" "$HERE/fuzz_engine.c" "$SRC/econet_aun.c"
"$B/f1"

echo "== fuzz: command interface (ASan/UBSan) =="
gcc -std=gnu2x -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -I"$HERE/lockstep" -o "$B/f2" "$HERE/fuzz_cmd.c" \
    "$SRC/econet_emulator.c" "$SRC/econet_aun.c" "$SRC/econet_config.c"
"$B/f2"

echo "== lockstep (ROM x C x AUN peer) =="
"$HERE/lockstep/run.sh"

echo "ALL TEST LAYERS PASSED"
