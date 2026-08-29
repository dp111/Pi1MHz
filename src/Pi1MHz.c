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
#include "services.h"
#include "net_service.h"
#include "helpers.h"
#include "mouseredirect.h"
#include "videoplayer.h"
#include "usb.h"
#include "wifi/wifi.h"
#include "AUN/aun_emulator.h"
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
   /* The services port: FAT/SD commands plus the ranges other services
      (AUN) claim via services_register(). */
   {"Services",services_emulator_init, 0xA6, 1 },
   {"Videoplayer",videoplayer_init, 0x00, 1},  // start before frame buffer , but after filesystem
   {"Framebuffer",fb_emulator_init, 0xA0, 1},
   {"Mouseredirect",mouse_redirect_init, 0xAC, 1 },
   {"usb",usb_init, 0x00, 1 },
   {"wifi",wifi_emulator_init, 0x00, 1 },
   {"aun",aun_emulator_init, 0x00, 1 },
   /* IP sockets on the services port (commands 45-79). After wifi and aun so
      its poll runs once lwIP has drained inbound frames; off unless
      net_enable=1 in Pi1MHz.cfg. */
   {"net",net_service_init, 0x00, 1 },
   {"Teletext",teletext_emulator_init, 0x10, 1 },  // Acorn Teletext Adapter at &FC10
   /* Last, so its poll callback re-arms the watchdog only after every other
      emulator has had its turn - a poll that stops responding still trips it. */
   {"Watchdog",watchdog_init, 0x00, 1 }
};

#define NUM_EMULATORS (sizeof(emulator)/sizeof(emulator_list))

const char *Pi1MHz_EmulatorName(unsigned int idx)
{
   return (idx < NUM_EMULATORS) ? emulator[idx].name : "?";
}

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
   // The compose-from-shadow makes this a critical section: the FIQ writes
   // the NEIGHBOUR byte of the same VPU word (the SCSI data port &FC40
   // pairs with the status byte &FC41 the main loop writes). A FIQ landing
   // between the neighbour read below and the two stores makes one side
   // write back a stale byte: a reverted data byte corrupts a transfer
   // (ADFS "Bad sum"/"Bad map"), a reverted status byte leaves BSY set in
   // the VPU word while the ARM copy says busfree - VFS then polls that
   // stale status forever (the post-BREAK *VFS wedge). ~4 instructions of
   // CPSID here; the FIQ itself cannot be preempted, so masking only this
   // side makes both directions atomic.
   unsigned int cpsr = _disable_interrupts_cspr();
   uint32_t other = Pi1MHz->Memory[addr ^ 1u] | 0xff00u;
   uint32_t mine  = 0xff00u | data;

   Pi1MHz_Memory_VPU[addr>>1] = (addr & 1u) ? ((mine << 16) | other)
                                            : (mine | (other << 16));

   Pi1MHz->Memory[addr] = data;
   _restore_cpsr(cpsr);
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

/* /status forensics: the per-source assert mask and the real pin level
   (0 = line pulled low = interrupting the Beeb). A wedge with the mask
   clear but the line low - or the reverse - is a driver-side bug; both
   clear and high exonerates the Pi's IRQ path entirely. */
