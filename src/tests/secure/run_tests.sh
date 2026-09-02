#!/bin/sh -e
# Host tests for the secure service's ABI core (secure_service_core.c).
# PI1MHZ_SSH defaults OFF, so no firmware build compiles that file; this is
# the only thing that does.  Provider-independent, so no stubs are needed -
# the suite supplies its own nts_secure_port.
set -e            # also when invoked as "bash run_tests.sh"
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/secure_service_core.c "$SRC"/secure_service_core.h "$B/"
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

echo "SECURE TESTS PASSED"
