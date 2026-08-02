#ifndef RAM_EMULATOR_H
#define RAM_EMULATOR_H
/* Test geometry: base 0 so a JIM data offset indexes JIM_ram directly; a
   64 KB region keeps command blocks (placed below it) and buffers in range. */
#define DISC_RAM_BASE 0u
#define DISC_RAM_SIZE 0x10000u
#endif