uint32_t Pi1MHz_nIRQ_diag(void)
{
   return Pi1MHz_nirq_mask |
          ((RPI_GpioBase->GPLEV0 & NIRQ_MASK) ? (1u << 31) : 0u);
}

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
      videoplayer_vsync_flip();   /* plane pointers, during blanking: tear-free */
      mouse_redirect_move_mouse();
      fb_process_flash();
      screen_plane_commit();      /* last: the two above only mark their planes */
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
   watchdog_boot_kick();       /* the card mount inside here can be slow */
   config_load("/Pi1MHz/Pi1MHz.cfg");
   watchdog_boot_kick();
   RPI_BootStage(BOOT_STAGE_CONFIG);

   /* Report the previous attempt, now the config is up.  Anything short of
      BOOT_STAGE_RUNNING means the last boot died there - and unlike the
      serial log, this survives the reset and does not depend on how far the
      64 KB TX ring had drained. */
   {
      boot_stage_t prev = RPI_BootStagePrevious();
      if (prev != 0u && prev != BOOT_STAGE_RUNNING)
         LOG_WARN("PREVIOUS BOOT DIED AT STAGE %u\r\n", (unsigned int)prev);
   }

   RPI_IRQBase->Disable_IRQs_1 = 0x200; // Disable USB IRQ which can be left enabled
   RPI_PropertySetWord(0x00038030,12,1); // Set domain 12 ISP
   {
      uint32_t *ico = (uint32_t *)0x20002000;
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

   // make sure we aren't causing an interrupt.  On a BBC RST this runs
   // again while nIRQ may currently be ASSERTED (some source - SCSI, AUN,
   // teletext - had it driven low at the instant of reset).  Force the pin
   // back to an input directly: zeroing Pi1MHz_nirq_mask first and then
   // calling Pi1MHz_nIRQ_CLEAR() would see no asserted->idle transition
   // (old==0, new==0) and never touch GPFSEL, leaving the pin an output
   // driving nIRQ low into the freshly-reset Beeb.
   Pi1MHz_nirq_mask = 0;
   RPI_SetGpioPinFunction(NIRQ_PIN, FS_INPUT);
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
         /* Each emulator's init is well under the boot timeout, but the
            sequence as a whole is not - so feed the dog between them. */
         watchdog_boot_kick();
         RPI_BootDetail(i + 1u);   /* a death here names emulator[i] on the next boot */
         if (emulator[i].enable == 1) emulator[i].init(i, emulator[i].address);
         /* watchdog_init() is what takes ownership of the boot watchdog -
            it either registers the kicking poll or stands the dog down. If
            the config disabled this entry ("Watchdog_addr=-1", the documented
            way to disable any device) its init never ran, so the 15 s boot
            watchdog stays armed with nothing to feed it: the Pi full-resets,
            reboots, re-arms, and resets again for ever, recoverable only by
            editing the SD card on another machine. Stand it down here. */
         else if (emulator[i].init == watchdog_init) watchdog_stop();
      }
   RPI_BootStage(BOOT_STAGE_EMULATORS);
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
   RPI_BootStage(BOOT_STAGE_INFO);
}
// cppcheck-suppress unusedFunction
/* Published once per poll-loop pass; see the loop in kernel_main. */
uint32_t Pi1MHz_now_us;

/* Poll-loop cycle profiler.  Off by default; set to 1, rebuild, and the
   firmware prints a per-callback cycle breakdown once after
   POLL_PROFILE_PASSES passes and then stops.  Uses the ARM1176 cycle counter
   (CP15 c15,c12) rather than RPI_GetSystemTime(), because the system timer is
   a Strongly-Ordered peripheral read - reading it per callback would cost more
   than most callbacks do. */
#define POLL_PROFILE 0
#define POLL_PROFILE_PASSES 200000u

#if (__ARM_ARCH >= 7)
/* Cortex-A53 (kernel7.img): the ARM1176 CP15 c15 performance-monitor
   registers do NOT exist on the A53 and every access faults as an Undefined
   Instruction - this crashed every kernel7 boot at STAGE 2, latent until a
   Pi Zero 2 was actually run (kernel7 had only ever been built, never
   booted, on real A53 silicon).  The c15 counter was purely an arm1176
   micro-optimisation to dodge the Strongly-Ordered system-timer read per
   callback; here just use the 1 MHz system timer.  poll_ticks is measured in
   microseconds, so POLL_TICKS_PER_MS is 1000 and the reported duration and
   the 50 ms threshold below both come out correct without further scaling. */
#define POLL_TICKS_PER_MS 1000u
static inline uint32_t poll_ticks(void) { return RPI_GetSystemTime(); }
static void poll_ticks_start(void) { }

#else
/* ARM1176 (kernel.img): the low-overhead CP15 c15 cycle counter, ticking
   once per 64 processor cycles (the /64 divider keeps a 32-bit counter from
   wrapping inside any interval we care about: ~275 s at 1 GHz).  A CP15
   register read costs a couple of cycles against 47 for RPI_GetSystemTime(),
   which is a Strongly-Ordered peripheral load - and the slow-poll check
   wants a timestamp per callback, so seven peripheral reads per pass were
   costing ~330 cycles, over 10% of the idle loop, purely to police a 50 ms
   threshold. */
