#pragma once
/* Host-test stub of Pi1MHz.h, for the RAM probe's tests: the real ramprobe.c and
   services_emulator.c, on kc-beeb's bus.  Memory[0-255] is FRED as the BBC reads
   it and Memory[256-511] is the &FD00 window, as on the board. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct { uint8_t Memory[512]; uint8_t *JIM_ram; uint8_t JIM_ram_size; } Pi1MHz_t;
extern Pi1MHz_t *const Pi1MHz;

#define JIM_RAM_STEP (16u*1024u*1024u)
#define DISC_RAM_SIZE (2u*JIM_RAM_STEP)
#define DISC_RAM_BASE ((uint32_t)(((size_t)Pi1MHz->JIM_ram_size)*JIM_RAM_STEP)-DISC_RAM_SIZE)

#define NOINIT_SECTION
#define WRITE_FRED   0
#define READ_FRED    1
#define Pi1MHz_MEM_PAGE (1u << 8)

#define GET_DATA(gpio) ((gpio) & 0xffu)
#define GET_ADDR(gpio) (((gpio) >> 8) & 0xffffu)
#define TEST_GPIO(addr, data) ((unsigned int)(((addr) << 8) | (data)))

typedef void (*func_ptr)(void);
typedef void (*callback_func_ptr)(unsigned int);

void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr function_ptr);
void Pi1MHz_EmulatedMemoryByte(unsigned int gpio);
void Pi1MHz_Register_Poll(func_ptr function_ptr, const char *name);
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data);
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data);
void Pi1MHz_nIRQ_ASSERT(uint8_t src);
void Pi1MHz_nIRQ_CLEAR(uint8_t src);
extern uint32_t Pi1MHz_now_us;
#define CLEAR_NMI 0
#define ASSERT_NMI 1
void Pi1MHz_SetnNMI(bool nmi);
