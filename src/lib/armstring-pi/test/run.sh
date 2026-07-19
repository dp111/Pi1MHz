#!/bin/sh
# Build and run the string-routine tests under qemu-system-arm.
# There is no user-mode qemu on the dev box, hence the bare-metal harness.
#
# -mno-unaligned-access is REQUIRED: qemu resets with SCTLR.A set, so the
# alignment faults it raises on GCC's unaligned accesses (ARMv6 defines
# __ARM_FEATURE_UNALIGNED) restart the harness in a loop. Real firmware has
# unaligned access enabled, so this is a harness concern only.
#
# Expected output: "checks=13152 failures=0". The trailing
# "Unsupported SemiHosting SWI 0xdeadbeef" is a known wart in the final
# RESULT print and does not affect the counts above it.
set -e
HERE=$(dirname "$0")
SRC=${1:-$HERE/../memset.S}
arm-none-eabi-gcc -O2 -mcpu=arm1176jzf-s -marm -mno-unaligned-access -ffreestanding -fno-builtin \
    -nostdlib -T "$HERE/link.ld" "$HERE/start.S" "$HERE/test.c" "$SRC" \
    -o "$HERE/test.elf" 2>&1 | grep -v RWX || true
timeout -s TERM 60 qemu-system-arm -M versatilepb -cpu arm1176 -m 128M \
    -nographic -semihosting -kernel "$HERE/test.elf" 2>&1 | cat
echo "[expected: checks=13152 failures=0]"
