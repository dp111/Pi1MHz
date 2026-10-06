/* Host stub for what rpi/vchiq.c includes besides vchiq.h: one header,
   which run_tests.sh puts at each of the paths it includes.  The test
   (test_vchiq.c) defines everything declared here. */
#ifndef VCHIQ_STUBS_H
#define VCHIQ_STUBS_H

#include <stdbool.h>
#include <stdint.h>

/* base.h: the doorbells are read and written, so they need somewhere real */
extern uint8_t fake_periph[0x10000];
#define PERIPHERAL_BASE ((uintptr_t)fake_periph)

/* rpi.h */
#define LOG_INFO(...)  ((void)0)
#define LOG_DEBUG(...) ((void)0)

/* mailbox.h */
typedef enum { TAG_VCHIQ_INIT = 0x48010 } rpi_mailbox_tag_t;
typedef struct {
   rpi_mailbox_tag_t tag;
   uint32_t byte_length;
   union { uint32_t value_32; uint32_t buffer_32[8]; } data;
} rpi_mailbox_property_t;
void RPI_PropertyStart(rpi_mailbox_tag_t tag, uint32_t length);
void RPI_PropertyAdd(uint32_t data);
void RPI_PropertyProcess(bool wait);
rpi_mailbox_property_t *RPI_PropertyGet(rpi_mailbox_tag_t tag);

/* systimer.h */
uint32_t RPI_GetSystemTime(void);

/* asm-helpers.h */
static inline void _data_memory_barrier(void) { __sync_synchronize(); }
static inline void _data_synchronization_barrier(void) { __sync_synchronize(); }

/* screen.h: the GPU-heap allocator */
uint32_t screen_allocate_buffer(uint32_t size, uint32_t *handle);
void screen_release_buffer(uint32_t handle);

#endif
