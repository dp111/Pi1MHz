/*
    Pi1MHz is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Pi1MHz is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Pi1MHz.  If not, see <http://www.gnu.org/licenses/>.

   Raspberry Pi 1MHz emulator

   The main program and the Fast Interrupt handle the interface side of things

   Emulated devices provided the following functions

   xxx_init() This registers which memory locations the device requires callbacks on and any other
      initialization required  called when reset is active.

   Functions provided

   Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, func_ptr *func_ptr )
         This needs to be called for each memory location that requires a call back
         The function will be run in FIQ mode and use the FIQ stack. If it needs to do anything
         complex e.g. allocate memory this should be put into a queue so the polled function can
         then execute code.
         For access variable use WRITE_FRED WRITE_JIM READ_FRED READ_JIM definitions
         When the function is called the parameter is the GPIO pin status. use GET_DATA and GET_ADDR
         macros

   Pi1MHz_Register_Poll( func_ptr *func_ptr )
         This registers a polling function that is called in a tight loop while idle.
         tasks must yield otherwise the system will lock up.

   Pi1MHz_Memory[]
         This array is used for reads by FIQ function. Tasks must put the data to be read by the
         host in the correct location.

   For reference from mdfs :

   Page &FC (252) - FRED I/O Space
===============================
See mdfs.net/Docs/Comp/BBC/Hardware/FREDaddrs for full details
&FC00-&FC03 Byte-Wide Expansion RAM
&FC04-&FC05 BeebOPL - *** Audio ****
&FC08-&FC0F Ample M2000 MIDI Interface (see also FCF0)
&FC10-&FC13 Teletext Hardware
&FC14-&FC1F Prestel Hardware
&FC20-&FC3F SID Interface  *** Audio ***
&FC20-&FC27 IEEE Interface
&FC28-&FC2F Electron Econet
&FC30-&FC3F Cambridge Ring interface
&FC40-&FC4F Hard Drive Access
&FC50-&FC5F
&FC60-&FC6F Electron Serial
&FC70-&FC7F Electron expansion
&FC80-&FC87 LCD Display Control
&FC88-&FC8F
&FC90-&FC9F Electron sound and speech
&FCA0-&FCAF
&FCB0-&FCBF Electron 6522 VIA expansion
            PRISMA Video System
&FCC0-&FCCF Morley Electronics RAMDisk
&FCC0-&FCCF Electron floppy disk expansion
&FCD0-&FCDB
&FCDC-&FCDF PRES Battery-backed RAM
&FCE0-&FCEF Electron Tube expansion
&FCF0-&FCF7 JGH/ETI MIDI Control (see also FC08)
&FCF8-&FCFB USB port
&FCFC-&FCFF Page-Wide Expansion RAM


Page &FD (253) - JIM I/O Space
==============================
See mdfs.net/Docs/Comp/BBC/Hardware/JIMAddrs for full details
&FD00-&FDFF Page-wide expansion RAM window
&FD40-&FD4F Torch SASI/SCSI Hard Drive Access
&FDFE-&FDFF Reset Test vector

*/

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "rpi/asm-helpers.h"
#include "rpi/auxuart.h"
#include "rpi/cache.h"
#include "rpi/info.h"
#include "rpi/gpio.h"
#include "rpi/interrupts.h"
#include "rpi/screen.h"
#include "rpi/systimer.h"
#include "rpi/armc-cstubs.h"
#include "Pi1MHz.h"

#include "Pi1MHzvc.c"

#include "scripts/gitversion.h"

#include "BeebSCSI/filesystem.h"
#include "config.h"

#define Pi1MHZ_FX_CONTROL 0xCA

// add new emulators to the lists below

