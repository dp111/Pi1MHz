#!/bin/bash
#
# Full release build: every platform, debug and release, zipped with a README.
# Uses the CMake presets and builds each config out-of-source in src/build/.

# exit on error
set -e

cd "$(dirname "$0")/.."          # src/ (where CMakePresets.json lives)

# A release is built from a clean tree at an exact tag, and named after it.
# RELEASE_FORCE=1 overrides both checks (a throw-away snapshot build); the
# name is then the old Pi1MHz_<date>_<user>.
TAG=$(git describe --tags --exact-match HEAD 2>/dev/null || true)
DIRTY=$(git status --porcelain)
if [ -z "$RELEASE_FORCE" ]; then
    if [ -n "$DIRTY" ]; then
        echo "release.sh: the working tree is not clean (git status --porcelain):" >&2
        echo "$DIRTY" | head -10 >&2
        echo "Commit or stash first, or set RELEASE_FORCE=1 for a snapshot build." >&2
        exit 1
    fi
    if [ -z "$TAG" ]; then
        echo "release.sh: HEAD has no exact tag (git tag Vx.yy first)." >&2
        echo "Set RELEASE_FORCE=1 for an untagged snapshot build." >&2
        exit 1
    fi
fi
if [ -n "$TAG" ]; then
    NAME=Pi1MHz_${TAG}
else
    NAME=Pi1MHz_$(date +"%Y%m%d_%H%M")_$USER
fi

# detect number of available CPUs for parallel builds
NCPUS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

REL=../releases/${NAME}
mkdir -p "${REL}/debug"

# from-scratch build of the whole matrix
rm -rf build

# debug kernels
for P in rpi rpi2 rpi3; do
    cmake --preset "${P}-debug"
    cmake --build --preset "${P}-debug" -j"$NCPUS"
done
cp -a ../firmware/kernel* "${REL}/debug"
cp -a ../firmware/kernel* "../firmware/debug"

# release kernels
for P in rpi rpi2 rpi3; do
    cmake --preset "${P}"
    cmake --build --preset "${P}" -j"$NCPUS"
done
cp -a ../firmware/* "${REL}"

# Create a simple README.txt file
cat >"${REL}/README.txt" <<EOF
Pi1MHz

(c) 2020-2026  Dominic Plunkett (dp111) and other contributors
  See CREDITS.md for third-party sources and contributors.

  git version: $(grep GITVERSION scripts/gitversion.h | cut -d\" -f2)
build version: ${NAME}

Install:
  1. Format a micro SD card FAT32.
  2. Copy the entire contents of this folder (not debug/) to the card root:
     bootcode.bin, config.txt, start.elf, kernel*.img, Pi1MHz/ and the rest.
  3. Fit the card, power the BBC on; CTRL-BREAK once the Pi has booted.
  4. Check: type  X%=0 : CALL &FC88  at the BASIC prompt for the help screen.
The debug/ folder holds debug kernels: copy over the kernel*.img only to trace.

Full guide: docs/user/getting-started.md in the source tree (docs/user/).
EOF

cd "../releases/${NAME}"
zip -qr "../${NAME}.zip" .
cd ../..

unzip -l "releases/${NAME}.zip"
