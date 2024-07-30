#!/bin/bash
../tools/vasm/vasmvidcore_std  -Fbin -L Pi1MHzvc.lst -o Pi1MHzvc.asm Pi1MHzvc.s
rm -f ../src/Pi1MHzvc.c
echo -n "__attribute__((aligned(256))) const " >>../src/Pi1MHzvc.c
xxd -i Pi1MHzvc.asm >> ../src/Pi1MHzvc.c