#define POLL_TICKS_PER_MS 15625u          /* 1 GHz / 64 / 1000 */

static inline uint32_t poll_ticks(void)
{
   uint32_t v;
   __asm volatile ("mrc p15,0,%0,c15,c12,1" : "=r" (v));
   return v;
}

static void poll_ticks_start(void)
{
   /* enable counters, reset them, reset CCNT, /64 divider on */
   uint32_t ctrl = 0x000Fu;
   __asm volatile ("mcr p15,0,%0,c15,c12,0" :: "r" (ctrl) : "memory");
}
#endif

#define POLL_SLOW_TICKS (50u * POLL_TICKS_PER_MS)

/* Longest single run of each poll callback since last read, for /status:
   the way to find out which poller is starving the audio DMA. */
static uint32_t poll_max_ticks[NUM_EMULATORS];

/* ticks -> microseconds, exact for any POLL_TICKS_PER_MS (15625 neither
   divides by 1000 nor is divisible by it - truncating either way skews
   the figure by 4%). */
static inline uint32_t poll_ticks_to_us(uint32_t ticks)
{
   return (uint32_t)((uint64_t)ticks * 1000u / POLL_TICKS_PER_MS);
}

uint32_t Pi1MHz_poll_max_us(unsigned int idx, bool reset)
{
   if (idx >= Pi1MHz_polls_max)
      return 0;
   uint32_t t = poll_max_ticks[idx];
   if (reset)
      poll_max_ticks[idx] = 0;
   return poll_ticks_to_us(t);
}

unsigned int Pi1MHz_poll_count(void)
{
   return Pi1MHz_polls_max;
}

#if POLL_PROFILE
#if (__ARM_ARCH >= 7)
#error "POLL_PROFILE uses the ARM1176 CP15 c15 cycle counter, which faults on the A53 (kernel7). Profile on kernel.img (rpi) instead."
#endif
static uint32_t poll_prof_cycles[NUM_EMULATORS];
static uint32_t poll_prof_overhead;
static uint32_t poll_prof_passes;

static inline uint32_t poll_prof_ccnt(void)
{
   uint32_t v;
   __asm volatile ("mrc p15,0,%0,c15,c12,1" : "=r" (v));
   return v;
}

static void poll_prof_start(void)
{
   /* enable counters + reset CCNT; divider bit (8) left clear so CCNT counts
      every cycle rather than every 64th */
   uint32_t ctrl = 0x0007u;
   __asm volatile ("mcr p15,0,%0,c15,c12,0" :: "r" (ctrl) : "memory");
}

static void poll_prof_report(void)
{
   uint32_t total = poll_prof_overhead;
   for (unsigned int i = 0; i < Pi1MHz_polls_max; i++)
      total += poll_prof_cycles[i];

   LOG_INFO("POLL PROFILE over %lu passes, %u callbacks\r\n",
            (unsigned long)poll_prof_passes, (unsigned int)Pi1MHz_polls_max);
   for (unsigned int i = 0; i < Pi1MHz_polls_max; i++)
      LOG_INFO("  idx %2u @%08lx: %6lu cycles/pass\r\n", i,
               (unsigned long)(uintptr_t)Pi1MHz_poll_table[i],
               (unsigned long)(poll_prof_cycles[i] / poll_prof_passes));
   LOG_INFO("  loop overhead: %6lu cycles/pass\r\n",
            (unsigned long)(poll_prof_overhead / poll_prof_passes));
   LOG_INFO("  TOTAL: %lu cycles/pass\r\n",
            (unsigned long)(total / poll_prof_passes));

   /* What does one system-timer read actually cost?  It is a
      Strongly-Ordered peripheral load, so the core cannot proceed until the
      peripheral bus answers - and the pollers call it repeatedly per pass. */
   {
      uint32_t t0 = poll_prof_ccnt();
      uint32_t sink = 0u;
      for (unsigned int i = 0; i < 1000u; i++)
         sink += RPI_GetSystemTime();
      LOG_INFO("  RPI_GetSystemTime: %lu cycles each (sink %lu)\r\n",
               (unsigned long)((poll_prof_ccnt() - t0) / 1000u),
               (unsigned long)(sink & 1u));
   }
}
#endif

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

   /* Take over any inherited countdown rather than switching it off.  A
      kernel.now chain-boot inherits the previous kernel's armed watchdog (the
      PM block does not reset on a warm jump); stopping it used to leave the
      machine unguarded for the whole of boot, which is exactly when a failed
      chain-boot dies.  See watchdog_boot_kick(). */
   watchdog_boot_kick();
   RPI_BootStage(BOOT_STAGE_ENTRY);

   /* Before any property request: a chain-boot inherits the VideoCore's
      mailbox state, and a stale reply desynchronises every later call. */
   RPI_MailboxInit();
   RPI_BootStage(BOOT_STAGE_MAILBOX);

   enable_MMU_and_IDCaches(0);
   RPI_BootStage(BOOT_STAGE_MMU);

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
      RPI_BootStage(BOOT_STAGE_HEAP);
   }

   init_hardware();

   filesystemInitialise(0); // default filesystem
   filesystemInitialiseVFS(0);

   init_emulator();
