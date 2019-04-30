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

   Pi1MHz_Register_Memory(int rnw, int fred, int jim, int addr, func_ptr *func_ptr )
         This needs to be called for each memory location that requires a call back
         The function will be run in FIQ mode and use the FIQ stack. If it needs to do anything
         complex e.g. allocate memory this should be put into a queue so the polled function can
         then execute code.
         when the function is called parameter is the GPIO pin status. use GET_DATA and GET_ADDR
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
            PRSIMA Video System
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
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "rpi/arm-start.h"
#include "rpi/rpi-aux.h"
#include "rpi/cache.h"
#include "rpi/performance.h"
#include "rpi/info.h"
#include "rpi/rpi-gpio.h"
#include "rpi/rpi-interrupts.h"
#include "Pi1MHz.h"
#include "scripts/gitversion.h"
#include "BeebSCSI/filesystem.h"

// add new emulators to the lists below

#include "ram_emulator.h"
#include "harddisc_emulator.h"
#include "M5000_emulator.h"

static const func_ptr emulator_inits[] = {
   ram_emulator_init,
   harddisc_emulator_init,
   M5000_emulator_init
};

uint8_t *JIM_ram; // 480M Bytes of RAM for pizero

uint32_t JIM_ram_size; // Size of JIM ram in 16Mbyte steps

// Memory for FRED and JIM so that the FIQ can read the result quickly
uint8_t * const Pi1MHz_Memory = (uint8_t *)Pi1MHz_MEM_BASE;

// Call back table for each address in FRED and JIM 
callback_func_ptr * const Pi1MHz_callback_table = (void *)Pi1MHz_CB_BASE;

// Table of polling functions to call while idle
func_ptr Pi1MHz_poll_table[sizeof(emulator_inits)/sizeof(func_ptr)];

// holds the total number of polling functions to call
size_t  Pi1MHz_polls_max;

// For each location in FRED and JIM which a task wants to be called for 
// it must register its interest. Only one task can be called per location
// for access variable use WRITE_FRED WRITE_JIM READ_FRED READ_JIM
void Pi1MHz_Register_Memory(int access, int addr, callback_func_ptr func_ptr )
{
   Pi1MHz_callback_table[access+addr] = func_ptr;
}

// For each task that whats to be polled during idle it must register itself.
// is can only register once
void Pi1MHz_Register_Poll( func_ptr func_ptr )
{
   Pi1MHz_poll_table[Pi1MHz_polls_max] = func_ptr;
   Pi1MHz_polls_max++;
}

bool Pi1MHz_is_rst_active() {
   return ((RPI_GpioBase->GPLEV0 & NRST_MASK) == 0);
}

void Pi1MHz_SetnIRQ(bool irq)
{
   RPI_SetGpioPinFunction(NIRQ_PIN, irq?FS_OUTPUT:FS_INPUT);
}

void Pi1MHz_SetnNMI(bool nmi)
{
   RPI_SetGpioPinFunction(NNMI_PIN, nmi?FS_OUTPUT:FS_INPUT);
}

static uint8_t status_addr;

// Variables used to check 1MHz interface for false accesses.

static uint8_t status_rd_count;
static uint8_t status_wr_count;

// Enables the beeb to read and write status info
// setup the address for status read write 
void Pi1MHzBus_addr_Status(unsigned int gpio)
{
   uint32_t data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   status_addr = data;
   Pi1MHz_Memory[addr] = data; // enable read back
   switch (status_addr)
   {
      case 0: data = JIM_ram_size ; break; // status reg 0 returns JIM_ram_size
      case 0xfe : data = status_rd_count; break;
      case 0xff : data = status_wr_count; break;
      default : break;
   }
   Pi1MHz_Memory[addr+1] = data;
}

// take data written by the beeb and put it to the correct place
void Pi1MHzBus_write_Status(unsigned int gpio)
{
   uint32_t data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   Pi1MHz_Memory[addr] = data; // enable read back
   status_wr_count ++;
   // writes not yet supported
}

void Pi1MHzBus_read_Status(unsigned int gpio)
{
   status_rd_count ++;
}

static void init_emulator() {
   LOG_INFO("\r\n**** Raspberry Pi 1MHz Emulator ****\r\n");
   RPI_IRQBase->Disable_IRQs_1 = 0x200; // Disable USB IRQ which can be left enabled

   _disable_interrupts();

   // Direct Fred Jim access to FIQ handler

   RPI_GpioBase->GPFEN0 = NPCFD_MASK | NPCFC_MASK;
   RPI_IRQBase->FIQ_control = 0x80 + 49; // enable GPIO IRQ

   Pi1MHz_polls_max = 0;
   memset(Pi1MHz_callback_table, 0, Pi1MHz_CB_SIZE);
   
// suppress a warning as we really do want to memset 0 !
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
   memset(Pi1MHz_Memory, 0, Pi1MHZ_MEM_SIZE);
#pragma GCC diagnostic pop

   // make sure we are causing an interrupt
   Pi1MHz_SetnIRQ(CLEAR_IRQ);
   Pi1MHz_SetnNMI(CLEAR_NMI);

   _enable_interrupts();

   // Regsiter Status read back
   Pi1MHz_Register_Memory(WRITE_FRED, 0xca, Pi1MHzBus_addr_Status );
   Pi1MHz_Register_Memory(WRITE_FRED, 0xcb, Pi1MHzBus_write_Status );
   Pi1MHz_Register_Memory( READ_FRED, 0xcb, Pi1MHzBus_read_Status );

   for( size_t i=0; i <sizeof(emulator_inits)/sizeof(func_ptr); i++)
   {
      emulator_inits[i]();
   }

   while (Pi1MHz_is_rst_active());
}