#include "ram_emulator.h"
#include "harddisc_emulator.h"
#include "M5000_emulator.h"
#include "BeebSID/BeebSid.h"
#include "rpi/audio.h"
#include "framebuffer/framebuffer.h"
#include "discaccess_emulator.h"
#include "helpers.h"
#include "mouseredirect.h"
#include "videoplayer.h"
#include "usb.h"
#include "wifi/wifi.h"
#include "aun/aun_emulator.h"
#include "teletext_emulator.h"
#include "watchdog.h"

typedef struct {
   const char *name;
   const func_ptr_parameter init;
   uint8_t address;
   uint8_t enable;
} emulator_list;

static emulator_list emulator[] = {
   {"Helpers",helpers_init, 0x88, 1 }, // needs to be before framebuffer so it can write to the screen
   {"Rampage",rampage_emulator_init, 0xFD, 1},
   {"Rambyte",rambyte_emulator_init, 0x00, 1},
   {"Harddisc",harddisc_emulator_init, 0x40, 1},
   {"M5000",M5000_emulator_init, 0, 1},
   /* Default off — enable with BeebSID_addr=0x20 in Pi1MHz.cfg (disables M5000). */
   {"BeebSID", BeebSID_emulator_init, 0x20, 0},
   {"Discaccess",discaccess_emulator_init, 0xA6, 1 },
   {"Videoplayer",videoplayer_init, 0x00, 1},  // start before frame buffer , but after filesystem
   {"Framebuffer",fb_emulator_init, 0xA0, 1},
   {"Mouseredirect",mouse_redirect_init, 0xAC, 1 },
   {"usb",usb_init, 0x00, 1 },
   {"wifi",wifi_emulator_init, 0x00, 1 },
   {"aun",aun_emulator_init, 0x00, 1 },
   {"Teletext",teletext_emulator_init, 0x10, 1 },  // Acorn Teletext Adapter at &FC10
   /* Last, so its poll callback re-arms the watchdog only after every other
      emulator has had its turn - a poll that stops responding still trips it. */
   {"Watchdog",watchdog_init, 0x00, 1 }
};

#define NUM_EMULATORS (sizeof(emulator)/sizeof(emulator_list))

// Memory for VPU to read FRED and JIM
static volatile uint32_t * const Pi1MHz_Memory_VPU = (uint32_t *)Pi1MHz_MEM_BASE;

// Table of polling functions to call while idle
NOINIT_SECTION static func_ptr Pi1MHz_poll_table[NUM_EMULATORS];
// holds the total number of polling functions to call
static uint8_t  Pi1MHz_polls_max;

// *fx register buffer
NOINIT_SECTION uint8_t fx_register[256];

_Static_assert(offsetof(Pi1MHz_t, callback_table) == 0x300, "callback_table must be at offset 0x300 for FIQ.s");

_Static_assert(Pi1MHz_STRUCT_VADDR + offsetof(Pi1MHz_t, callback_table) == Pi1MHz_CB_BASE, "Pi1MHz_CB_BASE must equal &Pi1MHz->callback_table[0]");

