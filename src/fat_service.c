/*
  The FAT/SD service: commands 0-20 on the services port (&FCA6).
  17 = readdir-ex (fixed records for the SD explorer), 18 = getcwd.

  Enables the Beeb to access the SDCARD.  16Mbytes of the JIM buffer is
  available as the transfer buffer; command blocks live in its top pages.
*/

#include <stdio.h>
#include <string.h>
#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "M5000_emulator.h"		/* M5000_recording_path_busy */
#include "services.h"
#include "config.h"				/* Beeb_write_protect */
#include "BeebSCSI/fatfs/ff.h"			/* Obtains integer types */
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"
#include "wifi/webserver.h"			/* cached SD free space - see case 13 */

NOINIT_SECTION static FIL fileObject[16];
NOINIT_SECTION static DIR dirObject[16];

/* 32-bit access into JIM RAM. The command block is always page-aligned
 * and JIM_ram is malloc'd, so &JIM_ram[off] is 4-byte aligned at every
 * call site below. memcpy + assume_aligned yields a single LDR/STR on
 * every CPU, with no strict-aliasing UB and no -Wcast-align warning. */
static inline uint32_t jim_read32(uint32_t off)
{
   uint32_t v;
   memcpy(&v, __builtin_assume_aligned(&Pi1MHz->JIM_ram[off], 4), sizeof v);
   return v;
}

static inline void jim_write32(uint32_t off, uint32_t v)
{
   memcpy(__builtin_assume_aligned(&Pi1MHz->JIM_ram[off], 4), &v, sizeof v);
}

/* The command structure handled by fat_service_command() is filled in
 * by the host (the Beeb), so every buffer offset, length and path string it
 * supplies is untrusted. Without the checks below a host-supplied offset is
 * added straight to base_addr and used to index JIM_ram[], which is an
 * arbitrary read/write primitive. These helpers keep every host-supplied
 * access inside the disc RAM region. */

/* FatFs is configured with a fixed 512-byte sector size (FF_MIN_SS ==
 * FF_MAX_SS == 512 in ffconf.h); disk_read()/disk_write() transfer whole
 * sectors of this size. */
#define DISC_SECTOR_SIZE 512u

/* Longest path string accepted from the host (well above any real FatFs
 * path); also bounds the terminator scan so a missing NUL cannot turn into
 * a multi-megabyte loop. */
#define DISC_MAX_PATH 1024u

/* Returns true if the data buffer [offset, offset+length) lies wholly inside
 * the disc RAM region. 'offset' is relative to base_addr. The subtraction
 * form cannot overflow because offset is bounded against DISC_RAM_SIZE
 * first. */


/* ---- open-file tracking for the webserver's in-use interlock ------------
   The Beeb opens files here (FIQ context); the webserver asks from the main
   loop whether a path is one of them before overwriting, deleting or moving
   it - the same protection the SCSI LUN images already have.  Paths are
   recorded absolute: relative opens are joined against a cwd cache that is
   refreshed on the rare chdir (f_getcwd walks directories, so it is not
   called per open).  The valid flag is set last on open and cleared first
   on close, so a torn read from the main loop cannot see a half-written
   path as valid.  A path too long to record, or an open under an unknown
   cwd, is left unrecorded: the interlock fails open rather than ever
   matching the wrong file. */
#define FAT_OPEN_PATH_MAX 130u
static volatile bool fat_open_valid[16];
static char fat_open_path[16][FAT_OPEN_PATH_MAX];
static char fat_cwd[FAT_OPEN_PATH_MAX] = "/";
static bool fat_cwd_known = true;
/* MMFS does NOT keep its image store open: it resolves BEEB.MMB to a start
   sector once and then reads and writes RAW SECTORS (commands 0/1), which
   file-handle tracking cannot see - and which assumes the file never moves.
   So once raw sector access has been used, BEEB.MMB is reported busy until
   a remount or reboot: replacing it would re-allocate clusters under a
   filing system holding absolute sector numbers. */
static volatile bool fat_raw_sector_seen;

