#!/bin/sh -e
# Host tests for the telnet daemon's byte translators (telnetd_proto.c) under
# ASan/UBSan: what a telnet client's keys become on the Beeb's keyboard, the
# negotiation, and what the Beeb's VDU stream becomes on a terminal.  Then the
# session (telnetd.c, the whole file) against a fake TCP, redirect and config:
# logins, connecting, keys, resets, the ways a session ends.
# TELNETD_TRACE_BASE=<git rev> also runs the session against that revision's
# telnetd.c and compares the two event traces (a refactor must leave them identical).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

echo "== telnetd protocol =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$SRC" -o "$B/t" "$HERE/test_telnetd_proto.c" "$SRC/telnetd_proto.c"
"$B/t"

# -funsigned-char as on the ARM, whose char telnetd.c is written for.
echo "== telnetd session =="
S="-std=gnu2x -Wall -Wextra -g -funsigned-char -I$HERE/session -I$SRC -I$SRC/wifi -Wl,--wrap=tnd_out_scan_merge"
L="$SRC/telnetd_proto.c $SRC/wifi/lwip_util.c $SRC/lib/textparse.c $SRC/net_telnet.c"
gcc $S -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o "$B/s" "$HERE/test_telnetd_session.c" "$SRC/telnetd.c" $L
"$B/s"
if [ -n "$TELNETD_TRACE_BASE" ]; then
   git -C "$SRC" show "$TELNETD_TRACE_BASE:src/telnetd.c" > "$B/old.c"
   cp "$B/old.c" "$SRC/.telnetd_trace_base.c"         # its "" includes resolve from src/
   gcc $S -o "$B/old" "$HERE/test_telnetd_session.c" "$SRC/.telnetd_trace_base.c" $L || { rm -f "$SRC/.telnetd_trace_base.c"; exit 1; }
   rm -f "$SRC/.telnetd_trace_base.c"
   "$B/s" trace > "$B/new.txt"; "$B/old" trace > "$B/old.txt" || true
   if cmp -s "$B/old.txt" "$B/new.txt"; then echo "trace identical to $TELNETD_TRACE_BASE"
   else diff "$B/old.txt" "$B/new.txt" | head -40; echo "TRACE DIFFERS from $TELNETD_TRACE_BASE"; exit 1; fi
fi
echo "TELNETD TESTS PASSED"
