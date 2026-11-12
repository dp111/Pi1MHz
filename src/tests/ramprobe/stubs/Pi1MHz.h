#pragma once
/* Host-test stub of Pi1MHz.h, for the RAM probe's tests: the real ramprobe.c,
   services_emulator.c, helpers.c and ram_emulator.c, on kc-beeb's bus.  Memory[0-255] is FRED as the BBC reads
   it and Memory[256-511] is the &FD00 window, as on the board. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct { uint8_t Memory[512]; uint8_t *JIM_ram; size_t page_ram_addr; size_t byte_ram_addr; uint8_t JIM_ram_size; } Pi1MHz_t;
extern Pi1MHz_t *const Pi1MHz;

#define JIM_RAM_STEP (16u*1024u*1024u)
#define DISC_RAM_SIZE (2u*JIM_RAM_STEP)
#define DISC_RAM_BASE ((uint32_t)(((size_t)Pi1MHz->JIM_ram_size)*JIM_RAM_STEP)-DISC_RAM_SIZE)

#define NOINIT_SECTION
#define PAGE_SIZE 0x100
#define LOG_INFO(...) ((void)0)
extern uint8_t fx_register[256];
/* The fields helpers.c's BREAK forensics touch. */
typedef struct { uint32_t rst_us, init_end_us, helper_us; int32_t margin_min_us; uint8_t helper_pending; } Pi1MHz_break_t;
extern volatile Pi1MHz_break_t Pi1MHz_break;
#define WRITE_FRED   0
#define READ_FRED    1
#define WRITE_JIM    2
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
void Pi1MHz_MemoryWrite_FIQ(uint32_t addr, uint8_t data);
void Pi1MHz_MemoryWritePage(uint32_t addr, const void *data);
void Pi1MHz_nIRQ_ASSERT(uint8_t src);
void Pi1MHz_nIRQ_CLEAR(uint8_t src);
extern uint32_t Pi1MHz_now_us;
#define CLEAR_NMI 0
#define ASSERT_NMI 1
void Pi1MHz_SetnNMI(bool nmi);
