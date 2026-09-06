// Host stubs for the bare-metal symbols the framebuffer sources reference.
// Enough to run src/framebuffer/* on a PC against a malloc'd screen buffer;
// nothing here emulates hardware, it only lets the real VDU code link.
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "../../src/rpi/armtimer.h"
#include "../../src/rpi/interrupts.h"
#include "../../src/rpi/screen.h"
#include "../../src/Pi1MHz.h"

// ---- screen buffer -------------------------------------------------------

// screen_allocate_buffer returns a uint32_t address, which a 64-bit malloc
// pointer does not fit in, so map the buffer low.  MAP_32BIT keeps the cast
// in screen_modes.c lossless without needing a 32-bit toolchain.
uint32_t screen_allocate_buffer(uint32_t buffer_size, uint32_t *handle)
{
   *handle = 1;
   void *p = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
   if (p == MAP_FAILED) {
      fprintf(stderr, "vdutest: MAP_32BIT allocation of %u bytes failed\n", buffer_size);
      exit(1);
   }
   memset(p, 0, buffer_size);
   return (uint32_t)(uintptr_t)p;
}

void screen_release_buffer(uint32_t handle) { (void)handle; }

void screen_create_RGB_plane(uint32_t planeno, uint32_t width, uint32_t height,
                             float par, uint32_t scale_height,
                             uint32_t colour_depth, uint32_t buffer)
{
   (void)planeno; (void)width; (void)height; (void)par;
   (void)scale_height; (void)colour_depth; (void)buffer;
}

void screen_plane_enable(uint32_t planeno, bool enable) { (void)planeno; (void)enable; }
void screen_set_vsync(bool enable) { (void)enable; }
void screen_set_palette(uint32_t planeno, uint32_t palette, uint32_t flags)
{ (void)planeno; (void)palette; (void)flags; }
void screen_update_palette_entry(uint32_t entry, uint32_t r, uint32_t g, uint32_t b)
{ (void)entry; (void)r; (void)g; (void)b; }
uint32_t screen_get_palette_entry(uint32_t entry) { (void)entry; return 0; }

// ---- timer / interrupt controller ---------------------------------------
// The VDU queue is drained by fb_process_vdu_queue() only when the ARM timer
// IRQ is pending, so keep that bit permanently set: draining then happens
// whenever the harness asks for it.

static rpi_arm_timer_t   arm_timer;
static rpi_irq_controller_t irq_controller;

void RPI_ArmTimerInit(void) { }
rpi_arm_timer_t *RPI_GetArmTimer(void) { return &arm_timer; }
rpi_irq_controller_t *RPI_GetIrqController(void)
{
   // the field is volatile const in the real header, so write it through
   *(volatile uint32_t *)&irq_controller.IRQ_basic_pending = RPI_BASIC_ARM_TIMER_IRQ;
   return &irq_controller;
}
unsigned int _disable_interrupts_cspr(void) { return 0; }
void _set_interrupts(unsigned int cspr) { (void)cspr; }

// ---- odds and ends -------------------------------------------------------

void _fast_scroll(void *dst, void *src, int num_bytes)
{
   memmove(dst, src, (size_t)num_bytes);
}

size_t helpers_screen_setup(char *helpscreen, size_t helpscreen_size)
{
   if (helpscreen_size) helpscreen[0] = 0;
   return 0;
}

void mouse_redirect_mouseoff(void) { }

void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr func)
{ (void)access; (void)addr; (void)func; }
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { (void)addr; (void)data; }
void Pi1MHz_EmulatedMemoryByte(unsigned int gpio) { (void)gpio; }
