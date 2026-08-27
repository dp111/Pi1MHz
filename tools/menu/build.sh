#!/bin/bash
# Build the LaserVision disc-menu image (scsi0.dat for /BeebVFS0).
#
# MENU.txt  - BBC BASIC source (ASCII listing)
# BOOT.txt  - !BOOT: *BASIC <cr> CH."MENU" <cr>
#
# Tokenising uses basictool (github.com/ZornsLemma/basictool) - the
# tool of record, ROM-exact.  Point $BASICTOOL at the binary if it is
# not on PATH.
set -e
cd "$(dirname "$0")"
BT=${BASICTOOL:-basictool}

"$BT" -t --output-binary MENU.txt MENU.tok
# round-trip check: detokenised output must match the source
"$BT" --input-tokenised -a MENU.tok - | sed 's/^ *//' | \
    diff - <(tr -d '\r' < MENU.txt | sed 's/^ *//')

python3 ../make_vfs_menu.py scsi0.dat '!BOOT=BOOT.txt' 'MENU=MENU.tok'
md5sum scsi0.dat
