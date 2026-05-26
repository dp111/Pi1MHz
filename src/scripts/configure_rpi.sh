#!/bin/sh
./clobber.sh
cmake -G "CodeBlocks - Unix Makefiles" "$*" -DCMAKE_TOOLCHAIN_FILE=rpi.cmake ../