static int led_pin;;

void Pi1MHz_LED(int led)
{
   if (led_pin!=-1)
      RPI_SetGpioValue(led_pin , led);
}

static char* putstring(char *ram, char term, const char *string)
{
   size_t length = strlen(string);
   strlcpy(ram, string, PAGE_SIZE);
   ram += length;
   if (term == '\n')
   {
      *ram++ ='\n';
      for (size_t i=0 ; i <length; i++)
         *ram++ = 8; // Cursor left
   }
   if (term == '\r')
      *ram++ ='\r';
   return ram;
}

static void init_JIM()
{
   extern char _end;
   JIM_ram_size = mem_info(1); // get size of ram
   JIM_ram_size = JIM_ram_size - (unsigned int)&_end; // remove program 
   JIM_ram_size = JIM_ram_size -( 4*1024*1024) ; // 4Mbytes for other mallocs
   JIM_ram_size = JIM_ram_size & 0xFF000000; // round down to 16Mbyte boundary
   JIM_ram_size = JIM_ram_size >> 24 ; // set to 16Mbyte sets 

   JIM_ram = (uint8_t *) malloc(16*1024*1024*JIM_ram_size); // malloc 480Mbytes

   // see if JIM_Init existing on the SDCARD if so load it to JIM and copy first page across Pi1MHz memory
   if (!filesystemReadFile("JIM_Init.bin",JIM_ram,JIM_ram_size<<24))
   {
       // put info in fred so beeb user can do P.$&FD00 if JIM_Init doesn't exist
      char * ram = (char *)JIM_ram;
      ram = putstring(ram,'\n', " ");
      ram = putstring(ram,'\n', " Pi1MHz "RELEASENAME);
      ram = putstring(ram,'\n', " Commit ID: "GITVERSION);
      ram = putstring(ram,'\n', " Date : " __DATE__ " " __TIME__);
      ram = putstring(ram,0   , " Pi :");
      ram = putstring(ram,'\n', get_info_string());
            putstring(ram,'\r', " ");
      
   }
// suppress a warning as we really do want to memcpy 256,jim_ram, 256!
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wrestrict"
   memcpy(&Pi1MHz_Memory[Pi1MHz_MEM_PAGE], &JIM_ram[0], PAGE_SIZE);
#pragma GCC diagnostic pop
}

static void init_hardware()
{
   // enable overriding default LED option using command.txt
   // depending on the pi use either bcm2708.disk_led_gpio=xx or bcm2709.disk_led_gpio=xx 
   char *prop = get_cmdline_prop("disk_led_gpio");
   if (prop)
   {
      led_pin = atoi(prop);
      RPI_SetGpioOutput(led_pin);
   }
   else
      led_pin = -1;
   
   LOG_DEBUG("LED pin %d\r\n",led_pin);
   
   // Configure our pins as default state as inputs 
   RPI_SetGpioInput(D7_PIN);
   RPI_SetGpioInput(D6_PIN);
   RPI_SetGpioInput(D5_PIN);
   RPI_SetGpioInput(D4_PIN);
   RPI_SetGpioInput(D3_PIN);
   RPI_SetGpioInput(D2_PIN);
   RPI_SetGpioInput(D1_PIN);
   RPI_SetGpioInput(D0_PIN);

   RPI_SetGpioInput(A7_PIN);
   RPI_SetGpioInput(A6_PIN);
   RPI_SetGpioInput(A5_PIN);
   RPI_SetGpioInput(A4_PIN);
   RPI_SetGpioInput(A3_PIN);
   RPI_SetGpioInput(A2_PIN);
   RPI_SetGpioInput(A1_PIN);
   RPI_SetGpioInput(A0_PIN);

   RPI_SetGpioInput(CLK1MHZ_PIN);
   RPI_SetGpioInput(NRST_PIN);
   RPI_SetGpioInput(NPCFD_PIN);
   RPI_SetGpioInput(NPCFC_PIN);
   RPI_SetGpioPinFunction(AUDIO_PIN, FS_ALT0); // PWM1
   RPI_SetGpioInput(NIRQ_PIN);
   RPI_SetGpioInput(NNMI_PIN);
   RPI_SetGpioInput(RNW_PIN);

   RPI_SetGpioOutput(TEST_PIN);
   RPI_SetGpioOutput(TEST2_PIN);

   RPI_SetGpioLo(NIRQ_PIN);   // Set outputs low ready for interrupts when pin is changed to FS_OUPTUT
   RPI_SetGpioLo(NNMI_PIN);

#ifdef DEBUG
   dump_useful_info();
#endif
}

void kernel_main()
{
   RPI_AuxMiniUartInit( 115200 );

   enable_MMU_and_IDCaches(0);
   _enable_unaligned_access();
   init_hardware();
   
   init_emulator();
   init_JIM();

   do {
      if (Pi1MHz_is_rst_active())
        init_emulator();

      for (size_t i=0 ; i<Pi1MHz_polls_max; i++ )
        Pi1MHz_poll_table[i]();
   } while (1);
}
