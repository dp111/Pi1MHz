#!/bin/bash
#
# Full release build: every platform, debug and release, zipped with a README.
# Uses the CMake presets and builds each config out-of-source in src/build/.

# exit on error
set -e

cd "$(dirname "$0")/.."          # src/ (where CMakePresets.json lives)

NAME=Pi1MHz_$(date +"%Y%m%d_%H%M")_$USER

# detect number of available CPUs for parallel builds
NCPUS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

REL=../releases/${NAME}
mkdir -p "${REL}/debug"

# from-scratch build of the whole matrix
rm -rf build

# debug kernels
for P in rpi rpi3; do
    cmake --preset "${P}-debug"
    cmake --build --preset "${P}-debug" -j"$NCPUS"
done
cp -a ../firmware/kernel* "${REL}/debug"
cp -a ../firmware/kernel* "../firmware/debug"

# release kernels
for P in rpi rpi3; do
    cmake --preset "${P}"
    cmake --build --preset "${P}" -j"$NCPUS"
done
cp -a ../firmware/* "${REL}"

# Create a simple README.txt file
cat >"${REL}/README.txt" <<EOF
Pi1MHz

(c) 2020-2022  Dominic Plunkett (dp11) and other contributors

  git version: $(grep GITVERSION scripts/gitversion.h | cut -d\" -f2)
build version: ${NAME}
EOF

cd "../releases/${NAME}"
zip -qr "../${NAME}.zip" .
cd ../..

unzip -l "releases/${NAME}.zip"
