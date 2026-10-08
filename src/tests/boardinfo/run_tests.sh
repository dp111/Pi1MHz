#!/bin/sh -e
# Host tests for rpi/info.c's board-revision decisions: the USB host/device
# choice for an unknown revision, the revision cache, and which mailbox bound
# the boot-critical queries use.  Real info.c, stub mailbox.  Under
# ASan/UBSan.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# Mirror the tree layout: the real info.c, info.h, mailbox.h and rpi.h; a
# stub base.h and Pi1MHz.h.
mkdir -p "$B/rpi"
cp "$SRC"/rpi/info.c "$SRC"/rpi/info.h "$SRC"/rpi/mailbox.h "$SRC"/rpi/rpi.h "$B/rpi/"
cp "$HERE"/boardinfo_stubs.h "$B/"
echo '#include "boardinfo_stubs.h"' > "$B/rpi/base.h"
echo '#include "boardinfo_stubs.h"' > "$B/Pi1MHz.h"

gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -c -o "$B/info.o" "$B/rpi/info.c"
gcc -std=gnu2x -Wall -Wextra -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -I"$B/rpi" -o "$B/t" "$HERE/test_boardinfo.c" "$B/info.o"
"$B/t"
