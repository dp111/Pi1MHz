/*
  6502 helper functions

*/
#include <string.h>

#include "Pi1MHz.h"
#include "ram_emulator.h"
#include "BeebSCSI/filesystem.h"
#include "scripts/gitversion.h"
#include "rpi/info.h"

NOINIT_SECTION uint8_t helper_ram[4*1024];

void helpers_bank_select(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   if (data == 0xFF)
    {
        // put RTS in instruction stream
        Pi1MHz_MemoryWrite(addr+4, 0x60);
        // old page data
        ram_emulator_page_restore();
    }
    else
    {
        // put JMP instruction stream
        Pi1MHz_MemoryWrite(addr+4, 0x4c);
        // select page
        Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, ((uint32_t *)(&helper_ram[data<<8])) );
    }

}
void helpers_init( uint8_t instance , int address)
{
   if (!filesystemReadFile("6502code.bin",&helper_ram[0],sizeof(helper_ram)))
    {
        // register call backs
        Pi1MHz_Register_Memory(WRITE_FRED, address, helpers_bank_select );

        Pi1MHz_MemoryWrite(address+0, 0x8F); // STX &FCxx
        Pi1MHz_MemoryWrite(address+1, (uint8_t) address);
        Pi1MHz_MemoryWrite(address+2, 0xFC);
        Pi1MHz_MemoryWrite(address+3, 0XEA); // NOP
        Pi1MHz_MemoryWrite(address+4, 0x4c); // JMP &FD00 // RTS
        Pi1MHz_MemoryWrite(address+5, 0xFD);
        Pi1MHz_MemoryWrite(address+6, 0x00);

           // We also hide the help screen at &FFE000
        char * helpscreen = ( char *) &Pi1MHz->JIM_ram[ DISC_RAM_BASE + 0x00FFE000] ;
        helpscreen += strlcpy(helpscreen, "\r Pi1MHZ "RELEASENAME
        "\r Commit ID : "GITVERSION
        "\r Date : " __DATE__ " " __TIME__
        "\r Pi : " , PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen, get_info_string(), PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen, "\r\r"
        "\r Helper functions"
        "\r"
        "\r *FX147,136,nn <ret> or X%=nn"
        "\r then either CALL&FC88 , *GO FC88, *GOIO FC88"
        "\r where nn is one of the following"
        "\r"
        "\r 0 # This help screen"
        "\r 1 # Enable screen redirector"
        "\r 2 # Load ADFS into SWR"
        "\r 3 # Load ADFS into MMFS"
        "\r 4 # Load ADFS into MMFSv2"
        "\r", PAGE_SIZE*16);
        helpscreen[0] = 0;

    }
}