../../../cppcheck/cppcheck/cppcheck --suppress=missingIncludeSystem --check-level=exhaustive --enable=all --enable=style --inconclusive --inline-suppr  \
 -DFF_CODE_PAGE=437  -i ../usb/tinyusb -i ../wifi/lwip/src/include/ -i ../wifi/lwip $1 ../.
