/*
  6502 helper functions

*/
#include <string.h>
#include <stdio.h>
#include "Pi1MHz.h"
#include "ram_emulator.h"
#include "harddisc_emulator.h"
#include "M5000_emulator.h"
#include "BeebSCSI/filesystem.h"
#include "scripts/gitversion.h"
#include "rpi/info.h"

// 4-byte aligned: passed to Pi1MHz_MemoryWritePage which copies it with LDM.
_Alignas(4) NOINIT_SECTION uint8_t helper_ram[4*1024];

static uint8_t helper_address;

static size_t strlcpylen(char *dst, const char *src, size_t dstsize)
{
    size_t srclen = 0;
    while (srclen < dstsize - 1 && src[srclen]) {
        dst[srclen] = src[srclen];
        srclen++;
    }
    dst[srclen] = '\0';

    return srclen;
}


void helpers_screen_setup( char * helpscreen, size_t helpscreen_size)
{
  // We also hide the help screen at &FFE000
        char hex[3];
        char dec[4];
        char M5000address[4];

        sprintf(hex, "%X",helper_address);
        sprintf(dec, "%d",helper_address);
        sprintf(M5000address, "%d", M5000_emulator_read_instance());

        char scsiaddress[4];
        sprintf(scsiaddress, "%d", harddisc_emulator_get_address()+1);

        size_t size;
        size = strlcpylen(helpscreen, "\r\nPi1MHz "RELEASENAME" , "GITVERSION
        "\r\nDate : " __DATE__ " " __TIME__
        "\r\nPi : " , helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen, get_info_string(), helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = (size_t)snprintf(helpscreen, helpscreen_size, " %2.1fC", (double) get_temp() );
        helpscreen += size;helpscreen_size -= size;
        size= strlcpylen(helpscreen, "\r\n"
        "\r\n3 ways to start helper functions :"
        "\r\n*FX147,", helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen, dec, helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen,",n <ret> *GO FD00 <ret>"
        "\r\n*FX147,", helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen, dec, helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen,",n <ret> *GOIO FD00 <ret>"
        "\r\nX%=n:CALL&FC", helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen, hex, helpscreen_size);
        helpscreen += size;helpscreen_size -= size;

        size = strlcpylen(helpscreen,
        " <ret>\r\n\r\nwhere n is one of the following :"
        "\r\n0 # This help screen"
        "\r\n1 # Status N/A"
        "\r\n2 # Enable screen redirector"
        "\r\n3 # Load ADFS into SWR"
        "\r\n4 # Load MMFS into SWR"
        "\r\n5 # Load MMFS2 into SWR"
        "\r\n6 # Load BeebSCSI helper ROM into SWR"
        "\r\n10-15 # Load User ROM10-ROM15 into SWR\r\n"
        "\r\n*FX147,", helpscreen_size);
        helpscreen += size;helpscreen_size -= size;

        size = strlcpylen(helpscreen, scsiaddress, helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen,",n # SCSIJUKE box directory"
            "\r\n*FX147,202,", helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen, M5000address, helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen,":*FX147,203,1 #Record M5000\r\n"
                    "*FX147,202,", helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        size = strlcpylen(helpscreen, M5000address, helpscreen_size);
        helpscreen += size;helpscreen_size -= size;
        strlcpylen(helpscreen,":*FX147,203,0 #End Record\r\n", helpscreen_size);
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
        if ((data )>= sizeof(helper_ram)>>8)
            data = 0;

        Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, &helper_ram[data<<8]);
        if (data==0)
        {
            helpers_screen_setup(( char *) &Pi1MHz->JIM_ram[ DISC_RAM_BASE + 0x00FFE000],1024);
            //signal to beeb the help screen is setup
            Pi1MHz_MemoryWrite(Pi1MHz_MEM_PAGE+1, 0x03);
        }
    }
}

void helpers_init( uint8_t instance , uint8_t address)
{
   uint8_t *helper = &helper_ram[0];
   helper_address = address;
   if (filesystemReadFile("Pi1MHz/6502code.bin",&helper,sizeof(helper_ram)))
    {
        // register call backs
        Pi1MHz_Register_Memory(WRITE_FRED, address, helpers_bank_select );

        Pi1MHz_MemoryWrite((uint32_t)(address+0), 0x8E); // STX &FCxx
        Pi1MHz_MemoryWrite((uint32_t)(address+1), (uint8_t) address);
        Pi1MHz_MemoryWrite((uint32_t)(address+2), 0xFC);
        Pi1MHz_MemoryWrite((uint32_t)(address+3), 0XEA); // NOP
        Pi1MHz_MemoryWrite((uint32_t)(address+4), 0x4c); // JMP &FD00 // RTS
        Pi1MHz_MemoryWrite((uint32_t)(address+5), 0x00);
        Pi1MHz_MemoryWrite((uint32_t)(address+6), 0xFD);
    }
}

uint8_t helpers_get_address(void)
{
   return helper_address;
}