/*
  The FAT/SD service: commands 0-20 on the services port (&FCA6).

  Enables the Beeb to access the SDCARD.  16Mbytes of the JIM buffer is
  available as the transfer buffer; command blocks live in its top pages.
*/

#include <string.h>
#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "services.h"
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

static void fat_service_command(uint32_t command_pointer, uint32_t addr, uint8_t data)
{
   uint32_t base_addr = DISC_RAM_BASE ;

   switch (  Pi1MHz->JIM_ram[command_pointer] )
   {
    case 0 :
    {
        uint32_t buf_off = jim_read32(command_pointer+4);
        uint32_t sectors = jim_read32(command_pointer+12);
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
        // disk_write transfers 'sectors' x 512-byte blocks from the buffer
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
        // Filename defined to be zero terminated string at command_pointer+3, mode in command_pointer+2
        if (!discaccess_string_ok(command_pointer+3))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
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
        Pi1MHz_MemoryWrite(addr,
             f_mkdir( (char * )&Pi1MHz->JIM_ram[command_pointer + 1] ) );
        break;

    case 11 : // fchdir
        if (!discaccess_string_ok(command_pointer + 1))
        {
            Pi1MHz_MemoryWrite(addr, FR_INVALID_PARAMETER);
            break;
        }
        Pi1MHz_MemoryWrite(addr,
             f_chdir( (char * )&Pi1MHz->JIM_ram[command_pointer + 1] ) );
        break;

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
        Pi1MHz_MemoryWrite(addr,
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
        Pi1MHz_MemoryWrite(addr,
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
   (void)services_register(SERVICE_CMD_FAT_FIRST, SERVICE_CMD_FAT_LAST,
                           fat_service_command);
}
