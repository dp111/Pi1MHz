#!/bin/bash

# exit on error
set -e

NAME=Pi1MHz_$(date +"%Y%m%d_%H%M")_$USER

DIR=../../releases/${NAME}
mkdir -p "${DIR}/debug"

for MODEL in rpi3 rpi
do
    # compile normal kernel
    ./clobber.sh
    ./configure_${MODEL}.sh
    make -B -j
    # compile debug kernel
    ./clobber.sh
    ./configure_${MODEL}.sh -DDEBUG=1
    make -B -j
done

cp -a ../../firmware/* "${DIR}"

# Create a simple README.txt file
cat >"${DIR}/README.txt" <<EOF
Pi1MHz

(c) 2020  Dominic Plunkett (dp11) and other contributors

  git version: $(grep GITVERSION gitversion.h  | cut -d\" -f2)
build version: ${NAME}
EOF

cd "../../releases/${NAME}"
zip -qr "../${NAME}.zip" .
cd ../..

unzip -l "releases/${NAME}.zip"

