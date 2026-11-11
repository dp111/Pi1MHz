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
#include "rpi/systimer.h"
#include "videoplayer.h"
#include "serial_redirect.h"
#include "services.h"
#include "BeebSCSI/fatfs/ff.h"

/* One 256-byte page per helper entry point, 0..47 - the page the Beeb runs
   when it asks for helper n.  It is exactly as many pages as 6502code.bin
   carries, so the bank select below rejects a helper number that has no
   page rather than running whatever follows.  17 is the SD card explorer,
   and 31-45 are the pages it chains through while it runs; 18 is FujiNet,
   19 the serial redirector, 20 the telnet daemon, 21 VFS, 22 and 23 the RAM probe
   (ramprobe.c), and 24-30 are left free
   for future helpers,
   answering with a bare return until one is written.  46 is the explorer's
   transfer page template and 47 the second half of every ROM loader
   (swrinstall). */
#define HELPER_PAGES 48u

/* Helpers 19 and 20 hook the vectors into the serial stub (19 for the modem,
   20 for the telnet daemon); the LDY # at &FD06 of each carries the stub's FRED
   offset, which only the Pi knows (Serial_addr). */
#define SERIAL_HELPER_PAGE  19u
#define TELNET_HELPER_PAGE  20u
#define SERIAL_HELPER_LDY   0x06u

/* &FC88 = HELPER_XFER shows the helper transfer page in the JIM window
   instead of a helper page.  It is a page of the services buffer, so a
   helper can hand the filing system a buffer it can see - OSGBPB, OSFIND,
   OSWORD 0 all take a pointer - without borrowing any Beeb RAM (pages &09
   and &0A are the RS423/cassette buffers, and serial must keep working).
   Its offset sits in the program area of the documented buffer layout
   (docs/advanced.md: 8-14 MB), beside the SD explorer's other buffers. */
#define HELPER_XFER        0xFEu
#define HELPER_XFER_OFFSET 0x00D90000u
// 4-byte aligned: passed to Pi1MHz_MemoryWritePage which copies it with LDM.
_Alignas(4) NOINIT_SECTION uint8_t helper_ram[HELPER_PAGES * 256u];

static uint8_t helper_address;
static volatile int helper_window = -1;     /* the helper page in the window, or -1 */
static volatile bool     help_shown_pending;   /* a help screen was shown: reformat it later */
static volatile uint32_t help_shown_us;
/* The formatted help screen, two copies: the poll formats the one the FIQ
   is not showing, then flips help_front, so a select arriving mid-format
   copies a whole screen. */
_Alignas(4) static char  help_text[2][1536];
static volatile uint16_t help_len[2];
static volatile uint8_t  help_front;
#define HELP_JIM (DISC_RAM_BASE + 0x00FFE000u)  /* where the page-0 code prints from */

/* The help screen (helper 0, and the Pi's boot splash).

   One printf template, 40 columns wide for the Beeb's text screen.  It is
   26 rows, and each user ROM after the first adds one (32 at most), so
   it is longer than a 25-row screen: helper 0 prints it in paged mode.
   src/tests/helpers (which pulls the #defines out of this file) checks it
   with worst-case values, so an edit that wraps a line fails there, not
   on the Beeb.  Every substituted value fits at its widest; the one line
   at exactly 40 columns is the version line, where a longer git describe
   wraps it.

   Arguments, in order:
     %s   git version              (GITVERSION)
     %s   Pi info string           (revision, ARM MHz)
     %ld.%ld  SoC temperature, tenths (integer arithmetic: no float printf)
     %X   helper base address, hex (CALL &FCxx)
     %d   helper base address, dec (*FX147,n)
     %s   the user ROM lines       (helpers_user_roms: 1-6 lines, each ending CR LF)
     %d   SCSI jukebox register    (*FX147,n)
     %d   M5000 instance           (*FX147,202,n)
     %d   hard disc instance       (*FX147,202,n: SD card eject/insert/reboot)
     %.2s x3, %.5s  build date as YYMMDD HH:MM, cut from BUILD_DATE
   The kernel letter - "D" for a DEBUG build, "R" for a release - is pasted
   in as a literal.  Lines end in CR LF for the Beeb's VDU driver. */
