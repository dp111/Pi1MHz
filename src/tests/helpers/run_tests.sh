#!/bin/sh -e
# Host check that the helper-0 help screen fits the Beeb's 40-column text
# screen (src/helpers.c) with worst-case values substituted, and that each
# user ROM line (helpers 10-15) adds exactly one row.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
# The template is a #define block inside helpers.c; extract.awk (shared with
# the webserver tests) pulls it out verbatim, so the check sees the real text.
awk -v defs="HELPERS_HELP_FMT,HELPERS_HELP_COLUMNS,HELPERS_USER_ROM_NONE,HELPERS_USER_ROM_LINE" \
    -f "$SRC/tests/webserver/extract.awk" "$SRC/helpers.c" > "$B/help_fmt.inc"
gcc -std=gnu2x -Wall -Wextra -Wconversion -g -I"$B" -o "$B/test_help_layout" "$HERE/test_help_layout.c"
"$B/test_help_layout"
echo "HELP LAYOUT TESTS PASSED"
