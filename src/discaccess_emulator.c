/*
  Creates methods to enable a beeb to access the SDCARD

 16Mbytes is available as a buffer

*/

#include <string.h>
#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "econet_emulator.h"
#include "BeebSCSI/fatfs/ff.h"			/* Obtains integer types */
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"

static size_t disc_ram_addr;
static size_t disc_ram_max;
// High byte (bits 16-23) of the address last written to the +2 read-back
// register; -1 means "not written yet" (reset on init).
static int disc_ram_addr_hi;

static uint8_t ram_address;

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

/* The command structure handled by discaccess_emulator_command() is filled in
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
   if (limit > (uint32_t)disc_ram_max)
      limit = (uint32_t)disc_ram_max;
   for (uint32_t i = start; i < limit; i++)
      if (Pi1MHz->JIM_ram[i] == 0)
         return true;
   return false;
}

static void discaccess_emulator_update_address(void)
{
   // Write the low 16 bits of the address back for the Beeb to read, and
   // refresh the high-byte (bits 16-23) register only when it actually
   // changes - the common sequential-access case skips the extra write.
   // Comparing against the last value written (rather than reconstructing
   // the previous address as disc_ram_addr-1) is correct for every caller,
   // including the byte_addr path that sets the address arbitrarily.
   int hi = (int)((disc_ram_addr >> 16) & 0xFF);
   Pi1MHz_MemoryWrite16(ram_address, disc_ram_addr);
   if (hi != disc_ram_addr_hi)
   {
      disc_ram_addr_hi = hi;
      Pi1MHz_MemoryWrite((uint32_t)(ram_address + 2), (uint8_t)hi);
   }
}

static void discaccess_emulator_byte_addr(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   switch (addr - ram_address)
   {
      case 0:  disc_ram_addr = (size_t) ((disc_ram_addr & 0xFFFFFF00) | data); break;
      case 1:  disc_ram_addr = (size_t) ((disc_ram_addr & 0xFFFF00FF) | (size_t)(data<<8)); break;
      default: disc_ram_addr = (size_t) ((disc_ram_addr & 0xFF00FFFF) | (size_t)(data<<16)); break;
   }

   Pi1MHz_MemoryWrite((uint32_t)(ram_address + 3) , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   discaccess_emulator_update_address();              // enable the address register to be read back
}

static void discaccess_emulator_byte_write_inc(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   Pi1MHz->JIM_ram[disc_ram_addr] =  data;
   disc_ram_addr++;
   if (disc_ram_addr >= disc_ram_max) disc_ram_addr = DISC_RAM_BASE;
   Pi1MHz_MemoryWrite((uint32_t)(ram_address + 3) , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   discaccess_emulator_update_address();
}

static void discaccess_emulator_byte_read_inc(unsigned int gpio)
{
   disc_ram_addr++;
   if (disc_ram_addr >= disc_ram_max) disc_ram_addr = DISC_RAM_BASE;
   Pi1MHz_MemoryWrite((uint32_t)(ram_address + 3) , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   discaccess_emulator_update_address();
}

static void discaccess_emulator_command(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   Pi1MHz_MemoryWrite(addr, data); // return existing command
   uint32_t base_addr = DISC_RAM_BASE ;

   // command pointer is always page aligned
   uint32_t command_pointer = (uint32_t) (base_addr | 0xFF0000U | (uint32_t) (data<<8));
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
        // 30..44 Econet over AUN/UDP
        if (Pi1MHz->JIM_ram[command_pointer] >= ECO_CMD_FIRST &&
            Pi1MHz->JIM_ram[command_pointer] <= ECO_CMD_LAST)
            econet_emulator_command(command_pointer, addr);
        break;
   }

}


void discaccess_emulator_init( uint8_t instance , uint8_t address)
{
   disc_ram_addr = DISC_RAM_BASE;
   disc_ram_max  = DISC_RAM_BASE + DISC_RAM_SIZE;
   disc_ram_addr_hi = -1;   // force the +2 read-back register to be written on the first update

   ram_address = address;

   // register call backs
   // byte memory address write
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(ram_address+0), discaccess_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(ram_address+1), discaccess_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(ram_address+2), discaccess_emulator_byte_addr );
   // data byte
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(ram_address+3), discaccess_emulator_byte_write_inc );
   Pi1MHz_Register_Memory(READ_FRED , (uint8_t)(ram_address+3), discaccess_emulator_byte_read_inc );
   // command pointer
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(ram_address+4), discaccess_emulator_command );

   econet_emulator_init(); // econet commands (30+) share this command interface

   Pi1MHz_MemoryWrite((uint32_t)(ram_address+4), 0 ) ; // make sure command is null on read back
}
