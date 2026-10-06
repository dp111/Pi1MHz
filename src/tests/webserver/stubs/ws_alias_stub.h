#pragma once
/* Host-test stand-in for the one FatFs call the path parsers make:
   ws_resolve_aliases asks f_stat for the long name behind an 8.3 alias.
   The "card" is a table of alias paths and the long names FatFs would
   report for them; anything else does not exist. */
#include <string.h>
#include <strings.h>

typedef enum { FR_OK = 0, FR_NO_FILE = 4 } FRESULT;
typedef struct { char fname[256]; } FILINFO;

static const struct { const char *alias; const char *lfn; } ws_alias_card[] = {
   { "/BEEBSC~1",                  "BeebSCSI0" },
   { "/BeebSCSI0/BACKUP~1.DAT",    "backup of scsi0.dat" },
   { "/games/LONGNA~1.SSD",        "longname disc.ssd" },
   { "/BEEBVF~1",                  "BeebVFS0" },
};

static FRESULT f_stat(const char *p, FILINFO *fno)
{
   for (size_t i = 0u; i < sizeof ws_alias_card / sizeof ws_alias_card[0]; i++)
      if (strcasecmp(p, ws_alias_card[i].alias) == 0) {
         strcpy(fno->fname, ws_alias_card[i].lfn);
         return FR_OK;
      }
   return FR_NO_FILE;
}
