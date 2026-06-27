#!/bin/bash
#
# One-command build for a single platform, using the CMake presets.
#
#   ./build.sh              # rpi3, release  (defaults)
#   ./build.sh rpi          # rpi,  release
#   ./build.sh rpi3 debug   # rpi3, debug
#   ./build.sh rpi  debug   # rpi,  debug
#
# Each config builds out-of-source in src/build/<preset>. Configure runs only
# the first time; after that it just does a fast incremental build.

set -e
cd "$(dirname "$0")/.."          # src/ (where CMakePresets.json lives)

PLATFORM=${1:-rpi3}
MODE=${2:-release}

case "$PLATFORM" in
    rpi|rpi3) ;;
    *) echo "usage: $0 [rpi|rpi3] [release|debug]"; exit 1 ;;
esac

if [ "$MODE" = "debug" ]; then
    PRESET="${PLATFORM}-debug"
else
    PRESET="${PLATFORM}"
fi

NCPUS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# Configure only if this preset's build dir doesn't exist yet.
[ -f "build/${PRESET}/CMakeCache.txt" ] || cmake --preset "${PRESET}"

cmake --build --preset "${PRESET}" -j"${NCPUS}"