#if POLL_PROFILE
   poll_prof_start();
#else
   poll_ticks_start();
#endif
   RPI_BootStage(BOOT_STAGE_RUNNING);

   bool oldreset = Pi1MHz_is_rst_active();
   uint32_t main_poll_loops = 0u;
   do {
      if ( Pi1MHz_is_rst_active() )
      {
         if (oldreset == false)
         {
            LOG_INFO("Reset detected\r\n");
            RPI_BootDetail(0xFEu);  /* re-init pass marker: a death in config_load shows FE */
            init_emulator();
            /* Re-stamp RUNNING: without this the session runs forever at
               "stage 7" after a BREAK re-init and every later runtime death
               is misreported as an init death. */
            RPI_BootStage(BOOT_STAGE_RUNNING);
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
#if POLL_PROFILE
      {
         uint32_t pass_start = poll_prof_ccnt();
         uint32_t mark = pass_start;

         for (size_t i = 0, n = Pi1MHz_polls_max; i < n; i++) {
            uint32_t after;
            Pi1MHz_poll_table[i]();
            after = poll_prof_ccnt();
            poll_prof_cycles[i] += after - mark;
            mark = after;
         }
         poll_prof_overhead += poll_prof_ccnt() - mark;

         if (++poll_prof_passes >= POLL_PROFILE_PASSES) {
            poll_prof_report();
            poll_prof_passes = 0u;
            poll_prof_overhead = 0u;
            for (unsigned int i = 0; i < NUM_EMULATORS; i++)
               poll_prof_cycles[i] = 0u;
         }
      }
#else
      /* One peripheral clock read per pass, published for the pollers whose
         deadlines are measured in milliseconds or longer - see
         Pi1MHz_now_us.  Anything needing sub-pass precision (teletext field
         phases, SDIO command timeouts) still reads the timer itself. */
      Pi1MHz_now_us = RPI_GetSystemTime();

      uint32_t before_ticks = poll_ticks();
      for (size_t i=0 , n=Pi1MHz_polls_max ; i<n; i++ )
      {
         func_ptr poll_fn = Pi1MHz_poll_table[i];

            RPI_BootDetail((uint32_t)(i + 1u) << 8);  /* runtime hang -> names the callback */
            poll_fn();
            {
               uint32_t after_ticks = poll_ticks();
               uint32_t duration_ticks = after_ticks - before_ticks;
               before_ticks = after_ticks;

               if (duration_ticks > poll_max_ticks[i])
                  poll_max_ticks[i] = duration_ticks;
               if (duration_ticks > POLL_SLOW_TICKS) {
               LOG_INFO("Slow poll callback idx=%u duration_us=%lu\r\n",
                        (unsigned int)i,
                        (unsigned long)poll_ticks_to_us(duration_ticks));
               }
            }
      }
#endif

      RPI_BootDetail(0u);
      main_poll_loops++;
      if ((main_poll_loops % 10000000u) == 0u) {
         LOG_INFO("Main poll heartbeat loops=%lu callbacks=%u\r\n",
                  (unsigned long)main_poll_loops,
                  (unsigned int)Pi1MHz_polls_max);
      }
   } while (1);
}
