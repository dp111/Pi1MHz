#!/bin/sh -e
# Host check that the helper-0 help screen fits the Beeb's 40 x 25 text
# screen (src/helpers_help.h) with worst-case values substituted.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
# The template is a #define block inside helpers.c; extract.awk (shared with
# the webserver tests) pulls it out verbatim, so the check sees the real text.
awk -v defs="HELPERS_HELP_FMT,HELPERS_HELP_COLUMNS,HELPERS_HELP_ROWS" \
    -f "$SRC/tests/webserver/extract.awk" "$SRC/helpers.c" > "$B/help_fmt.inc"
gcc -std=gnu2x -Wall -Wextra -Wconversion -g -I"$B" -o "$B/test_help_layout" "$HERE/test_help_layout.c"
"$B/test_help_layout"
echo "HELP LAYOUT TESTS PASSED"
