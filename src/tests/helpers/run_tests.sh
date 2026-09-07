#!/bin/sh -e
# Host check that the helper-0 help screen fits the Beeb's 40 x 25 text
# screen (src/helpers_help.h) with worst-case values substituted.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
gcc -std=gnu2x -Wall -Wextra -Wconversion -g -I"$SRC" -o "$B/test_help_layout" "$HERE/test_help_layout.c"
"$B/test_help_layout"
echo "HELP LAYOUT TESTS PASSED"
