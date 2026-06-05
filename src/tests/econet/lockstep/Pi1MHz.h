#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct { uint8_t Memory[512]; uint8_t *JIM_ram; uint8_t JIM_ram_size; } Pi1MHz_t;
extern Pi1MHz_t *const Pi1MHz;
#define JIM_RAM_STEP (16u*1024u*1024u)
#define DISC_RAM_SIZE (2u*JIM_RAM_STEP)
#define DISC_RAM_BASE ((uint32_t)(((size_t)Pi1MHz->JIM_ram_size)*JIM_RAM_STEP)-DISC_RAM_SIZE)
typedef void (*func_ptr)(void);
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data);
void Pi1MHz_Register_Poll(func_ptr f);
#define CLEAR_IRQ 0
#define ASSERT_IRQ 1
void Pi1MHz_SetnIRQ(bool irq);
#define PI1MHZ_IRQ_SRC_HARDDISC 0u
#define PI1MHZ_IRQ_SRC_ECONET   1u
void Pi1MHz_SetnIRQ_src(uint8_t src, bool assert_irq);
