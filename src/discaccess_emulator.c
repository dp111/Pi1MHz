/*
  Creates methods to enable a beeb to access the SDCARD

 16Mbytes is available as a buffer

*/

#include <string.h>
#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "BeebSCSI/fatfs/ff.h"			/* Obtains integer types */
#include "BeebSCSI/fatfs/diskio.h"

static size_t disc_ram_addr;

static uint8_t ram_address;

NOINIT_SECTION static FIL fileObject[16];

static void discaccess_emulator_update_address(void)
{
   size_t disc_ram_addr_old = disc_ram_addr-1;

   Pi1MHz_MemoryWrite16(ram_address, disc_ram_addr);
   if ((disc_ram_addr_old & 0x00FF0000 ) != (disc_ram_addr & 0x00FF0000 ) )
      Pi1MHz_MemoryWrite( ram_address+2, ( disc_ram_addr >> 16 ) & 0xFF );
}

void discaccess_emulator_byte_addr(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   switch (addr - ram_address)
   {
      case 0:  disc_ram_addr = (disc_ram_addr & 0xFFFFFF00) | data; break;
      case 1:  disc_ram_addr = (disc_ram_addr & 0xFFFF00FF) | data<<8; break;
      default: disc_ram_addr = (disc_ram_addr & 0xFF00FFFF) | data<<16; break;
   }

   Pi1MHz_MemoryWrite(ram_address + 3 , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   discaccess_emulator_update_address();              // enable the address register to be read back
}

void discaccess_emulator_byte_write_inc(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   Pi1MHz->JIM_ram[disc_ram_addr] =  data;
   disc_ram_addr++;
   Pi1MHz_MemoryWrite(ram_address + 3 , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   discaccess_emulator_update_address();
}

void discaccess_emulator_byte_read_inc(unsigned int gpio)
{
   disc_ram_addr++;
   Pi1MHz_MemoryWrite(ram_address + 3 , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   discaccess_emulator_update_address();
}


void discaccess_emulator_command(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   Pi1MHz_MemoryWrite(addr, data); // return existing command
   uint32_t base_addr = DISC_RAM_BASE ;

   uint32_t command_pointer = base_addr | 0xFF0000 | (data<<8);

   switch (  Pi1MHz->JIM_ram[command_pointer] )
   {
    case 0 :
        Pi1MHz_MemoryWrite(addr,
            disk_read( Pi1MHz->JIM_ram[command_pointer+1],
                        &Pi1MHz->JIM_ram[(*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4])+base_addr ],
                        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+8],
                        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+12]
                        ) );
       #if 0
            printf("Read sector %x, %p %x, %x, %x, %x\r\n",base_addr, &Pi1MHz->JIM_ram[0],Pi1MHz->JIM_ram[command_pointer+1],*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4],*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+8],*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+12] );
             uint8_t buffer[512];
             for(int i=0;i<512;i++)
            {
                printf("%x ",buffer[i] /*Pi1MHz->JIM_ram[(*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4])+base_addr + i ] */);
                Pi1MHz->JIM_ram[(*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4])+base_addr + i ]=buffer[i];
                if ((i%16)==0) printf("\r\n");
            }
        #endif
        break;
    case 1 :
         Pi1MHz_MemoryWrite(addr,
            disk_write( Pi1MHz->JIM_ram[command_pointer+1],
                        &Pi1MHz->JIM_ram[(*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4])+base_addr ],
                        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+8] ,
                        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+12] )
                        );
         break;
    case 2 :
        Pi1MHz_MemoryWrite(addr,
            f_open( &fileObject[data & 15], (char * )&Pi1MHz->JIM_ram[command_pointer+3]
                    , Pi1MHz->JIM_ram[command_pointer+2] ) );
        break;
    case 3 :
        Pi1MHz_MemoryWrite(addr,
             f_close( &fileObject[data & 15] ) );
        break;
    case 4 :
    {
        FRESULT result;
        UINT length;
        result = f_lseek( &fileObject[data & 15], *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+8] );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = f_read( &fileObject[data & 15], &Pi1MHz->JIM_ram[*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4]+base_addr] , (*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer])>>8 , &length);
        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer] = (length << 8 ) | Pi1MHz->JIM_ram[command_pointer];
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length <  ((*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer])>>8 ))
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
        result = f_lseek( &fileObject[data & 15], *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+8] );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = f_write( &fileObject[data & 15], &Pi1MHz->JIM_ram[*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+4]+base_addr] , (*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer])>>8 , &length);
        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer] = (length << 8 ) | Pi1MHz->JIM_ram[command_pointer];
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length <  ((*(uint32_t *)&Pi1MHz->JIM_ram[command_pointer])>>8 ))
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 6 :
    {
        *(uint32_t *)&Pi1MHz->JIM_ram[command_pointer+8] = f_size(  &fileObject[data & 15] );
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 20 : Pi1MHz_MemoryWrite(addr, disk_type()); break;
    default : break;
   }

}


void discaccess_emulator_init( uint8_t instance , uint8_t address)
{
   disc_ram_addr = DISC_RAM_BASE;

   ram_address = address;

   // register call backs
   // byte memory address write
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+0, discaccess_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+1, discaccess_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+2, discaccess_emulator_byte_addr );
   // data byte
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+3, discaccess_emulator_byte_write_inc );
   Pi1MHz_Register_Memory(READ_FRED , ram_address+3, discaccess_emulator_byte_read_inc );
   // command pointer
   Pi1MHz_Register_Memory(WRITE_FRED, ram_address+4, discaccess_emulator_command );

   Pi1MHz_MemoryWrite(ram_address+4, 0 ) ; // make sure command is null on read back

}
