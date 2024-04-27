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

#include "rpi/info.h"
#include <stdlib.h>
#include "scripts/gitversion.h"
#include "BeebSCSI/filesystem.h"

static uint8_t rambyte_address;

void ram_emulator_byte_addr(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   switch (addr - rambyte_address)
   {
      case 0:  Pi1MHz->byte_ram_addr = (Pi1MHz->byte_ram_addr & 0xFFFFFF00) | data; break;
      case 1:  Pi1MHz->byte_ram_addr = (Pi1MHz->byte_ram_addr & 0xFFFF00FF) | data<<8; break;
      default: Pi1MHz->byte_ram_addr = (Pi1MHz->byte_ram_addr & 0xFF00FFFF) | data<<16; break;
   }

   Pi1MHz_MemoryWrite(rambyte_address + 3 , Pi1MHz->JIM_ram[Pi1MHz->byte_ram_addr]); // setup new data now the address has changed;
   Pi1MHz_MemoryWrite(addr, data);               // enable the address register to be read back
}

void ram_emulator_byte_write(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   Pi1MHz->JIM_ram[Pi1MHz->byte_ram_addr] =  data;
   Pi1MHz_MemoryWrite(addr,  data);
}

void ram_emulator_page_addr_high(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   if (data > (Pi1MHz->JIM_ram_size)) data = Pi1MHz->JIM_ram_size - 1;
               Pi1MHz->page_ram_addr = (Pi1MHz->page_ram_addr & 0x00FFFFFF) | data<<24;
   Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&Pi1MHz->JIM_ram[Pi1MHz->page_ram_addr])) );
   Pi1MHz_MemoryWrite(addr,data); // enable the address register to be read back
}

void ram_emulator_page_addr_mid(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   Pi1MHz->page_ram_addr = (Pi1MHz->page_ram_addr & 0xFF00FFFF) | data<<16;
   Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&Pi1MHz->JIM_ram[Pi1MHz->page_ram_addr])) );
   Pi1MHz_MemoryWrite(addr,data); // enable the address register to be read back
}

void ram_emulator_page_addr_low(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   Pi1MHz->page_ram_addr = (Pi1MHz->page_ram_addr & 0xFFFF00FF) | data<<8 ;
   // RPI_SetGpioHi(TEST_PIN);
   Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&Pi1MHz->JIM_ram[Pi1MHz->page_ram_addr])) );
   // RPI_SetGpioLo(TEST_PIN);
   Pi1MHz_MemoryWrite(addr,data); // enable the address register to be read back
}

void ram_emulator_page_restore(void)
{
   Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&Pi1MHz->JIM_ram[Pi1MHz->page_ram_addr])) );
}

void ram_emulator_page_write(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   Pi1MHz->JIM_ram[Pi1MHz->page_ram_addr + addr] = data;
   Pi1MHz_MemoryWrite(Pi1MHz_MEM_PAGE + addr, data);
}

static char* putstring(char *ram, char term, const char *string)
{
   size_t length;
   length = strlcpy(ram, string, PAGE_SIZE);
   ram += length;
   if (term == '\n')
   {
      *ram++ ='\n';
      for (size_t i=0 ; i <length; i++)
         *ram++ = 8; // Cursor left
   }
   if (term == '\r')
      {
         *ram++ ='\n';
         *ram++ ='\r';
      }
   return ram;
}

void rampage_emulator_init( uint8_t instance , uint8_t address)
{
   static uint8_t init = 0 ;
   // Page access register write fcfd fcfe fcff
   Pi1MHz_Register_Memory(WRITE_FRED, address + 1, ram_emulator_page_addr_high ); // high byte
   Pi1MHz_Register_Memory(WRITE_FRED, address + 2, ram_emulator_page_addr_mid ); // Mid byte
   Pi1MHz_Register_Memory(WRITE_FRED, address + 3, ram_emulator_page_addr_low ); // low byte

   // register every address in JIM &FD00
   for (uint32_t i=0 ; i<PAGE_SIZE; i++)
      Pi1MHz_Register_Memory(WRITE_JIM, (uint8_t) i, ram_emulator_page_write );

   // Initialise JIM RAM

   extern char _end;

   uint32_t temp = mem_info(1); // get size of ram
   temp = temp - (unsigned int)&_end; // remove program
   temp = temp -( 4*1024*1024) ; // 4Mbytes for other mallocs
   temp = temp & 0xFF000000; // round down to 16Mbyte boundary
   Pi1MHz->JIM_ram_size = (uint8_t)(temp >> 24) ; // set to 16Mbyte sets

   Pi1MHz->byte_ram_addr = (size_t) ((Pi1MHz->JIM_ram_size - 1) * 16UL * 1024UL * 1024UL);
   Pi1MHz->page_ram_addr = 0;

   fx_register[instance] = Pi1MHz->JIM_ram_size;  // fx addr 0 returns ram size
   if (init == 0)
   {
      init = 1;
      Pi1MHz->JIM_ram = (uint8_t *) malloc(16*1024*1024*Pi1MHz->JIM_ram_size); // malloc 480Mbytes
   }
   filesystemInitialise(0);

   // see if JIM_Init existing on the SDCARD if so load it to JIM and copy first page across Pi1MHz memory
   if (!filesystemReadFile("JIM_Init.bin",Pi1MHz->JIM_ram,Pi1MHz->JIM_ram_size<<24))
   {
       // put info in fred so beeb user can do P.$&FD00 if JIM_Init doesn't exist
      char * ram = (char *)Pi1MHz->JIM_ram;
      ram = putstring(ram,'\n', "");
      ram = putstring(ram,'\n', " Pi1MHz "RELEASENAME);
      ram = putstring(ram,'\n', " Commit ID: "GITVERSION);
      ram = putstring(ram,'\n', " Date : " __DATE__ " " __TIME__);
      ram = putstring(ram,0   , " Pi :");
            putstring(ram,'\r', get_info_string());
   }

   // see if BEEB.MMB exists on the SDCARD if so load it into JIM+16Mbytes
  // filesystemReadFile("BEEB.MMB",Pi1MHz->JIM_ram+(16*1024*1024),Pi1MHz->JIM_ram_size<<24);

   Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&Pi1MHz->JIM_ram[0])) );
}

void rambyte_emulator_init( uint8_t instance , uint8_t address)
{
   rambyte_address = address;

   // register call backs
   // byte memory address write fc00 01 02
   Pi1MHz_Register_Memory(WRITE_FRED, rambyte_address+0, ram_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, rambyte_address+1, ram_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, rambyte_address+2, ram_emulator_byte_addr );
   // fc03 write data byte
   Pi1MHz_Register_Memory(WRITE_FRED, rambyte_address+3, ram_emulator_byte_write );
}