#define HELPERS_HELP_FMT(kernel)                                         \
   "Pi1MHz %s\r\n"                                                       \
   "Pi %s %ld.%ldC %.2s%.2s%.2s %.5s " kernel "\r\n"                      \
   "\r\n"                                                                \
   "Run helper n:  X%%=n:CALL &FC%X\r\n"                                 \
   "  or *FX147,%d,n then *GO/GOIO FD00\r\n"                             \
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
   "%s"                                                                  \
   "16  1MHz-WiFi+WiCFS    1mhz-wicfs.rom\r\n"                           \
   "17  SD card explorer (file transfer)\r\n"                            \
   "18  FujiNet            fujinetB/M.rom\r\n"                           \
   "19  Modem (serial redirector)\r\n"                                  \
   "20  Telnetd\r\n"                                                    \
   "21  VFS (bank 8,3-0)   VFS.rom\r\n"                                 \
   "*FX147,%d,n     SCSIJUKE box n\r\n"                                  \
   "*FX147,202,%d then 203,1/0 M5000 rec\r\n"                          \
   "*FX147,202,%d 203,1-3 out/in/restart\r\n"                          \
   "  then 203,4/5/6 discs on SD/USB/SMB\r\n"

#define HELPERS_HELP_COLUMNS 40u
/* The user ROM lines: none found, or one per helper 10-15 that has a file. */
#define HELPERS_USER_ROM_NONE "10+ Your ROM (10-15)   ROM10*.rom\r\n"
/* 35, not 36: a line that fills all 40 columns wraps the Beeb's cursor,
   and its CR LF then leaves a blank row */
#define HELPERS_USER_ROM_LINE "%u  %.35s\r\n"

/* The file each of helpers 10-15 loads - the name after "Pi1MHz/", ""
   for none - as the loader's wildcard open chooses it
   (fat_service_wild_leaves: one pass of the Pi1MHz folder for all six).
   Read from the card in the poll loop, never in FIQ: see helpers_poll. */
static char helper_user_rom[6][40];
static bool helper_user_rom_scan;     /* read them again, when the player is idle */

static void helpers_user_rom_scan(void)
{
   static const char *const pats[6] = { "ROM10*.rom", "ROM11*.rom", "ROM12*.rom",
                                        "ROM13*.rom", "ROM14*.rom", "ROM15*.rom" };
   fat_service_wild_leaves("Pi1MHz/", pats, 6u, helper_user_rom[0], sizeof helper_user_rom[0]);
}

/* The %s of the help screen. */
static void helpers_user_roms(char *out, size_t size)
{
   size_t len = 0u;
   out[0] = '\0';
   for (unsigned int i = 0u; i < 6u; i++) {
      if (helper_user_rom[i][0] == '\0')
         continue;
      int n = snprintf(out + len, size - len, HELPERS_USER_ROM_LINE, 10u + i, helper_user_rom[i]);
      if (n < 0 || (size_t)n >= size - len)
         break;
      len += (size_t)n;
   }
   if (len == 0u)
      (void)snprintf(out, size, "%s", HELPERS_USER_ROM_NONE);
}


size_t helpers_screen_setup( char * helpscreen, size_t helpscreen_size)
{
        // We also hide the help screen at &FFE000 (1.5 KB there, ~600-850 B used).
        // Hand-formatted to tenths so this doesn't need newlib's float printf
        // support (dtoa machinery, ~5 KB) -- see CMakeLists.txt, -u _printf_float.
        // Integer arithmetic on purpose: the float printf support is not
        // linked (and this once ran in FIQ context, where FIQ.s saves no VFP
        // state).  Millidegrees to tenths, rounded.
        // Cannot go negative (0 on failure), so no sign handling.
        long temp_tenths = (long)((get_temp_millidegrees() + 50u) / 100u);
        char user_roms[6u * 42u + 1u];
        helpers_user_roms(user_roms, sizeof user_roms);
#ifdef DEBUG
#define HELPERS_KERNEL_LETTER "D"
#else
#define HELPERS_KERNEL_LETTER "R"
#endif
        int n = snprintf(helpscreen, helpscreen_size, HELPERS_HELP_FMT(HELPERS_KERNEL_LETTER),
                         GITVERSION,
                         get_info_string(), temp_tenths / 10, temp_tenths % 10,
                         BUILD_DATE + 2, BUILD_DATE + 5, BUILD_DATE + 8, BUILD_DATE + 11,
                         (unsigned int)helper_address,
                         (int)helper_address,
                         user_roms,
                         (int)(harddisc_emulator_get_address() + 1),
                         (int)M5000_emulator_read_instance(),
                         (int)harddisc_emulator_get_instance());
        // snprintf returns the would-be length, which may exceed the buffer;
        // clamp so the returned length (used for fb_writen) stays exact.
        if (n < 0)
            n = 0;
        if ((size_t)n >= helpscreen_size)
            n = (int)(helpscreen_size - 1u);
        return (size_t)n;
}

