/*
  The services port: a command mailbox at &FCA6 (formerly "Discaccess").

  +0/+1/+2  24-bit address into the JIM buffer (low/mid/high)
  +3        data port, auto-incrementing through the buffer
  +4        command pointer: writing &F0-&FF dispatches the command block
            at the top of the buffer (&F0 -> 0xFFF000, ... &FF -> 0xFFFF00)
  +5        IRQ status (used by services that raise nIRQ, e.g. AUN)

  The command's first byte selects the service: each service claims a range
  in services.h and registers a handler here.  Everything below runs in FIQ
  context off the FRED write callbacks.
*/

#include "Pi1MHz.h"

#include "ram_emulator.h"
#include "services.h"

static size_t disc_ram_addr;
static size_t disc_ram_max;
// High byte (bits 16-23) of the address last written to the +2 read-back
// register; -1 means "not written yet" (reset on init).
static int disc_ram_addr_hi;

static uint8_t ram_address;

typedef struct {
   uint8_t first;
   uint8_t last;
   service_command_fn handler;
} service_range_t;

#define SERVICES_MAX 4u
static service_range_t s_services[SERVICES_MAX];
static unsigned int s_service_count;

bool services_register(uint8_t first, uint8_t last, service_command_fn handler)
{
   if (handler == NULL || first > last || s_service_count >= SERVICES_MAX)
      return false;

   for (unsigned int i = 0; i < s_service_count; i++)
      if (first <= s_services[i].last && last >= s_services[i].first)
         return false;            /* overlaps a claimed range */

   s_services[s_service_count].first = first;
   s_services[s_service_count].last = last;
   s_services[s_service_count].handler = handler;
   s_service_count++;
   return true;
}

static void services_emulator_update_address(void)
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

static void services_emulator_byte_addr(unsigned int gpio)
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
   services_emulator_update_address();              // enable the address register to be read back
}

static void services_emulator_byte_write_inc(unsigned int gpio)
{
   uint8_t data = GET_DATA(gpio);
   Pi1MHz->JIM_ram[disc_ram_addr] =  data;
   disc_ram_addr++;
   if (disc_ram_addr >= disc_ram_max) disc_ram_addr = DISC_RAM_BASE;
   Pi1MHz_MemoryWrite((uint32_t)(ram_address + 3) , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   services_emulator_update_address();
}

static void services_emulator_byte_read_inc(unsigned int gpio)
{
   disc_ram_addr++;
   if (disc_ram_addr >= disc_ram_max) disc_ram_addr = DISC_RAM_BASE;
   Pi1MHz_MemoryWrite((uint32_t)(ram_address + 3) , Pi1MHz->JIM_ram[disc_ram_addr]); // setup new data now the address has changed;
   services_emulator_update_address();
}

static void services_emulator_command(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);

   Pi1MHz_MemoryWrite(addr, data); // return existing command

   // command pointer is always page aligned
   uint32_t command_pointer = (uint32_t) (DISC_RAM_BASE | 0xFF0000U | (uint32_t) (data<<8));
   uint8_t command = Pi1MHz->JIM_ram[command_pointer];

   for (unsigned int i = 0; i < s_service_count; i++) {
      if (command >= s_services[i].first && command <= s_services[i].last) {
         s_services[i].handler(command_pointer, addr, data);
         return;
      }
   }
   /* Unclaimed command numbers are ignored, as they always were. */
}

void services_emulator_init( uint8_t instance , uint8_t address)
{
   (void)instance;
   disc_ram_addr = DISC_RAM_BASE;
   disc_ram_max  = DISC_RAM_BASE + DISC_RAM_SIZE;
   disc_ram_addr_hi = -1;   // force the +2 read-back register to be written on the first update

   ram_address = address;

   /* The FAT/SD service is intrinsic to the port; other services (AUN)
      register from their own emulator-table inits, which run later. */
   fat_service_init();

   // register call backs
   // byte memory address write
   Pi1MHz_Register_Memory(WRITE_FRED, (ram_address+0u), services_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, (ram_address+1u), services_emulator_byte_addr );
   Pi1MHz_Register_Memory(WRITE_FRED, (ram_address+2u), services_emulator_byte_addr );
   // data byte
   Pi1MHz_Register_Memory(WRITE_FRED, (ram_address+3u), services_emulator_byte_write_inc );
   Pi1MHz_Register_Memory(READ_FRED , (ram_address+3u), services_emulator_byte_read_inc );
   // command pointer
   Pi1MHz_Register_Memory(WRITE_FRED, (ram_address+4u), services_emulator_command );

   Pi1MHz_MemoryWrite((uint32_t)(ram_address+4), 0 ) ; // make sure command is null on read back
   Pi1MHz_MemoryWrite((uint32_t)(ram_address+5), 0 ) ; // clear IRQ register
}