_Static_assert(sizeof(Pi1MHz_t) <= 0x2000, "Pi1MHz_t must fit into low memory");

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data)
{
   // One VPU word holds two adjacent bus addresses, so this used to read the
   // word back to preserve the other half. Timed on hardware (ARM1176,
   // 200k iterations, 1MHz system timer):
   //
   //    this function, with the read-back        200 ns
   //    this function, as written below          126 ns
   //    VPU word read alone                       76 ns
   //    VPU word write alone                      16 ns
   //
   // The read-back is avoidable because Pi1MHz->Memory[] already shadows
   // every byte, and every writer of the VPU window maintains it: this
   // function, MemoryWrite16/32, and the assembly MemoryWritePage. So the
   // other half is reconstructed from the shadow instead. That is not a cache
   // that can go stale - it is the shadow the write path already keeps - but
   // it does mean a new writer of the VPU window must maintain both, exactly
   // as the existing ones do.
   //
   // The rest is ordering, and it is worth more than the read-back was. The
   // shadow store must come AFTER the VPU store, not before:
   //
   //    shadow store then VPU store              126 ns
   //    VPU store then shadow store               18 ns
   //    VPU store alone                           17 ns
   //    shadow store alone                         0 ns
   //
   // Pi1MHz_MEM_BASE is Strongly-Ordered, so a pending buffered write must
   // drain before it can issue. Storing the shadow first parks a write in the
   // buffer that the VPU store then has to wait out - 108ns of stall for a
   // store that costs nothing on its own. Reversed, the shadow write is
   // absorbed by the buffer and the function costs no more than the bus
   // access it has to make anyway.
   //
   // The reversal also skews which side sees the update first, in the better
   // direction: the bus now gets the new byte immediately and the CPU-side
   // copy lags by a few ns, rather than the other way round.
   uint32_t other = Pi1MHz->Memory[addr ^ 1u] | 0xff00u;
   uint32_t mine  = 0xff00u | data;

   Pi1MHz_Memory_VPU[addr>>1] = (addr & 1u) ? ((mine << 16) | other)
                                            : (mine | (other << 16));

   Pi1MHz->Memory[addr] = data;
}

void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data)
{
   // addr is always even and Pi1MHz->Memory is page-aligned, so the
   // destination is 2-byte aligned. memcpy + assume_aligned lets the
   // compiler emit a single STRH on every CPU (incl. ARMv6) with no
   // aliasing UB and no -Wcast-align warning.
   uint16_t v = (uint16_t) data;
   memcpy(__builtin_assume_aligned(&Pi1MHz->Memory[addr], 2), &v, sizeof v);

   Pi1MHz_Memory_VPU[addr >> 1] = 0xFF00FF00 | (data&0xFF) | (data<<8);
}

// cppcheck-suppress unusedFunction
void Pi1MHz_MemoryWrite32(uint32_t addr, uint32_t data)
{
   // addr is always a multiple of 4, so the destination is 4-byte aligned.
   // memcpy + assume_aligned -> a single STR on every CPU (incl. ARMv6),
   // no aliasing UB, no -Wcast-align warning.
   memcpy(__builtin_assume_aligned(&Pi1MHz->Memory[addr], 4), &data, sizeof data);

   uint32_t ad = addr >> 1;

   Pi1MHz_Memory_VPU[ad++] = 0xFF00FF00 | (data&0xFF) | (data<<8);
   Pi1MHz_Memory_VPU[ad] = 0xFF00FF00 | (data>>16) | (data>>24)<<16;
}

uint8_t Pi1MHz_MemoryRead(uint32_t addr)
{
   return Pi1MHz->Memory[addr];
}

void Pi1MHz_EmulatedMemoryByte(unsigned int gpio)
{
   Pi1MHz_MemoryWrite(GET_ADDR(gpio), GET_DATA(gpio));
}

// For each location in FRED and JIM which a task wants to be called for
// it must register its interest. Only one task can be called per location
// for access variable use WRITE_FRED WRITE_JIM READ_FRED READ_JIM
void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr function_ptr )
{
   Pi1MHz->callback_table[access+addr] = function_ptr;
}

// For each task that needs to be polled during idle it must register itself.
// is can only register once
void Pi1MHz_Register_Poll( func_ptr function_ptr )
{
   uint8_t i;

   if (function_ptr == NULL)
      return;

   for (i = 0u; i < Pi1MHz_polls_max; ++i) {
      if (Pi1MHz_poll_table[i] == function_ptr)
         return;
   }

   if (Pi1MHz_polls_max >= NUM_EMULATORS) {
      LOG_INFO("Poll registration ignored: table full (%u)\r\n",
               (unsigned int)NUM_EMULATORS);
      return;
   }

   Pi1MHz_poll_table[Pi1MHz_polls_max] = function_ptr;
   Pi1MHz_polls_max++;
}

bool Pi1MHz_is_rst_active(void) {
   return ((RPI_GpioBase->GPLEV0 & NRST_MASK) == 0);
}

