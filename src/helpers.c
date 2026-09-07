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
#include "videoplayer.h"

// 4-byte aligned: passed to Pi1MHz_MemoryWritePage which copies it with LDM.
_Alignas(4) NOINIT_SECTION uint8_t helper_ram[4*1024];

static uint8_t helper_address;

/* The help screen (helper 0, and the Pi's boot splash).

   One printf template, laid out for the Beeb's 40 x 25 text screen: every
   line is 40 columns or fewer and the whole screen is 21 rows, leaving four
   free for future helpers.  src/tests/helpers (which pulls the #defines out of this file) checks both against worst-case
   values, so an edit that wraps a line or spills past row 25 fails there,
   not on the Beeb.  Every substituted value fits at its widest; the one line
   at exactly 40 columns is the version line, where a longer git describe
   wraps it and spends a free row.

   Arguments, in order:
     %s   git version              (GITVERSION)
     %s   Pi info string           (revision, ARM/core MHz)
     %ld.%ld  SoC temperature, tenths (integer arithmetic: FIQ context)
     %X   helper base address, hex (CALL &FCxx)
     %d   helper base address, dec (*FX147,n)
     %d   SCSI jukebox register    (*FX147,n)
     %d   M5000 instance           (*FX147,202,n)
   The build date and the kernel letter - "D" for a DEBUG build, "R" for a
   release - are pasted in as literals.  Lines end in CR LF for the Beeb's
   VDU driver. */
#define HELPERS_HELP_FMT(build_date, kernel)                             \
   "Pi1MHz %s\r\n"                                                       \
   "Built " build_date " " kernel "\r\n"                                 \
   "Pi %s %ld.%ldC\r\n"                                                  \
   "\r\n"                                                                \
   "Run helper n:  X%%=n:CALL &FC%X\r\n"                                 \
   "  or *FX147,%d,n then *GO/GOIO FD00\r\n"                             \
   "\r\n"                                                                \
   " n  Helper    (CTRL-BREAK after a ROM)\r\n"                          \
   " 0  This help\r\n"                                                   \
   " 2  Screen redirector\r\n"                                           \
   " 3  ADFS               ADFS.rom\r\n"                                 \
   " 4  MMFS               SWMMFS.rom\r\n"                               \
   " 5  MMFS2              SWMMFS2.rom\r\n"                              \
   " 6  BeebSCSI utils     BSRom.rom\r\n"                                \
   " 7  ATS teletext       ATS.rom\r\n"                                  \
   " 8  AUNFS BBC B        AUNFSbeeb.rom\r\n"                            \
   " 9  AUNFS Master       AUNFSM128.rom\r\n"                            \
   "10+ Your ROM (10-15)   ROM10-15.rom\r\n"                             \
   "*FX147,%d,n     SCSIJUKE box n\r\n"                                  \
   "*FX147,202,%d then *FX147,203,1/0\r\n"                              \
   "   M5000 record on/off\r\n"

#define HELPERS_HELP_COLUMNS 40u
#define HELPERS_HELP_ROWS    25u


size_t helpers_screen_setup( char * helpscreen, size_t helpscreen_size)
{
        // We also hide the help screen at &FFE000 (1 KB there, ~600 B used).
        // Hand-formatted to tenths so this doesn't need newlib's float printf
        // support (dtoa machinery, ~5 KB) -- see CMakeLists.txt, -u _printf_float.
        // Integer arithmetic on purpose: this runs in FIQ context, and FIQ.s
        // saves no VFP state, so a float here would corrupt the registers of
        // whatever it interrupted.  Millidegrees to tenths, rounded.
        // Cannot go negative (0 on failure), so no sign handling.
        long temp_tenths = (long)((get_temp_millidegrees() + 50u) / 100u);
#ifdef DEBUG
#define HELPERS_KERNEL_LETTER "D"
#else
#define HELPERS_KERNEL_LETTER "R"
#endif
        int n = snprintf(helpscreen, helpscreen_size, HELPERS_HELP_FMT(BUILD_DATE, HELPERS_KERNEL_LETTER),
                         GITVERSION,
                         get_info_string(), temp_tenths / 10, temp_tenths % 10,
                         (unsigned int)helper_address,
                         (int)helper_address,
                         (int)(harddisc_emulator_get_address() + 1),
                         (int)M5000_emulator_read_instance());
        // snprintf returns the would-be length, which may exceed the buffer;
        // clamp so the returned length (used for fb_writen) stays exact.
        if (n < 0)
            n = 0;
        if ((size_t)n >= helpscreen_size)
            n = (int)(helpscreen_size - 1u);
        return (size_t)n;
}

static void helpers_bank_select(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   if (data == 0xFF)
    {
        // put RTS in instruction stream
        Pi1MHz_MemoryWrite_FIQ(addr+4, 0x60);
        // old page data
        ram_emulator_page_restore();
    }
    else
    {
        // put JMP instruction stream
        Pi1MHz_MemoryWrite_FIQ(addr+4, 0x4c);
        // select page
        if ((data )>= sizeof(helper_ram)>>8)
            data = 0;

        Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, &helper_ram[data<<8]);
        if (data==0)
        {
            helpers_screen_setup(( char *) &Pi1MHz->JIM_ram[ DISC_RAM_BASE + 0x00FFE000],1024);
            //signal to beeb the help screen is setup
            Pi1MHz_MemoryWrite_FIQ(Pi1MHz_MEM_PAGE+1, 0x03);
        }
    }
}

/* The help screen is assembled in FIQ (a Beeb write to FRED), and the
   mailbox is main-loop only - one static property buffer, no locking. So
   everything the screen prints must already be cached. Refresh here, and
   only while the player is idle: a mailbox exchange can block the poll loop
   (bounded only by the 3 s mailbox timeout) and stalling it mid-playback
   starves the Beeb's SCSI handshake. */
static void helpers_poll(void)
{
   if (videoplayer_active())
      return;
   info_refresh_cached();
}

void helpers_init( uint8_t instance , uint8_t address)
{
   uint8_t *helper = &helper_ram[0];
   helper_address = address;
   if (Pi1MHz->JIM_ram_size == 0)     // the help screen lives in JIM RAM
      return;
   if (filesystemReadFile("Pi1MHz/6502code.bin",&helper,sizeof(helper_ram)))
    {
        // register call backs
        Pi1MHz_Register_Memory(WRITE_FRED, address, helpers_bank_select );
        /* Prime once here, before anything can be playing, so the FIQ path
           can never be the first to touch the mailbox - then keep it fresh
           from the poll loop. */
        info_refresh_cached();
        Pi1MHz_Register_Poll(helpers_poll, "helpers");

        Pi1MHz_MemoryWrite((uint32_t)(address+0), 0x8E); // STX &FCxx
        Pi1MHz_MemoryWrite((uint32_t)(address+1), (uint8_t) address);
        Pi1MHz_MemoryWrite((uint32_t)(address+2), 0xFC);
        Pi1MHz_MemoryWrite((uint32_t)(address+3), 0XEA); // NOP
        Pi1MHz_MemoryWrite((uint32_t)(address+4), 0x4c); // JMP &FD00 // RTS
        Pi1MHz_MemoryWrite((uint32_t)(address+5), 0x00);
        Pi1MHz_MemoryWrite((uint32_t)(address+6), 0xFD);

        if (!ram_emulator_jim_init_loaded())
        {
            // put info in JIM page 0 so the beeb user can do P.$&FD00
            snprintf((char *)Pi1MHz->JIM_ram, PAGE_SIZE, " Use CALL &FC%X\n\r", address);
            Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, &Pi1MHz->JIM_ram[0]);
        }
    }
}

