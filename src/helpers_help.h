/* helpers_help.h - the help screen (helper 0, and the Pi's boot splash).

   One printf template, laid out for the Beeb's 40 x 25 text screen: every
   line is 40 columns or fewer and the whole screen is 20 rows, leaving five
   free for future helpers.  src/tests/helpers checks both against worst-case
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
#ifndef HELPERS_HELP_H
#define HELPERS_HELP_H

#define HELPERS_HELP_FMT(build_date, kernel)                             \
   "Pi1MHz %s\r\n"                                                       \
   "Built " build_date " " kernel "\r\n"                                 \
   "Pi %s %ld.%ldC\r\n"                                                  \
   "\r\n"                                                                \
   "Run helper n:  X%%=n:CALL &FC%X\r\n"                                 \
   "  or *FX147,%d,n then *GO[IO] FD00\r\n"                              \
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
   "*FX147,202,%d:*FX147,203,1/0 M5000 rec\r\n"

#define HELPERS_HELP_COLUMNS 40u
#define HELPERS_HELP_ROWS    25u

#endif