/* nIRQ is a shared open-collector line; multiple emulators may want to
 * assert it. Each caller owns one bit of the mask, indexed by its
 * emulator-table slot (the 'instance' passed to <emu>_init), so one
 * emulator releasing its request cannot clear another's.
 *
 * The read-modify-write (and the GPIO function-select RMW inside
 * RPI_SetGpioPinFunction) is guarded because callers run in different
 * contexts - harddisc from the FIQ FRED callbacks, AUN from the main
 * loop - and a FIQ preempting the main loop must not lose an update. */
static volatile uint32_t Pi1MHz_nirq_mask = 0;

inline static void Pi1MHz_SetnIRQ_src(uint8_t src, bool assert_irq)
{
   unsigned int cpsr = _disable_interrupts_cspr();
   uint32_t old  = Pi1MHz_nirq_mask;
   uint32_t mask = old;
   if (assert_irq)
      mask |= (1u << src);
   else
      mask &= ~(1u << src);
   Pi1MHz_nirq_mask = mask;
   /* The pin function depends only on whether ANY source is asserted, so
    * touch GPFSEL only when that changes. This matters: the SCSI DMA loops
    * call us once per byte with the bit already set, so 255 of every 256
    * calls used to do a read-modify-write of GPFSEL - two Strongly-Ordered
    * (uncached, unbuffered) peripheral round trips - writing back the value
    * just read, with FIQ masked throughout. The mask RMW above still has to
    * be guarded, because harddisc really does call this from FIQ context. */
   if ((old != 0u) != (mask != 0u))
      RPI_SetGpioPinFunction(NIRQ_PIN, (mask != 0) ? FS_OUTPUT : FS_INPUT);
   _restore_cpsr(cpsr);
}

void Pi1MHz_nIRQ_ASSERT(uint8_t src) {
   Pi1MHz_SetnIRQ_src(src, true);
}

void Pi1MHz_nIRQ_CLEAR(uint8_t src) {
   Pi1MHz_SetnIRQ_src(src, false);
}

void Pi1MHz_SetnNMI(bool nmi)
{
   RPI_SetGpioPinFunction(NNMI_PIN, nmi?FS_OUTPUT:FS_INPUT);
}

static volatile uint8_t status_addr;

// Enables the beeb to read and write status info
// setup the address for status read write
static void Pi1MHzBus_addr_Status(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   status_addr = data;
   Pi1MHz_MemoryWrite(addr, data); // enable read back
   Pi1MHz_MemoryWrite(addr+1, fx_register[data]);
}

// take data written by the beeb and put it to the correct place
static void Pi1MHzBus_write_Status(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   fx_register[status_addr] = data;
   Pi1MHz_MemoryWrite(addr, data); // enable read back
}

static void Pi1MHzBus_read_Status(unsigned int gpio)
{
   // TODO
}

// cppcheck-suppress unusedFunction
void IRQHandler_main(void) {
   _data_memory_barrier();
   // Check for USB IRQ (IRQ #9 in Enable_IRQs_1)
   if (RPI_GetIrqController()->IRQ_pending_1 & (1 << 9)) {
      tud_int_handler(0);
   }

   RPI_AuxMiniUartIRQHandler();
   if (screen_check_vsync())
   {
      mouse_redirect_move_mouse();
      fb_process_flash();
   }
   // Periodically also process the VDU Queue
   fb_process_vdu_queue();

   _data_memory_barrier();
}

