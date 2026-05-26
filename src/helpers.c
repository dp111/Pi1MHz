/*
  6502 helper functions

*/
#include <string.h>
#include <stdio.h>
#include "Pi1MHz.h"
#include "ram_emulator.h"
#include "BeebSCSI/filesystem.h"
#include "scripts/gitversion.h"
#include "rpi/info.h"

NOINIT_SECTION uint8_t helper_ram[4*1024];

static void helpers_setup(uint8_t helper_address)
{
  // We also hide the help screen at &FFE000
        char hex[3];
        char dec[4];
        sprintf(hex, "%X",helper_address);
        sprintf(dec, "%d",helper_address);

        char * helpscreen = ( char *) &Pi1MHz->JIM_ram[ DISC_RAM_BASE + 0x00FFE000] ;
        helpscreen += strlcpy(helpscreen, "\r\n Pi1MHZ "RELEASENAME" , "GITVERSION
        "\r\n Date : " __DATE__ " " __TIME__
        "\r\n Pi : " , PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen, get_info_string(), PAGE_SIZE*16);
        sprintf(helpscreen, " %2.1fC", (double) get_temp() );
        helpscreen += 6;
        helpscreen += strlcpy(helpscreen, "\r\n"
        "\r\n Helper functions can be started in one"
        "\r\n of three ways :"
        "\r\n"
        "\r\n *FX147,", PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen, dec, PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen,",n <ret> *GO FD00 <ret>", PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen,"\r\n *FX147,", PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen, dec, PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen,",n <ret> *GOIO FD00 <ret>", PAGE_SIZE*16);

        helpscreen += strlcpy(helpscreen,"\r\n X%=n:CALL&FC", PAGE_SIZE*16);
        helpscreen += strlcpy(helpscreen, hex, PAGE_SIZE*16);

        helpscreen += strlcpy(helpscreen,
        " <ret>\r\n\r\n where n is one of the following"
        "\r\n"
        "\r\n 0 # This help screen"
        "\r\n 1 # Status N/A"
        "\r\n 2 # Enable screen redirector"
        "\r\n 3 # Load ADFS into SWR"
        "\r\n 4 # Load MMFS into SWR"
        "\r\n 5 # Load MMFS2 into SWR"
        "\r\n 6 # Load BeebSCSI helper ROM into SWR"
        "\r\n 10-15 # Load User ROM10-ROM15 into SWR"
        "\r\n", PAGE_SIZE*16);
        helpscreen[0] = 0;
        //signal to beeb the help screen is setup
        Pi1MHz_MemoryWrite(Pi1MHz_MEM_PAGE+1, 0x03);
}

static void helpers_bank_select(unsigned int gpio)
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
        if (data==0)
            helpers_setup((uint8_t)addr);
    }

}

void helpers_init( uint8_t instance , uint8_t address)
{
   uint8_t *helper = &helper_ram[0];
   if (filesystemReadFile("Pi1MHz/6502code.bin",&helper,sizeof(helper_ram)))
    {
        // register call backs
        Pi1MHz_Register_Memory(WRITE_FRED, address, helpers_bank_select );

        Pi1MHz_MemoryWrite(address+0, 0x8E); // STX &FCxx
        Pi1MHz_MemoryWrite(address+1, (uint8_t) address);
        Pi1MHz_MemoryWrite(address+2, 0xFC);
        Pi1MHz_MemoryWrite(address+3, 0XEA); // NOP
        Pi1MHz_MemoryWrite(address+4, 0x4c); // JMP &FD00 // RTS
        Pi1MHz_MemoryWrite(address+5, 0x00);
        Pi1MHz_MemoryWrite(address+6, 0xFD);
    }
}