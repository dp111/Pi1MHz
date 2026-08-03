/*
  The FAT/SD service: commands 0-20 on the services port (&FCA6).

  Enables the Beeb to access the SDCARD.  16Mbytes of the JIM buffer is
  available as the transfer buffer; command blocks live in its top pages.
*/

#include <stdio.h>
#include <string.h>
#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "services.h"
#include "config.h"				/* Beeb_write_protect */
#include "BeebSCSI/fatfs/ff.h"			/* Obtains integer types */
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"

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
static bool discaccess_buffer_ok(uint32_t offset, uint32_t length)
{
   if (offset > DISC_RAM_SIZE)
      return false;
   return length <= (DISC_RAM_SIZE - offset);
}

/* Returns true if a NUL terminator is found within DISC_MAX_PATH bytes of
 * JIM_ram[start] and before the end of the disc RAM region, so that
 * strlen()/FatFs cannot run off the end of the JIM_ram allocation.
 * 'start' is an absolute JIM_ram byte offset. */
static bool discaccess_string_ok(uint32_t start)
{
   uint32_t limit = start + DISC_MAX_PATH;
   if (limit > (uint32_t)(DISC_RAM_BASE + DISC_RAM_SIZE))
      limit = (uint32_t)(DISC_RAM_BASE + DISC_RAM_SIZE);
   for (uint32_t i = start; i < limit; i++)
      if (Pi1MHz->JIM_ram[i] == 0)
         return true;
   return false;
}

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
   for (unsigned int i = 0; i < 16u; i++)
      fat_open_valid[i] = false;
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

static void fat_service_command(uint32_t command_pointer, uint32_t addr, uint8_t data)
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
            !discaccess_buffer_ok(buf_off, sectors * DISC_SECTOR_SIZE))
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
            !discaccess_buffer_ok(buf_off, sectors * DISC_SECTOR_SIZE))
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
        if (!discaccess_string_ok(command_pointer+3))
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
        if (result == FR_OK)
            fat_open_record(data & 15, (char * )&Pi1MHz->JIM_ram[command_pointer+3]);
        Pi1MHz_MemoryWrite(addr, result);
        break;
    }
    case 3 :
        fat_open_valid[data & 15] = false;
        Pi1MHz_MemoryWrite(addr,
             f_close( &fileObject[data & 15] ) );
        break;
    case 4 :
    {
        FRESULT result;
        UINT length;
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t buf_len = jim_read32(command_pointer)>>8;
        if (!discaccess_buffer_ok(buf_off, buf_len))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
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
        if (!discaccess_buffer_ok(buf_off, buf_len))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
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
        jim_write32(command_pointer + 8, f_size( &fileObject[data & 15] ));
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 7 : // fopendir
        if (!discaccess_string_ok(command_pointer + 1))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr,
             f_opendir( (DIR * )&dirObject[data & 15], (char * )&Pi1MHz->JIM_ram[command_pointer + 1] ) );
        break;


    case 8: // fclosedir
        Pi1MHz_MemoryWrite(addr,
             f_closedir( (DIR * )&dirObject[data & 15] ) );
        break;


    case 9 : // f readdir
    {
        FRESULT result;
        FILINFO fileInfo;
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

        memcpy(&Pi1MHz->JIM_ram[command_pointer + 4], fileInfo.fname, strlen(fileInfo.fname)+1);
        Pi1MHz_MemoryWrite(addr, FR_OK);
        break;
    }

    case 10 : // f mkdir
        if (!discaccess_string_ok(command_pointer + 1))
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
        if (!discaccess_string_ok(command_pointer + 1))
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
        if (!discaccess_string_ok(name1))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        uint32_t name2 = name1 + (uint32_t)strlen((char * )&Pi1MHz->JIM_ram[name1]) + 1;
        if (!discaccess_string_ok(name2))
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
        FATFS *fs;
        DWORD fre_clust;
        FRESULT result = f_getfree("", &fre_clust, &fs);
        if (result)
            {
                Pi1MHz_MemoryWrite(addr, result);
                break;
            }
        // assumes sector size of 512 bytes
        jim_write32(command_pointer+8, (fs->csize * fre_clust) * 2);  // return free space in bytes/256
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
        if (!discaccess_string_ok(command_pointer + 1))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr, config_beeb_write_protected() ? FR_OK :
             f_unlink( (char * )&Pi1MHz->JIM_ram[command_pointer + 1] ) );
        break;

    case 20 : Pi1MHz_MemoryWrite(addr, disk_type()); break;

    default :
        /* 17-19 and 21-29 are reserved within the FAT range; ignored. */
        break;
   }

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
}