static void init_emulator(void) {
   LOG_INFO("\r\n\r\n**** Raspberry Pi 1MHz Emulator %s %s " BUILD_DATE " ****\r\n\r\n",RELEASENAME, GITVERSION);

   // Load Pi1MHz.cfg from the SD card before any emulator reads its config.
   // All emulator/option keys come from this file via config_get(); only the
   // boot-essential keys read before this point (disk_led_gpio, baud_rate)
   // still come from cmdline.txt. (filesystemReadFile self-mounts the FAT.)
   config_load("/Pi1MHz/Pi1MHz.cfg");

   RPI_IRQBase->Disable_IRQs_1 = 0x200; // Disable USB IRQ which can be left enabled
   RPI_PropertySetWord(0x00038030,12,1); // Set domain 12 ISP
   {
      uint32_t *ico = (uint32_t *)0x20002000;
      //for(int i=0; i<(0x38/4); i++)
      //   LOG_DEBUG("ICO %d %08x\r\n",i,ico[i]);

      ico[0x20/4] = 0x00000000;// disable HVS interrupts going to the VPU
   }

   _enable_interrupts();

   for( uint8_t i=0; i <NUM_EMULATORS; i++)
      {
         // "<name>_addr=0xNN" overrides the FRED base address; a negative
         // value (e.g. "<name>_addr=-1") disables the emulator.
         int ov = config_emulator_override(emulator[i].name, &emulator[i].address);
         if (ov < 0)
            {
               LOG_DEBUG("Disabling %s\r\n", emulator[i].name);
               emulator[i].enable = 0;
            }
         else if (ov > 0)
            {
               LOG_DEBUG("%s address = 0x%02x\r\n", emulator[i].name, emulator[i].address);
               /* Config address enables emulators that default to off (e.g. BeebSID). */
               emulator[i].enable = 1;
            }
      }

   /* BeebSID and M5000 share AUDIO_PIN / rpi_audio — only one may run. */
   {
      uint8_t beebsid_on = 0;
      for (uint8_t i = 0; i < NUM_EMULATORS; i++) {
         if (emulator[i].enable == 1 && strcmp(emulator[i].name, "BeebSID") == 0) {
            beebsid_on = 1;
            break;
         }
      }
      if (beebsid_on) {
         for (uint8_t i = 0; i < NUM_EMULATORS; i++) {
            if (strcmp(emulator[i].name, "M5000") == 0 && emulator[i].enable == 1) {
               LOG_INFO("BeebSID enabled: disabling M5000 (shared audio path)\r\n");
               emulator[i].enable = 0;
            }
         }
      }
   }

   Pi1MHz_polls_max = 0;

   memset(&Pi1MHz->callback_table[0], 0, Pi1MHz_CB_SIZE);
   memset(&Pi1MHz->Memory[0],0,sizeof(Pi1MHz->Memory)); // Clear FRED and JIM memory

   for(int i=255; i>=0; i--)
      Pi1MHz_Memory_VPU[i]=0;             // Clear VPU ram.

   RPI_PropertyStart(TAG_LAUNCH_VPU1, 7);
   RPI_PropertyAdd((uint32_t)Pi1MHzvc_asm); // VPU function
   RPI_PropertyAdd (Pi1MHz_MEM_BASE_GPU); // r0 address of register block in IO space
   RPI_PropertyAdd((PERIPHERAL_BASE_GPU | (Pi1MHz_VPU_RETURN & 0x00FFFFFF) )); // r1

   const char *prop = config_get("Pi1MHznOE");
   if (prop)
   {
      int temp = atoi(prop);
      if (temp == 0)
         RPI_PropertyAdd(0); // r2  No external nOE pin
      else
         RPI_PropertyAdd(1<<(NOE_PIN)); // r2 ( External nOE pin)
   }
   else
      RPI_PropertyAdd(1<<(NOE_PIN)); // r2 ( External nOE pin)

   RPI_PropertyAdd(DATABUS_TO_OUTPUTS); // r3
   RPI_PropertyAdd(TEST_PINS_OUTPUTS | (1<<(NOE_PIN<<3))); // r4
   RPI_PropertyAdd(0); // r5 TEST_MASK
   RPI_PropertyProcess(false);

   RPI_IRQBase->FIQ_control = 0x80 + 67; // doorbell FIQ

   // make sure we aren't causing an interrupt
   Pi1MHz_nirq_mask = 0;
   Pi1MHz_nIRQ_CLEAR(0);
   Pi1MHz_SetnNMI(CLEAR_NMI);

   // Register Status read back
   Pi1MHz_Register_Memory(WRITE_FRED, Pi1MHZ_FX_CONTROL  , Pi1MHzBus_addr_Status );
   Pi1MHz_Register_Memory(WRITE_FRED, Pi1MHZ_FX_CONTROL+1, Pi1MHzBus_write_Status );
   Pi1MHz_Register_Memory( READ_FRED, Pi1MHZ_FX_CONTROL+1, Pi1MHzBus_read_Status );

   /* BeebAudio_Off routes the shared audio pin once here, so whichever PWM
      audio emulator (M5000/BeebSID) runs doesn't have to read it itself. */
   {
      const char *bp = config_get("BeebAudio_Off");
      rpi_audio_mute_beeb(bp && atoi(bp) == 1);
   }

   for( uint8_t i=0; i <NUM_EMULATORS; i++)
      {
         LOG_DEBUG("Init %s at 0x%02x\r\n",emulator[i].name, emulator[i].address);
         if (emulator[i].enable == 1) emulator[i].init(i, emulator[i].address);
      }
}