/* Which fileObject[]/dirObject[] slots actually hold an opened FatFs object.
   Those arrays live in .noinit, which is deliberately NOT zeroed, so an
   un-opened slot holds whatever the previous session - or, on a cold boot,
   uninitialised DRAM - left there.  FatFs's validate() gate dereferences
   obj->fs, so a command naming a handle the ROM never opened made it load
   through a wild pointer, in FIQ context.  The handle is (data & 15), taken
   straight from the Beeb's command register, so it is entirely host-chosen.

   fat_open_valid[] cannot serve as this gate: it records whether the PATH was
   captured for the webserver interlock, and fat_open_record() gives up -
   leaving it false - on an unresolvable relative name even though the f_open
   succeeded.  These two live in .bss, so a cold boot, a watchdog reset and a
   chain-boot all start them false, which is the safe direction: refuse until
   the ROM re-opens. */
static volatile bool fat_file_open[16];
static volatile bool fat_dir_open[16];

static void fat_open_record(unsigned int handle, const char *name)
{
   char joined[FAT_OPEN_PATH_MAX];
   const char *path = name;

   fat_open_valid[handle] = false;
   if (name[0] != '/') {
      int n;
      if (!fat_cwd_known)
         return;
      n = snprintf(joined, sizeof joined, "%s/%s",
                   (fat_cwd[0] == '/' && fat_cwd[1] == '\0') ? "" : fat_cwd,
                   name);
      if (n < 0 || (size_t)n >= sizeof joined)
         return;
      path = joined;
   }
   if (strlen(path) >= sizeof fat_open_path[0])
      return;
   strcpy(fat_open_path[handle], path);
   fat_open_valid[handle] = true;
}

static void fat_open_clear_all(void)
{
   for (unsigned int i = 0; i < 16u; i++) {
      fat_open_valid[i] = false;
      fat_file_open[i] = false;
      fat_dir_open[i] = false;
   }
   strcpy(fat_cwd, "/");
   fat_cwd_known = true;
   fat_raw_sector_seen = false;
}

/* FAT names are case-insensitive, and either side may carry a "0:" drive
   prefix or leading slashes. */
static const char *fat_path_norm(const char *p)
{
   if (p[0] == '0' && p[1] == ':')
      p += 2;
   while (*p == '/')
      p++;
   return p;
}

bool fat_service_file_in_use(const char *host_path)
{
   const char *q = fat_path_norm(host_path);
   size_t qlen = strlen(q);

   /* The raw-sector client's store (see fat_raw_sector_seen above).  MMFS
      reads BEEB.MMB from the ROOT of the card (its own minimal FAT reader
      scans the root directory for that fixed name), so the file can only
      live there - matching a BEEB.MMB basename (case-insensitive) and the
      root path covers it.  We do not need to walk parent directories: MMFS
      cannot reach an MMB in a subdirectory, so no such directory can hold
      the sectors it cached. */
   if (fat_raw_sector_seen) {
      static const char mmb[] = "BEEB.MMB";
      size_t base = qlen;
      while (base > 0u && q[base - 1u] != '/')
         base--;
      if (qlen == 0u)
         return true;              /* the root holds BEEB.MMB */
      if (qlen - base == sizeof mmb - 1u) {
         size_t k;
         for (k = 0; k < sizeof mmb - 1u; k++) {
            char c = q[base + k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c != mmb[k])
               break;
         }
         if (k == sizeof mmb - 1u)
            return true;
      }
   }

   for (unsigned int i = 0; i < 16u; i++) {
      const char *p;
      size_t j;

      if (!fat_open_valid[i])
         continue;
      p = fat_path_norm(fat_open_path[i]);
      for (j = 0; j < qlen; j++) {
         char ca = p[j], cb = q[j];
         if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
         if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
         if (ca != cb)
            break;
      }
      /* Equal - or host_path is a directory containing the open file, so
         a recursive DELETE/MOVE of the folder is refused too. */
      if (j == qlen && (p[qlen] == '\0' || p[qlen] == '/' || qlen == 0u))
         return true;
   }
   return false;
}

