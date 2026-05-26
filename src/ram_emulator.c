/*
 Emulates both byte ram and page ram

 16Mbytes is shared between both access types.

 Byte RAM docs and RAMFS : http://www.sprow.co.uk/bbc/ramdisc.htm

 Notes from stardot

 Torch Graduate ( Rom not paged in)
- Paging register at &FCFF to page into JIM 32 pages (8K) of ROM.
  At startup page 0 of ROM is paged into JIM and executed from OS with JMP (&FDFE) to download into BBC RAM.

Music 5000/3000
- Paging register at &FCFF to page into JIM 8 pages (4K) of RAM.
  Both devices use JIM, 5000 when 0xFCFF & 0xF0 = 0x30, 3000 when 0xFCFF & 0xF0 = 0x50.

Opus Challenger 3
- Paging registers at &FCFE, &FCFF to page into JIM 2048 pages (512K) of RAM disc.

Morley RAMdisc (Not supported)
- Paging registers at &FC00-02 to page into JIM 4096 pages (1MB) of RAM disc.
  Not fully understood so may be inaccurate.

Millipede PRISMA-3 (Not support)
- Paging register at &FCB7 to page into JIM 32 pages (8K) of non-volatile RAM, containing palette, etc.

*/

#include <string.h>
#include "Pi1MHz.h"

static size_t byte_ram_addr;
static size_t page_ram_addr;

static uint8_t ram_address;

void ram_emulator_byte_addr(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   switch (addr - ram_address)
   {
      case 0:  byte_ram_addr = (byte_ram_addr & 0xFFFFFF00) | data; break;
      case 1:  byte_ram_addr = (byte_ram_addr & 0xFFFF00FF) | data<<8; break;
      default: byte_ram_addr = (byte_ram_addr & 0xFF00FFFF) | data<<16; break;
   }

   Pi1MHz_MemoryWrite(ram_address + 3 , JIM_ram[byte_ram_addr]); // setup new data now the address has changed;
   Pi1MHz_MemoryWrite(addr, data);               // enable the address register to be read back
}

void ram_emulator_byte_write(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   JIM_ram[byte_ram_addr] =  data;
   Pi1MHz_MemoryWrite(ram_address + 3,  data);
}

void ram_emulator_page_addr(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   if ( addr == 0xfd)
   {  if (data > (JIM_ram_size))
         data = JIM_ram_size;
      page_ram_addr = (page_ram_addr & 0x00FFFFFF) | data<<24;
   } else {
   if ( addr == 0xfe)
      page_ram_addr = (page_ram_addr & 0xFF00FFFF) | data<<16;
   else
      page_ram_addr = (page_ram_addr & 0xFFFF00FF) | data<<8;
   }

   // setup new data now the address has changed

   // RPI_SetGpioHi(TEST_PIN);
   Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&JIM_ram[page_ram_addr])) );
   // RPI_SetGpioLo(TEST_PIN);
   Pi1MHz_MemoryWrite(addr,data); // enable the address register to be read back
}

void ram_emulator_page_write(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   JIM_ram[page_ram_addr + addr] = data;
   Pi1MHz_MemoryWrite(Pi1MHz_MEM_PAGE + addr, data);
}

void ram_emulator_init( uint8_t instance , int address)
{
   byte_ram_addr = 0;
   page_ram_addr = 0;

   ram_address = (uint8_t) address;

   for( uint32_t i = 0; i < PAGE_SIZE ; i++)
      Pi1MHz_MemoryWrite(Pi1MHz_MEM_PAGE + i, JIM_ram[page_ram_addr+i]);

   // register call backs
   // byte memory address write fc00 01 02
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+0, ram_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+1, ram_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+2, ram_emulator_byte_addr );

   // fc03 write data byte
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+3, ram_emulator_byte_write );

   // Page access register write fcfd fcfe fcff
   Pi1MHz_Register_Memory(WRITE_FRED, 0xfd, ram_emulator_page_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, 0xfe, ram_emulator_page_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, 0xff, ram_emulator_page_addr );

   // register every address in JIM &FD00
   for (int i=0 ; i<PAGE_SIZE; i++)
      Pi1MHz_Register_Memory(WRITE_JIM, i, ram_emulator_page_write );
}
