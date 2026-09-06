#ifndef PI1MHZ_H
#define PI1MHZ_H
/* Host-test stub of the Pi1MHz core surface net_service.c needs. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NOINIT_SECTION
typedef void (*func_ptr)(void);

/* Small JIM window for tests: DISC_RAM_BASE 0, DISC_RAM_SIZE 64 KB (see
   ram_emulator.h), plus room for command blocks below the region. */
#ifdef COPY_PUBLIC_NONZERO_ONLY
#define TEST_JIM_SIZE 0x1100000u
#else
#define TEST_JIM_SIZE 0x1000000u
#endif
typedef struct { uint8_t JIM_ram[TEST_JIM_SIZE]; } Pi1MHz_t;
extern Pi1MHz_t *Pi1MHz;

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data);
void Pi1MHz_Register_Poll(func_ptr function_ptr, const char *name);
void Pi1MHz_nIRQ_ASSERT(uint8_t src);
void Pi1MHz_nIRQ_CLEAR(uint8_t src);
#endif
