#ifndef RAM_EMULATOR_H
#define RAM_EMULATOR_H
/* Test geometry: base 0 so a JIM data offset indexes JIM_ram directly; a
   16 MB region includes the real service scratch offset and public buffers. */
#ifndef DISC_RAM_BASE
#define DISC_RAM_BASE 0u
#endif
#define DISC_RAM_SIZE 0x1000000u
#endif
