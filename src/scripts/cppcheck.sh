#!/usr/bin/env bash
#
# Run cppcheck on the Pi1MHz sources using the CMake compile database.
#
# Two problems with third-party libs (lwIP, TinyUSB) are handled here:
#
#  1. They are include_directories(SYSTEM ...) in CMake, so they appear as
#     -isystem in compile_commands.json.  cppcheck won't search -isystem
#     dirs to resolve  #include "lwip/..." / "tusb.h", giving endless
#     [missingInclude] info lines.  We rewrite -isystem -> -I so they
#     resolve, and -i them so their code is never reported on.
#
#  2. cppcheck's own preprocessor does not define __GNUC__, so TinyUSB's
#     compiler detection (tusb_compiler.h) falls through to
#       #error "Compiler attribute porting is required"
#     which is FATAL to preprocessing.  The real build uses arm-none-eabi-gcc
#     (a GCC), so we inject -D__GNUC__ to take TinyUSB's GCC branch - this
#     both removes the #error and makes cppcheck's view match the build.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_root="$(cd "$here/.." && pwd)"                 # .../Pi1MHz/src
db="$here/compile_commands.json"                   # produced by CMake
db_fixed="$here/compile_commands.cppcheck.json"

if [ ! -f "$db" ]; then
  echo "error: $db not found." >&2
  echo "Configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and build." >&2
  exit 1
fi

# Rewrite the database: -isystem -> -I, and inject the compiler macros that
# cppcheck does not provide on its own.  Handles both the "command" string
# and "arguments" array forms of compile_commands.json.
python3 - "$db" "$db_fixed" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
DEFS = ["-D__GNUC__=12", "-D__GNUC_MINOR__=2", "-D__GNUC_PATCHLEVEL__=0"]
db = json.load(open(src))
for e in db:
    if "command" in e:
        e["command"] = e["command"].replace("-isystem ", "-I") + " " + " ".join(DEFS)
    if "arguments" in e:
        out, it = [], iter(e["arguments"])
        for a in it:
            if a == "-isystem":
                out.append("-I" + next(it))      # "-isystem","X" -> "-IX"
            else:
                out.append(a.replace("-isystem ", "-I"))
        e["arguments"] = out + DEFS
json.dump(db, open(dst, "w"), indent=1)
PY

# --suppress on the libraries: -i stops their .c being *checked*, but
# diagnostics found in their *headers* (pulled into your files) are still
# reported - these path suppressions silence those too.
exec cppcheck \
  --project="$db_fixed" \
  --enable=warning,style,performance,portability,unusedFunction \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  --suppress='*:*/fatfs/*' \
  --suppress='*:*/tinyusb/*' \
  --suppress='*:*/lwip/*' \
  -i "$src_root/wifi/lwip" \
  -i "$src_root/usb/tinyusb" \
  "$@"
