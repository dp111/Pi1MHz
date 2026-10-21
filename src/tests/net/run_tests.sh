#!/bin/sh -e
# Host tests for the IP/net service (net_service.c).  Mirrors the firmware
# layout in a temp tree - the REAL net_service.c/net_service.h/services.h plus
# stub Pi1MHz/lwIP/wifi headers - and runs under ASan/UBSan.
# NB: set -e here too - the shebang -e is ignored under "sh script.sh".
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/net_service.c "$SRC"/net_service.h "$SRC"/net_dns.h "$SRC"/net_dns.c "$SRC"/services.h "$B/"
mkdir -p "$B/wifi"
cp "$SRC"/wifi/lwip_util.c "$SRC"/wifi/lwip_util.h "$B/wifi/"
cp "$SRC"/net_tnfs.c "$SRC"/net_tnfs.h "$SRC"/net_telnet.c "$SRC"/net_telnet.h "$B/"
cp "$HERE"/test_net.c "$HERE"/test_tnfs.c "$HERE"/test_telnet.c "$B/"
cp -r "$HERE"/stubs/. "$B/"
mkdir -p "$B/lib"
cp "$SRC"/lib/textparse.c "$SRC"/lib/textparse.h "$B/lib/"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "== net service: full command lifecycle =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN \
    -I"$B" -o "$B/t" \
    "$B/test_net.c" "$B/net_service.c" "$B/net_dns.c" "$B/net_tnfs.c" "$B/net_telnet.c" "$B/lib/textparse.c" "$B/wifi/lwip_util.c"
"$B/t"

# Command 58 copies from DISC_RAM_BASE-relative service RAM to an absolute
# JIM offset.  With the default test base of 0 the two are the same number, so
# the asymmetry is only checked by building the suite again with a real base.
echo "== net service: scratch -> public JIM copy, nonzero service base =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN \
    -DDISC_RAM_BASE=0x10000u -DCOPY_PUBLIC_NONZERO_ONLY \
    -I"$B" -o "$B/copy-public" \
    "$B/test_net.c" "$B/net_service.c" "$B/net_dns.c" "$B/net_tnfs.c" "$B/net_telnet.c" "$B/lib/textparse.c" "$B/wifi/lwip_util.c"
"$B/copy-public"

echo "== TNFS wire codec =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN \
    -I"$B" -o "$B/tnfs" \
    "$B/test_tnfs.c" "$B/net_tnfs.c"
"$B/tnfs"

echo "== TELNET filter =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g $SAN \
    -I"$B" -o "$B/telnet" \
    "$B/test_telnet.c" "$B/net_telnet.c"
"$B/telnet"