static uint8_t led_pin;

void Pi1MHz_LED(int led)
{
   if (led_pin!=255)
      RPI_SetGpioValue(led_pin , led);
}

static void init_hardware(void)
{
   // enable overriding default LED option using command.txt
   // depending on the pi use either bcm2708.disk_led_gpio=xx or bcm2709.disk_led_gpio=xx
   const char *prop = get_cmdline_prop("disk_led_gpio");
   if (prop)
   {
      led_pin = (uint8_t)atoi(prop);
      RPI_SetGpioOutput(led_pin);
   }
   else
      led_pin = 255;

   LOG_DEBUG("LED pin %d\r\n",led_pin);

   // Configure our pins as default state as inputs
   // Pins default to inputs so only setup Outputs and alternate modes

   RPI_SetGpioPinFunction(AUDIO_PIN, FS_ALT0); // PWM1

   RPI_SetGpioHi(NOE_PIN);          // disable external data bus buffer
   RPI_SetGpioOutput(NOE_PIN);      // external data buffer nOE pin

   RPI_SetGpioOutput(TEST_PIN);

   RPI_SetGpioLo(NIRQ_PIN);   // Set outputs low ready for interrupts when pin is changed to FS_OUTPUT
   RPI_SetGpioLo(NNMI_PIN);

#ifdef DEBUG
   dump_useful_info();
#endif
}
// cppcheck-suppress unusedFunction
/* Poll-loop timing, for /status.  See the loop in kernel_main. */
uint32_t Pi1MHz_poll_max_us[PI1MHZ_POLL_STATS_MAX];
uint32_t Pi1MHz_poll_over_5ms[PI1MHZ_POLL_STATS_MAX];

