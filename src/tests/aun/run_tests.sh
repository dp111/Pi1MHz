#!/bin/sh -e
# Run the whole AUN test stack on a PC. From tests/aun/.
# Builds in a temp tree that mirrors the firmware layout (sources under
# AUN/, stub headers alongside) so the AUN sources resolve their headers
# whether they are included as "Pi1MHz.h" or "../Pi1MHz.h".
HERE=$(cd "$(dirname "$0")" && pwd)
AUN=${AUN_SRC:-$HERE/../../AUN}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

stubs() {   # populate $1 with the lockstep stub headers
   mkdir -p "$1/rpi" "$1/wifi" "$1/lwip"
   cp "$HERE"/lockstep/Pi1MHz.h "$1/"
   cp "$HERE"/lockstep/rpi/*.h  "$1/rpi/"
   cp "$HERE"/lockstep/wifi/*.h "$1/wifi/"
   cp "$HERE"/lockstep/lwip/*.h "$1/lwip/"
}
mkdir -p "$B/AUN"
cp "$AUN"/aun*.c "$AUN"/aun*.h "$B/AUN/"
cp "$HERE"/test_aun.c "$HERE"/test_aun_config.c "$HERE"/test_irq_mask.c "$HERE"/fuzz_engine.c "$HERE"/fuzz_cmd.c "$B/"
stubs "$B"; stubs "$B/AUN"
I="-I$B -I$B/AUN"

echo "== unit: AUN engine =="
gcc -std=gnu2x -Wall -Wextra -Wconversion $I -o "$B/t1" "$B/test_aun.c" "$B/AUN/aun.c"
"$B/t1"

echo "== unit: cmdline parsers =="
gcc -std=gnu2x -Wall -Wextra -Wconversion $I -o "$B/t2" "$B/test_aun_config.c" "$B/AUN/aun_config.c"
"$B/t2"

echo "== unit: nIRQ shared mask (slot-11 width regression) =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -o "$B/t3" "$B/test_irq_mask.c"
"$B/t3"

echo "== fuzz: engine (ASan/UBSan) =="
gcc -std=gnu2x -g -fsanitize=address,undefined -fno-sanitize-recover=all $I -o "$B/f1" "$B/fuzz_engine.c" "$B/AUN/aun.c"
"$B/f1"

echo "== fuzz: command interface (ASan/UBSan) =="
gcc -std=gnu2x -g -fsanitize=address,undefined -fno-sanitize-recover=all $I -o "$B/f2" "$B/fuzz_cmd.c" "$B/AUN/aun_emulator.c" "$B/AUN/aun.c" "$B/AUN/aun_config.c"
"$B/f2"

echo "== lockstep (ROM x C x AUN peer) =="
"$HERE/lockstep/run.sh"

echo "ALL TEST LAYERS PASSED"
