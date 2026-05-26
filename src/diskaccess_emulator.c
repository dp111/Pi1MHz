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

#include "ram_emulator.h"
#include "BeebSCSI/fatfs/ff.h"			/* Obtains integer types */
#include "BeebSCSI/fatfs/diskio.h"

static size_t byte_ram_addr;

static uint8_t ram_address;

static FIL fileObject[16];

static void diskaccess_emulator_update_address()
{
   size_t byte_ram_addr_old = byte_ram_addr-1;

   Pi1MHz_MemoryWrite16(ram_address, byte_ram_addr);
   if ((byte_ram_addr_old & 0x00FF0000 ) != (byte_ram_addr & 0x00FF0000 ) )
      Pi1MHz_MemoryWrite( ram_address+2, ( byte_ram_addr >> 16 ) & 0xFF );
}

void diskaccess_emulator_byte_addr(unsigned int gpio)
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

void diskaccess_emulator_byte_write_inc(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   JIM_ram[byte_ram_addr] =  data;
   byte_ram_addr++;
   Pi1MHz_MemoryWrite(ram_address + 3 , JIM_ram[byte_ram_addr]); // setup new data now the address has changed;
   diskaccess_emulator_update_address();
}

void diskaccess_emulator_byte_read_inc(unsigned int gpio)
{
   byte_ram_addr++;
   Pi1MHz_MemoryWrite(ram_address + 3 , JIM_ram[byte_ram_addr]); // setup new data now the address has changed;
   diskaccess_emulator_update_address();
}


void diskaccess_emulator_command(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   Pi1MHz_MemoryWrite(addr, data); // return existing command
   uint32_t base_addr = ((JIM_ram_size - 1) * 16 * 1024 * 1024) ;

   uint32_t command_pointer = base_addr | 0xFF0000 | (data<<8);

   switch (  JIM_ram[command_pointer] )
   {
    case 0 :
        Pi1MHz_MemoryWrite(addr,
            disk_read( JIM_ram[command_pointer+1], &JIM_ram[*(uint32_t *)&JIM_ram[command_pointer+4]+base_addr ]
                    , *(uint32_t *)&JIM_ram[command_pointer+8] , *(uint32_t *)&JIM_ram[command_pointer+12] ) );
        break;
    case 1 :
         Pi1MHz_MemoryWrite(addr,
            disk_write( JIM_ram[command_pointer+1], &JIM_ram[*(uint32_t *)&JIM_ram[command_pointer+4]+base_addr ]
                    , *(uint32_t *)&JIM_ram[command_pointer+8] , *(uint32_t *)&JIM_ram[command_pointer+12] ) );
         break;
    case 2 :
        Pi1MHz_MemoryWrite(addr,
            f_open( &fileObject[data & 15], (char * )&JIM_ram[command_pointer+3]
                    , JIM_ram[command_pointer+2] ) );
        break;
    case 3 :
        Pi1MHz_MemoryWrite(addr,
             f_close( &fileObject[data & 15] ) );
        break;
    case 4 :
    {
        FRESULT result;
        UINT length;
        result = f_lseek( &fileObject[data & 15], *(uint32_t *)&JIM_ram[command_pointer+8] );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = f_read( &fileObject[data & 15], &JIM_ram[*(uint32_t *)&JIM_ram[command_pointer+4]+base_addr] , (*(uint32_t *)&JIM_ram[command_pointer])>>8 , &length);
        *(uint32_t *)&JIM_ram[command_pointer] = (length << 8 ) | JIM_ram[command_pointer];
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length <  ((*(uint32_t *)&JIM_ram[command_pointer])>>8 ))
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 5 :
    {
        FRESULT result;
        UINT length;
        result = f_lseek( &fileObject[data & 15], *(uint32_t *)&JIM_ram[command_pointer+8] );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = f_write( &fileObject[data & 15], &JIM_ram[*(uint32_t *)&JIM_ram[command_pointer+4]+base_addr] , (*(uint32_t *)&JIM_ram[command_pointer])>>8 , &length);
        *(uint32_t *)&JIM_ram[command_pointer] = (length << 8 ) | JIM_ram[command_pointer];
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length <  ((*(uint32_t *)&JIM_ram[command_pointer])>>8 ))
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 6 :
    {
        *(uint32_t *)&JIM_ram[command_pointer+8] = f_size(  &fileObject[data & 15] );
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    default : break;
   }

}


void diskaccess_emulator_init( uint8_t instance , int address)
{
   byte_ram_addr = (JIM_ram_size - 1) * 16 * 1024 * 1024;

   ram_address = (uint8_t) address;

   // register call backs
   // byte memory address write
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+0, diskaccess_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+1, diskaccess_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+2, diskaccess_emulator_byte_addr );
   // data byte
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+3, diskaccess_emulator_byte_write_inc );
   Pi1MHz_Register_Memory(READ_FRED , ram_address+3, diskaccess_emulator_byte_read_inc );
   // command pointer
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+4, diskaccess_emulator_command );

}
