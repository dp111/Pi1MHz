#!/bin/sh
./clobber.sh
cmake  "$*" -DCMAKE_TOOLCHAIN_FILE=rpi3.cmake ../