_Noreturn void kernel_main(void)
{
   unsigned int baud_rate = 115200;
   const char * const prop = get_cmdline_prop("baud_rate");
   if (prop)
   {
      int b = atoi(prop);
      if (b > 0)
         baud_rate = (unsigned int) b;
   }

   RPI_AuxMiniUartInit( baud_rate );

   /* Before anything slow.  A kernel.now chain-boot inherits the previous
      kernel's armed watchdog - the PM block does not reset on a warm jump -
      and boot takes far longer than any sane timeout, so an inherited
      countdown would reset us part-way up.  See watchdog_stop(). */
   watchdog_stop();

   enable_MMU_and_IDCaches(0);

   // Setup malloc memory
   {
      /* A zero here is not a small problem.  mem_info() returns 0 when its
         mailbox query fails, arm_setup_heap_limit() then rejects it, and
         _sbrk falls back to the 4 MB early limit - which is enough for the
         main loop but not for TinyUSB and lwIP, so the Pi comes up with no
         USB and no network and cannot be reached to be fixed.  That has
         happened, and it cost an SD card swap.

         So: ask more than once, and if the VC still will not answer, use a
         figure low enough to be safe on any GPU split (the smallest leaves
         the ARM 64 MB) and high enough to run the whole system.  Booting
         with a modest heap and a working network beats booting unreachable. */
      uint32_t arm_memory_bytes = mem_info(1);
      unsigned int retry;

      for (retry = 0u; arm_memory_bytes == 0u && retry < 3u; ++retry)
         arm_memory_bytes = mem_info(1);

      if (arm_memory_bytes == 0u)
         arm_memory_bytes = 32u * 1024u * 1024u;

      arm_setup_heap_limit((void *) (uintptr_t) arm_memory_bytes);
   }

   init_hardware();

   filesystemInitialise(0,0); // default filesystem

   init_emulator();
#if 0
   {
   uint32_t *ico = (uint32_t *)0x20002000;
   for(int i=0; i<(0x38/4); i++)
      LOG_DEBUG("ICO %x %08x\r\n",i*4,ico[i]);

   ico[0x20/4] = 0x00000000; // hvs
   ico[0x14/4] = 0x00000000;
   ico[0x1C/4] = 0x00000000; // undefined is required

   for(int i=0; i<(0x38/4); i++)
      ico[i] =0;
   }

   {
      uint32_t *ico = (uint32_t *)0x20002800;
      for(int i=0; i<(0x38/4); i++)
         LOG_DEBUG("IC1 %x %08x\r\n",i*4,ico[i]);
      }

   uint32_t * fsel_reg = &RPI_GpioBase->GPFSEL[0];
   for(int i=0; i<0x94/4; i++)
      LOG_DEBUG("GPIO %x %08x\r\n",i*4,fsel_reg[i]);

      uint32_t * irq_reg = (uint32_t *) 0x2000B200;
      for(int i=0; i<0x24/4; i++)
         LOG_DEBUG("IRQ %x %08x\r\n",i*4,irq_reg[i]);

#endif

   bool oldreset = Pi1MHz_is_rst_active();
   uint32_t main_poll_loops = 0u;
   do {
      if ( Pi1MHz_is_rst_active() )
      {
         if (oldreset == false)
         {
            LOG_INFO("Reset detected\r\n");
            init_emulator();
            oldreset = true;
         }
      } else
      {
         oldreset = false;
      }
      /* One system-timer read per poll, not two.  RPI_GetSystemTime() is a
         Strongly-Ordered peripheral load the core waits on, and each poll's
         "after" timestamp is the next poll's "before", so carrying it over
         halves the round trips across the whole loop. */
      uint32_t before_us = RPI_GetSystemTime();
      for (size_t i=0 , n=Pi1MHz_polls_max ; i<n; i++ )
      {
         func_ptr poll_fn = Pi1MHz_poll_table[i];

            poll_fn();
            {
               uint32_t after_us = RPI_GetSystemTime();
               uint32_t duration_us = after_us - before_us;
               before_us = after_us;

               /* Per-callback worst case, kept for /status.  A logged
                  threshold only answers "did anything block for 50 ms";
                  the question here is which callback owns the tail, and
                  at what size, which needs the whole distribution. */
               if (i < PI1MHZ_POLL_STATS_MAX) {
                  if (duration_us > Pi1MHz_poll_max_us[i])
                     Pi1MHz_poll_max_us[i] = duration_us;
                  if (duration_us > 5000u)
                     Pi1MHz_poll_over_5ms[i]++;
               }
            }
      }

      main_poll_loops++;
      if ((main_poll_loops % 10000000u) == 0u) {
         LOG_INFO("Main poll heartbeat loops=%lu callbacks=%u\r\n",
                  (unsigned long)main_poll_loops,
                  (unsigned int)Pi1MHz_polls_max);
      }
   } while (1);
}