/* The one predicate every host-side writer (WebDAV, MTP) asks before it
   overwrites, deletes or renames a path: is the Beeb using it by ANY route -
   a started SCSI LUN image (or a directory holding one), a file open
   through this service, or the WAV the Music 5000 is still writing out.  A host write that lands on a running LUN's cluster
   chain is silent corruption, so the two halves live together here rather
   than being re-joined at each call site. */
bool beeb_path_busy(const char *host_path)
{
   return filesystemHostPathBusy(host_path) || fat_service_file_in_use(host_path)
       || M5000_recording_path_busy(host_path);    /* a WAV still being flushed */
}

/* ---- readdir-ex (command 17) record ------------------------------------
   A fixed 128-byte record per directory entry, laid out for a 6502 client:
   power-of-two size so entry N is a shift away, and a ready-to-print
   38-column display line so the Beeb never has to format sizes itself.

     +0        attribute byte (AM_DIR etc., see FatFs FILINFO.fattrib)
     +1..3     reserved (0)
     +4..7     file size, little-endian (0 for a directory)
     +8..46    display line: exactly FAT_DIRENT_DISP printable chars
               (name, then right-aligned size or <DIR>), NUL terminated
     +48..127  raw name for fopen/fchdir, NUL terminated (truncated if
               longer than 79 chars)                                      */
#define FAT_DIRENT_SIZE  128u
#define FAT_DIRENT_DISP   38u
#define FAT_DIRENT_NAME  (FAT_DIRENT_SIZE - 48u)
#define FAT_CWD_MAX      128u

static void fat_dirent_record(uint8_t *rec, const FILINFO *info)
{
   char size_text[12];
   char name[FAT_DIRENT_DISP];     /* display copy, truncated + sanitized */
   size_t n;

   memset(rec, 0, FAT_DIRENT_SIZE);
   rec[0] = info->fattrib;
   rec[4] = (uint8_t)(info->fsize);
   rec[5] = (uint8_t)(info->fsize >> 8);
   rec[6] = (uint8_t)(info->fsize >> 16);
   rec[7] = (uint8_t)(info->fsize >> 24);

   /* Right-hand column: <DIR>, bytes, KB or MB - always fits 7 chars. */
   if (info->fattrib & AM_DIR)
      strcpy(size_text, "<DIR>");
   else if (info->fsize < 1000000u)
      sprintf(size_text, "%lu", (unsigned long)info->fsize);
   else if ((info->fsize >> 10) < 1000000u)
      sprintf(size_text, "%luK", (unsigned long)(info->fsize >> 10));
   else
      sprintf(size_text, "%luM", (unsigned long)(info->fsize >> 20));

   /* Display name: truncate to the name column, mark directories with a
      trailing '/', and replace anything a teletext VDU stream would
      interpret as a control code. */
   n = 0;
   while (n < sizeof name - 2u && info->fname[n]) {
      char c = info->fname[n];
      name[n] = (c < 32 || c > 126) ? '?' : c;
      n++;
   }
   if ((info->fattrib & AM_DIR) && n < sizeof name - 1u)
      name[n++] = '/';
   name[n] = '\0';

   sprintf((char *)&rec[8], "%-*.*s%7s",
           (int)(FAT_DIRENT_DISP - 7u), (int)(FAT_DIRENT_DISP - 7u),
           name, size_text);

   /* Raw name, for the Beeb to hand back to fopen/fchdir. */
   n = strlen(info->fname);
   if (n >= FAT_DIRENT_NAME)
      n = FAT_DIRENT_NAME - 1u;
   memcpy(&rec[48], info->fname, n);
   rec[48u + n] = '\0';
}

/* Runs on the main loop (fat_service_poll): every FatFs and SD call below
   may block for milliseconds, which the FIQ cannot afford - see
   fat_service_command. */
