#!/bin/sh -e
# The RAM probe (ramprobe.c) on the real services port (services_emulator.c) and a cycle-stepped
# BBC: kc-beeb, a Master 128 with Acorn's ROMs.  Neither is in this repository, so the run needs
#   KC_BEEB    a kc-beeb core (machine.c and the rest)
#   KC_ROMS    the MOS 3.20 ROMs (mos.rom; default: roms/mos3.20 beside the core's checkout)
# Without them it prints SKIPPED (run_all.sh reports it) and exits 0.  ASan and UBSan.
#
#   run_tests.sh           the end-to-end test, then tools/bbcprobe.py over HTTP to bbcsim
#   run_tests.sh PORT      just serve bbcsim on 127.0.0.1:PORT, to try a client against it
#   RAMPROBE_QUICK=1       the short end-to-end run
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}

# The probe's 6502 code is helpers 22 and 23 in 6502code.asm; the tests run the committed
# firmware/Pi1MHz/6502code.bin.  With beebasm about (on PATH, or BEEBASM), check that bin is what
# the source assembles to, so an edit to one can't drift from the other.
CODE_BIN="$SRC/../firmware/Pi1MHz/6502code.bin"
export RAMPROBE_6502CODE="$CODE_BIN"
GEN=$(mktemp -d)
trap 'rm -rf "$GEN"' EXIT
BEEBASM=${BEEBASM:-$(command -v beebasm || true)}
if [ -n "$BEEBASM" ]; then
   echo "== 6502code.bin is up to date with 6502code.asm =="
   mkdir -p "$GEN/firmware/Pi1MHz" "$GEN/6502code"
   cp "$SRC/../6502code/6502code.asm" "$GEN/6502code/"
   (cd "$GEN/6502code" && "$BEEBASM" -i 6502code.asm >/dev/null)
   if ! cmp -s "$GEN/firmware/Pi1MHz/6502code.bin" "$CODE_BIN"; then
      echo "6502code.bin is stale: run 6502code/build.sh" >&2
      exit 1
   fi
else
   echo "== SKIPPED: no beebasm, so 6502code.bin isn't checked against 6502code.asm =="
fi

# The API's request parsing alone, on a recording stub of ramprobe.c: no kc-beeb needed.
echo "== ramprobe_api.c request parsing =="
gcc -std=gnu2x -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -I"$HERE/stubs" -o "$GEN/api" "$HERE/test_api.c" "$SRC/ramprobe_api.c" "$SRC/lib/textparse.c"
"$GEN/api"

if [ -z "${KC_BEEB:-}" ] || [ ! -f "$KC_BEEB/machine.c" ]; then
   echo "== ramprobe: SKIPPED: set KC_BEEB to a kc-beeb core and KC_ROMS to its MOS 3.20 ROMs =="
   exit 0
fi
KC_ROMS=${KC_ROMS:-$(dirname "$(dirname "$(dirname "$KC_BEEB")")")/roms/mos3.20}
if [ ! -f "$KC_ROMS/mos.rom" ]; then
   echo "== ramprobe: SKIPPED: set KC_BEEB to a kc-beeb core and KC_ROMS to its MOS 3.20 ROMs =="
   exit 0
fi
B=$(mktemp -d)
trap 'rm -rf "$B" "$GEN"' EXIT
mkdir "$B/kc"
cp "$KC_BEEB"/* "$B/kc/"
# kc-beeb has nothing on FRED and JIM: give them to the test's board.
python3 -I - "$B/kc/machine.c" <<'E'
import sys
p = sys.argv[1]
s = open(p).read()
def rep(a, b):
    global s
    assert s.count(a) == 1, a
    s = s.replace(a, b)
rep("    (void)m;\n    (void)addr;\n    return 0xff;", "    return board_read(m, addr);")
rep("INLINE uint8_t fred_jim_read(struct machine *m, uint16_t addr)",
    "extern uint32_t board_now;\nuint8_t board_read(struct machine *m, uint16_t addr);\n"
    "void board_write(struct machine *m, uint16_t addr, uint8_t val);\n"
    "INLINE uint8_t fred_jim_read(struct machine *m, uint16_t addr)")
rep("IO_AFTER_TICK();  // FRED, JIM: 1 MHz, nothing listening",
    "board_now = cyc; board_write(m, addr, val); IO_AFTER_TICK();  // FRED, JIM: the board")
rep("v = fred_jim_read(m, addr);", "board_now = cyc; v = fred_jim_read(m, addr);")
# and the Pi's nIRQ, ORed with the VIAs'
rep("INLINE uint8_t machine_irq_level(const struct machine *m)\n{\n    return m->sysvia.irq | m->usrvia.irq;",
    "extern uint8_t board_irq;\nINLINE uint8_t machine_irq_level(const struct machine *m)\n{\n    return m->sysvia.irq | m->usrvia.irq | board_irq;")
open(p, 'w').write(s)
E
# The firmware's own: the probe, the services port, the helper select and the JIM window.
cp -r "$HERE"/stubs/* "$B/"
for f in services_emulator.c services.h ramprobe.c ramprobe.h ramprobe_api.c ramprobe_api.h \
         helpers.c helpers.h ram_emulator.c ram_emulator.h; do cp "$SRC/$f" "$B/"; done
mkdir -p "$B/lib"; cp "$SRC"/lib/textparse.c "$SRC"/lib/textparse.h "$B/lib/"
cp "$HERE"/board.h "$HERE"/test_ramprobe.c "$HERE"/bbcsim.c "$B/"
# -no-pie: ram_emulator.c sizes JIM RAM from the address of _end (board.h's mem_info).
CC="gcc -std=gnu2x -g -D_GNU_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie -w -I$B -I$B/kc"
OBJS="$B/services_emulator.c $B/ramprobe.c $B/ramprobe_api.c $B/helpers.c $B/ram_emulator.c $B/lib/textparse.c"
$CC -O2 -o "$B/bbcsim" "$B/bbcsim.c" $OBJS
if [ -n "$1" ]; then
   exec "$B/bbcsim" "$KC_ROMS" "$1"
fi
echo "== ramprobe =="
$CC -O1 -o "$B/t" "$B/test_ramprobe.c" $OBJS
"$B/t" "$KC_ROMS"
echo "== bbcprobe.py over HTTP, to bbcsim =="
python3 -I "$HERE/test_bbcprobe.py" "$B/bbcsim" "$KC_ROMS" "$SRC/../tools/bbcprobe.py"
