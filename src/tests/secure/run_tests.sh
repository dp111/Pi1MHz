#!/bin/sh -e
# Host tests for the secure service's ABI core (secure_service_core.c).
# PI1MHZ_SSH defaults OFF, so no firmware build compiles that file; this is
# the only thing that does.  Provider-independent, so no stubs are needed -
# the suite supplies its own nts_secure_port.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

mkdir -p "$B/rpi"
cp "$SRC"/secure_service_core.c "$SRC"/secure_service_core.h "$B/"
cp "$SRC"/rpi/byteorder.h "$B/rpi/"
cp "$HERE"/test_secure.c "$B/"

echo "== secure service ABI core =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/t" \
    "$B/test_secure.c" "$B/secure_service_core.c"
"$B/t"

# secure_service.c - the Pi1MHz-side wrapper - is likewise compiled by no
# firmware build while PI1MHZ_SSH is OFF.  It needs the firmware headers, so
# borrow the services stubs and compile it to an object: no link, but every
# warning and type error in it is caught.
echo "== secure service wrapper compiles =="
cp "$SRC"/secure_service.c "$SRC"/secure_service.h "$SRC"/secure_service_wolfssh.h \
   "$SRC"/services.h "$B/"
cp -r "$SRC"/tests/services/stubs/. "$B/"
gcc -std=gnu2x -Wall -Wextra -Wconversion -Wshadow -g \
    -I"$B" -c "$B/secure_service.c" -o "$B/wrapper.o"
echo "  ok: secure_service.c builds warning-free against the firmware headers"

# ...and linked, with the real ABI core and a fake provider, to drive the
# FIQ latch and the poll that answers it.
echo "== secure service wrapper: latch and poll =="
cp "$HERE"/test_wrapper.c "$B/"
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/tw" \
    "$B/test_wrapper.c" "$B/secure_service.c" "$B/secure_service_core.c"
"$B/tw"

# The wolfSSH provider itself - compiled by no firmware build while
# PI1MHZ_SSH is OFF, and wolfSSH is not in the tree - against stand-ins for
# wolfSSH/wolfCrypt, FatFs and the WiFi glue (provider_stubs/) and the net
# suite's lwIP headers.
echo "== wolfSSH provider =="
P=$B/provider
mkdir -p "$P"
cp -r "$HERE"/provider_stubs/. "$P/"
cp -r "$SRC"/tests/net/stubs/lwip "$P/"
cp "$SRC"/secure_service_wolfssh.c "$SRC"/secure_service_wolfssh.h \
   "$SRC"/secure_service_core.h "$HERE"/test_provider.c "$P/"
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$P" -o "$P/t" "$P/test_provider.c" "$P/secure_service_wolfssh.c"
"$P/t"

echo "SECURE TESTS PASSED"