static void fat_service_execute(uint32_t command_pointer, uint32_t addr, uint8_t data)
{
   uint32_t base_addr = DISC_RAM_BASE ;

   switch (  Pi1MHz->JIM_ram[command_pointer] )
   {
    case 0 :
    {
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t sectors = jim_read32(command_pointer+12);
        fat_raw_sector_seen = true;
        // disk_read transfers 'sectors' x 512-byte blocks into the buffer
        if ((sectors > (DISC_RAM_SIZE / DISC_SECTOR_SIZE)) ||
            !service_buffer_ok(buf_off, sectors * DISC_SECTOR_SIZE))
        {
            Pi1MHz_MemoryWrite(addr, RES_PARERR);
            break;
        }
        Pi1MHz_MemoryWrite(addr,
            disk_read( Pi1MHz->JIM_ram[command_pointer+1],
                        &Pi1MHz->JIM_ram[buf_off+base_addr],
                        jim_read32(command_pointer+8),
                        sectors
                        ) );
        break;
    }
    case 1 :
    {
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t sectors = jim_read32(command_pointer+12);
        fat_raw_sector_seen = true;
        // disk_write transfers 'sectors' x 512-byte blocks from the buffer
        if (config_beeb_write_protected())      // Beeb writes ignored: report OK
        {
            Pi1MHz_MemoryWrite(addr, RES_OK);
            break;
        }
        if ((sectors > (DISC_RAM_SIZE / DISC_SECTOR_SIZE)) ||
            !service_buffer_ok(buf_off, sectors * DISC_SECTOR_SIZE))
        {
            Pi1MHz_MemoryWrite(addr, RES_PARERR);
            break;
        }
        Pi1MHz_MemoryWrite(addr,
            disk_write( Pi1MHz->JIM_ram[command_pointer+1],
                        &Pi1MHz->JIM_ram[buf_off+base_addr],
                        jim_read32(command_pointer+8) ,
                        sectors )
                        );
        break;
    }
    case 2 :
    {
        FRESULT result;
        // Filename defined to be zero terminated string at command_pointer+3, mode in command_pointer+2
        if (!service_string_ok(command_pointer+3, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        fat_open_valid[data & 15] = false;   /* re-open replaces any record */
        BYTE mode = Pi1MHz->JIM_ram[command_pointer+2];
        if (config_beeb_write_protected())
            mode = FA_READ;                  /* strip write/create bits: read-only open */
        result = f_open( &fileObject[data & 15], (char * )&Pi1MHz->JIM_ram[command_pointer+3]
                    , mode );
        if (result == FR_OK) {
            fat_file_open[data & 15] = true;
            fat_open_record(data & 15, (char * )&Pi1MHz->JIM_ram[command_pointer+3]);
        }
        Pi1MHz_MemoryWrite(addr, result);
        break;
    }
    case 3 :
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        fat_open_valid[data & 15] = false;
        fat_file_open[data & 15] = false;
        Pi1MHz_MemoryWrite(addr,
             f_close( &fileObject[data & 15] ) );
        break;
    case 4 :
    {
        FRESULT result;
        UINT length;
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t buf_len = jim_read32(command_pointer)>>8;
        if (!service_buffer_ok(buf_off, buf_len))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        result = f_lseek( &fileObject[data & 15], jim_read32(command_pointer+8) );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = f_read( &fileObject[data & 15], &Pi1MHz->JIM_ram[buf_off+base_addr] , buf_len , &length);
        jim_write32(command_pointer, (length << 8 ) | Pi1MHz->JIM_ram[command_pointer]);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length < buf_len )
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
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t buf_len = jim_read32(command_pointer)>>8;
        if (!service_buffer_ok(buf_off, buf_len))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        if (config_beeb_write_protected())      // Beeb write ignored: claim it all landed
        {
            jim_write32(command_pointer, (buf_len << 8 ) | Pi1MHz->JIM_ram[command_pointer]);
            Pi1MHz_MemoryWrite(addr, FR_OK);
            break;
        }
        result = f_lseek( &fileObject[data & 15], jim_read32(command_pointer+8) );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        result = f_write( &fileObject[data & 15], &Pi1MHz->JIM_ram[buf_off+base_addr] , buf_len , &length);
        jim_write32(command_pointer, (length << 8 ) | Pi1MHz->JIM_ram[command_pointer]);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if ( length < buf_len )
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }
    case 6 : // fsize
    {
        if (!fat_file_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        jim_write32(command_pointer + 8, f_size( &fileObject[data & 15] ));
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 7 : // fopendir
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        {
            FRESULT dresult = f_opendir( (DIR * )&dirObject[data & 15],
                                         (char * )&Pi1MHz->JIM_ram[command_pointer + 1] );
            if (dresult == FR_OK)
                fat_dir_open[data & 15] = true;
            Pi1MHz_MemoryWrite(addr, dresult);
        }
        break;


    case 8: // fclosedir
        if (!fat_dir_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        fat_dir_open[data & 15] = false;
        Pi1MHz_MemoryWrite(addr,
             f_closedir( (DIR * )&dirObject[data & 15] ) );
        break;


    case 9 : // f readdir
    {
        FRESULT result;
        FILINFO fileInfo;
        if (!fat_dir_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        result = f_readdir( (DIR * )&dirObject[data & 15], &fileInfo );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if (fileInfo.fname[0] == 0)
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }

        {
            /* Cap to the 252 bytes left in this 256-byte command page: a
               255-char LFN + NUL otherwise spills 4 bytes into the next
               page's command block. */
            size_t nlen = strlen(fileInfo.fname);
            if (nlen > 251) nlen = 251;
            memcpy(&Pi1MHz->JIM_ram[command_pointer + 4], fileInfo.fname, nlen);
            Pi1MHz->JIM_ram[command_pointer + 4 + nlen] = 0;
        }
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 10 : // f mkdir
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             f_mkdir( (char * )&Pi1MHz->JIM_ram[command_pointer + 1] ) );
        break;

    case 11 : // fchdir
    {
        FRESULT result;
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        result = f_chdir( (char * )&Pi1MHz->JIM_ram[command_pointer + 1] );
        if (result == FR_OK)
            fat_cwd_known = (f_getcwd(fat_cwd, sizeof fat_cwd) == FR_OK);
        Pi1MHz_MemoryWrite(addr, result);
        break;
    }

    case 12 : // f_rename
    {
        // Two NUL-terminated names back to back; the second starts after the first.
        uint32_t name1 = command_pointer + 1;
        if (!service_string_ok(name1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        uint32_t name2 = name1 + (uint32_t)strlen((char * )&Pi1MHz->JIM_ram[name1]) + 1;
        if (!service_string_ok(name2, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             f_rename( (char * )&Pi1MHz->JIM_ram[name1] ,
                       (char * )&Pi1MHz->JIM_ram[name2] ) );
        break;
    }

    case 13 : // fgetfree
    {
        /* The Beeb blocks on this, but so does everything else: f_getfree
           walks the whole FAT (FF_FS_NOFSINFO=3) and used to freeze the
           poll loop - audio, video and SCSI servicing included - for
           seconds each time a program asked for free space. Answer from
           the webserver's background sweep, which pays at most one
           blocking scan ever and refreshes itself afterwards. The figure
           can lag recent writes; it is advisory (a free-space display),
           and the alternative is a multi-second stall per query. */
        uint64_t total_bytes = 0, free_bytes = 0;
        if (!webserver_sd_space_now(&total_bytes, &free_bytes))
            {
                Pi1MHz_MemoryWrite(addr, FR_DISK_ERR);
                break;
            }
        jim_write32(command_pointer+8, (uint32_t)(free_bytes / 256u));  // free space in bytes/256
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 14 : // f mount
        fat_open_clear_all();
        if (filesystemMount())
         {
            Pi1MHz_MemoryWrite(addr, FR_OK);
         }
         else
         {
            Pi1MHz_MemoryWrite(addr, FR_DISK_ERR);
         }
        break;

    case 15 : // f unmount
        fat_open_clear_all();
        if (filesystemDismount())
        {
            Pi1MHz_MemoryWrite(addr, FR_OK);
        }
        else
        {
            Pi1MHz_MemoryWrite(addr, FR_DISK_ERR);
        }
        break;
    case 16 : // f_unlink
        if (!service_string_ok(command_pointer + 1, DISC_MAX_PATH))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             f_unlink( (char * )&Pi1MHz->JIM_ram[command_pointer + 1] ) );
        break;

    case 17 : // readdir-ex: next entry as a fixed 128-byte record in the buffer
    {
        FRESULT result;
        FILINFO fileInfo;
        uint32_t buf_off = jim_read32(command_pointer+4);
        if (!service_buffer_ok(buf_off, FAT_DIRENT_SIZE))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        if (!fat_dir_open[data & 15])
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_OBJECT);
            break;
        }
        result = f_readdir( (DIR * )&dirObject[data & 15], &fileInfo );
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        if (fileInfo.fname[0] == 0)
        {
                Pi1MHz_MemoryWrite(addr, 20);
                break;
        }
        fat_dirent_record(&Pi1MHz->JIM_ram[buf_off + base_addr], &fileInfo);
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 18 : // getcwd: current directory as a string in the buffer
    {
        char cwd[FAT_CWD_MAX];
        uint32_t buf_off = jim_read32(command_pointer+4);
        FRESULT result;
        if (!service_buffer_ok(buf_off, FAT_CWD_MAX))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        result = f_getcwd(cwd, sizeof cwd);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        for (uint32_t i = 0; cwd[i]; i++)
            if ((uint8_t)cwd[i] < 32u || (uint8_t)cwd[i] > 126u)
                cwd[i] = '?';           /* keep the Beeb's VDU stream safe */
        memcpy(&Pi1MHz->JIM_ram[buf_off + base_addr], cwd, strlen(cwd)+1);
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 20 : Pi1MHz_MemoryWrite(addr, disk_type()); break;

    default :
        /* 19 and 21-29 are reserved within the FAT range; ignored. */
        break;
   }

}

/* One-slot command mailbox latched in FIQ, drained by the poll (the net
   service's pattern).  The services port dispatches command-register
   writes from the FIQ; the FatFs work behind them reads the SD card for
   milliseconds, and a FIQ held that long lets the 8-slot bus post ring
   overrun whenever the Beeb keeps the bus busy meanwhile - the helper ROM
   loader executes from JIM, so every instruction fetch is a posted cycle.
   The ring then drops the writes that follow, and the loader hung on its
   next command.  The Beeb already polls bit 7 of the command register, so
   deferring the work changes nothing on its side. */
static volatile bool     fat_pending;
static volatile uint32_t fat_pending_cp;
static volatile uint32_t fat_pending_addr;
static volatile uint8_t  fat_pending_data;

static void fat_service_command(uint32_t command_pointer, uint32_t addr, uint8_t data)
{
   /* FIQ context: latch only.  The register must read busy (bit 7) until
      the poll writes the real result; every Beeb client sends a command
      byte with bit 7 set, so this is the byte it wrote back with the bit
      forced for safety. */
   fat_pending_cp   = command_pointer;
   fat_pending_addr = addr;
   fat_pending_data = data;
   fat_pending      = true;
   Pi1MHz_MemoryWrite(addr, (uint8_t)(data | 0x80u));
}

static void fat_service_poll(void)
{
   if (!fat_pending)
      return;
   uint32_t cp   = fat_pending_cp;
   uint32_t addr = fat_pending_addr;
   uint8_t  data = fat_pending_data;
   fat_pending = false;
   fat_service_execute(cp, addr, data);
}

void fat_service_init(void)
{
   /* Runs on every BBC RST (init_emulator re-runs the whole table).  The
      Beeb-side filing system restarts on reset and abandons whatever it had
      open through this service, so drop the open-file tracking and the
      raw-sector (BEEB.MMB) latch: otherwise the webserver's in-use
      interlock would report ghosts of a pre-reset session as busy until the
      Pi itself rebooted.  If the Beeb re-opens files after the reset, the
      tracking simply re-populates. */
   fat_open_clear_all();
   (void)services_register(SERVICE_CMD_FAT_FIRST, SERVICE_CMD_FAT_LAST,
                           fat_service_command);
   /* fat_pending is deliberately not cleared here: a command latched around
      the reset is still answered by the poll (against the now-closed
      handles), so the busy bit the FIQ wrote can never be stranded. */
   Pi1MHz_Register_Poll(fat_service_poll, "fatsvc");
}
