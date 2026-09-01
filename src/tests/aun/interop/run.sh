#!/bin/sh
# Interop layer: the real AUN engine against a real PiEconetBridge.
#
# OPTIONAL - it needs a compiled econet-hpbridge, which most machines will
# not have. With no bridge binary this prints "skipped" and exits 0, so
# run_tests.sh keeps working everywhere; it is never a hard dependency.
#
# Point it at a bridge with:   PEB_BRIDGE=/path/to/econet-hpbridge
# otherwise the usual places are tried. Build one with:
#   cd PiEconetBridge/utilities && touch .noexplain && make econet-hpbridge
# (.noexplain links without libexplain, which is often not packaged.)
#
# Two things are asserted: interop.c checks the bytes WE emit, and this
# script checks what the BRIDGE made of them - it greps the bridge's debug
# log for the NOTIFY string reassembled from our port-0 datagrams. That
# second half is the point of the layer: it is a third-party implementation
# judging our wire format, not our own model of it.
HERE=$(cd "$(dirname "$0")" && pwd)
AUN=${AUN_SRC:-$HERE/../../../AUN}

find_bridge() {
   # An explicitly-set PEB_BRIDGE wins, but only if it is really runnable -
   # a stale path should fall back to the search rather than "fail" the
   # whole test run with an exec error.
   [ -x "$PEB_BRIDGE" ] && { echo "$PEB_BRIDGE"; return; }
   for p in \
      "$HERE/../../../../../PiEconetBridge/utilities/econet-hpbridge" \
      /usr/local/sbin/econet-hpbridge \
      /usr/sbin/econet-hpbridge
   do
      [ -x "$p" ] && { echo "$p"; return; }
   done
   echo ""
}

BRIDGE=$(find_bridge)
if [ -z "$BRIDGE" ]; then
   echo "interop: skipped (no econet-hpbridge binary; set PEB_BRIDGE=<path>)"
   exit 0
fi
echo "interop: using bridge $BRIDGE"

B=$(mktemp -d)
LOG=$B/bridge.log
cleanup() {
   [ -n "$BPID" ] && kill -9 "$BPID" 2>/dev/null
   rm -rf "$B"
}
trap cleanup EXIT INT TERM

# -l = IP only, no Econet hardware. Debug level 3 prints the NOTIFY lines.
"$BRIDGE" -l -j "$HERE/peb-test.json" -z -z -z > "$LOG" 2>&1 &
BPID=$!

# wait for the AUN listener rather than sleeping blind
i=0
while [ $i -lt 50 ]; do
   grep -q 'Main loop going to sleep' "$LOG" 2>/dev/null && break
   kill -0 "$BPID" 2>/dev/null || { echo "interop: bridge died at startup"; \
      sed -n '1,20p' "$LOG"; exit 1; }
   i=$((i + 1))
   sleep 0.2
done
if [ $i -ge 50 ]; then
   echo "interop: bridge did not come up"; sed -n '1,20p' "$LOG"; exit 1
fi

gcc -std=gnu2x -Wall -Wextra -I"$AUN" -o "$B/interop" \
    "$HERE/interop.c" "$AUN/aun.c" || exit 1
"$B/interop" || { echo "interop: engine-side assertions failed"; exit 1; }

# The bridge's notify watcher flushes the assembled string a few seconds
# after the last character, so give it time before judging.
echo "  waiting for the bridge to flush its NOTIFY buffer..."
i=0
while [ $i -lt 60 ]; do
   grep -q 'NOTIFY: PI' "$LOG" 2>/dev/null && break
   i=$((i + 1))
   sleep 0.25
done

if grep -q 'NOTIFY: PI' "$LOG"; then
   echo "  ok  : the bridge decoded our port-0 datagrams as NOTIFY \"PI\""
   echo "INTEROP PASSED"
   exit 0
fi

echo "  FAIL: the bridge did not reassemble the NOTIFY string"
echo "  --- bridge log (NOTIFY/BRIDGE lines) ---"
grep -E 'NOTIFY|BRIDGE.*port' "$LOG" | tail -15
exit 1