const char *helpers_firmware_version(void)
{
   return GITVERSION " " BUILD_DATE " " HELPERS_KERNEL_LETTER;
}

/* Transfer page state: touched in helpers_bank_select (FIQ), and cleared by
   helpers_init on a BBC reset - the JIM page it saved belongs to a session
   the reset has ended (rampage_emulator_init resets page_ram_addr itself). */
static bool   helper_xfer_active;
static size_t helper_xfer_saved_page;

static void helpers_bank_select(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   if (Pi1MHz_break.helper_pending) {       /* first helper select after a reset: BREAK forensics */
      Pi1MHz_break.helper_pending = 0u;
      Pi1MHz_break.helper_us = RPI_GetSystemTime();
      /* Only against a re-init that belongs to this reset (its end stamp is
         after the edge); otherwise the previous reset's init would be measured. */
      if ((int32_t)(Pi1MHz_break.init_end_us - Pi1MHz_break.rst_us) > 0) {
         int32_t margin = (int32_t)(Pi1MHz_break.helper_us - Pi1MHz_break.init_end_us);
         if (margin < Pi1MHz_break.margin_min_us)
            Pi1MHz_break.margin_min_us = margin;
      }
   }

   if (data == HELPER_XFER)
   {
      /* A Beeb write into the window lands in JIM_ram[page_ram_addr + offset]
         (ram_emulator_page_write), so pointing page_ram_addr at the transfer
         page is what routes a filing system's writes into it.  The user's own
         JIM RAM page is saved here and put back by the next select of any
         other kind - every way out of a helper, including its BRK trap, is
         one. */
      if (!helper_xfer_active) {
         helper_xfer_saved_page = Pi1MHz->page_ram_addr;
         helper_xfer_active = true;
      }
      Pi1MHz->page_ram_addr = DISC_RAM_BASE + HELPER_XFER_OFFSET;
      Pi1MHz_MemoryWrite_FIQ(addr+4, 0x4c);
      ram_emulator_page_restore();       /* shows JIM RAM: writes reach it */
      helper_window = -1;
      return;
   }

   if (helper_xfer_active) {
      Pi1MHz->page_ram_addr = helper_xfer_saved_page;
      helper_xfer_active = false;
   }

   if (data == 0xFF)
    {
        // put RTS in instruction stream
        Pi1MHz_MemoryWrite_FIQ(addr+4, 0x60);
        // old page data
        ram_emulator_page_restore();
        helper_window = -1;
    }
    else
    {
        // put JMP instruction stream
        Pi1MHz_MemoryWrite_FIQ(addr+4, 0x4c);
        // select page
        if (data >= HELPER_PAGES)
            data = 0;

        Pi1MHz_MemoryWritePage(Pi1MHz_MEM_PAGE, &helper_ram[data<<8]);
        ram_emulator_window_helper();    /* its writes stay out of the user's JIM RAM */
        helper_window = data;
        if (data==0)
        {
            /* &FFE000 is also the MMFS/MMFS2 sector buffer, so the screen
               goes there only now, when the Beeb has asked for it - a copy,
               not the 1 KB format, which is too long for the FIQ.  Words,
               with its terminating zero; a plain loop, not memcpy, because
               the FIQ saves no VFP state. */
            uint8_t f = help_front;
            const uint32_t *src = (const uint32_t *)(const void *)help_text[f];
            uint32_t *dst = (uint32_t *)(void *)&Pi1MHz->JIM_ram[HELP_JIM];
            for (unsigned int w = (help_len[f] + 4u) / 4u; w != 0u; w--)
                *dst++ = *src++;
            Pi1MHz_MemoryWrite_FIQ(Pi1MHz_MEM_PAGE+1, 0x03);   /* screen in place: release the waitloop */
            help_shown_us = Pi1MHz_now_us;
            help_shown_pending = true;                          /* reformat for next time, once printed */
        }
    }
}

/* What the screen prints comes from the mailbox, which is main-loop only -
   one static property buffer, no locking - so it is cached
   (info_refresh_cached) and refreshed from the poll, and only while the
   player is idle: a mailbox exchange can block the poll loop (bounded only
   by the 3 s mailbox timeout) and stalling it mid-playback starves the
   Beeb's SCSI handshake. */
/* The help screen is formatted ahead of time, so the page-0 switch in FIQ
   only copies it into JIM and releases the Beeb (formatting there took
   tens of microseconds, long enough for the bus post ring to overrun while
   the Beeb fetched its waitloop from JIM).  It is formatted once at init
   and again two seconds after each showing, which keeps the temperature
   one showing old at most.  It is formatted here, not into JIM: that is
   the MMFS sector buffer, and two seconds on a filing system may be
   reading or writing through it. */
static void helpers_screen_refresh(void)
{
   uint8_t back = (uint8_t)(help_front ^ 1u);
   help_len[back] = (uint16_t)helpers_screen_setup(help_text[back], sizeof help_text[back]);
   help_front = back;
}

/* The user ROM files are read again after a BBC reset and after each
   showing, so a ROM copied to the card turns up at the next help screen
   after either.  Only while the player is idle: it reads the card, and
   the player streams from it. */
static void helpers_poll(void)
{
   if (help_shown_pending && (Pi1MHz_now_us - help_shown_us) > 2000000u) {
      help_shown_pending = false;
      helper_user_rom_scan = true;
      helpers_screen_refresh();
   }
   if (videoplayer_active())
      return;
   if (helper_user_rom_scan) {
      helper_user_rom_scan = false;
      helpers_user_rom_scan();
      helpers_screen_refresh();
   }
   info_refresh_cached();
}

int helpers_window_page(void) { return helper_window; }

const uint8_t *helpers_page(uint8_t page)
{
   return page < HELPER_PAGES ? &helper_ram[(size_t)page << 8] : NULL;
}

int helpers_page_number(uint8_t page)
{
   if (page >= HELPER_PAGES)
      return -1;
   return helper_ram[((size_t)page << 8) + 255u];
}

void helpers_init( uint8_t instance , uint8_t address)
{
   uint8_t *helper = &helper_ram[0];
   helper_address = address;
   help_shown_pending = false;      /* the screen is formatted again below */
   helper_xfer_active = false;      /* a BREAK mid-transfer: nothing to restore */
   helper_window = -1;
   if (Pi1MHz->JIM_ram_size == 0)     // the help screen lives in JIM RAM
      return;
   /* 0x60 = RTS.  filesystemReadFile clamps to the file's length and leaves
      the rest of the buffer alone, and this buffer is .noinit - so an older
      or truncated 6502code.bin would otherwise leave a page of whatever
      survived the last boot, and the Beeb would JMP into it.  Filled first,
      an absent page just returns. */
   memset(helper_ram, 0x60, sizeof helper_ram);
   if (filesystemReadFile("Pi1MHz/6502code.bin",&helper,sizeof(helper_ram)))
    {
        static const uint8_t stub_pages[] = { SERIAL_HELPER_PAGE, TELNET_HELPER_PAGE };
        for (unsigned int i = 0u; i < sizeof stub_pages; i++) {
            uint8_t *ser = &helper_ram[stub_pages[i] * 256u + SERIAL_HELPER_LDY];
            if (ser[0] == 0xA0)       /* LDY #: an older 6502code.bin has no such page */
                ser[1] = serial_redirect_address();
        }

        // register call backs
        Pi1MHz_Register_Memory(WRITE_FRED, address, helpers_bank_select );
        /* Prime once here, before anything can be playing, so the FIQ path
           can never be the first to touch the mailbox - then keep it fresh
           from the poll loop. */
        info_refresh_cached();
        helpers_screen_refresh();
        helper_user_rom_scan = true;
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

