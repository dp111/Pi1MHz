#ifndef SERVICES_H
#define SERVICES_H

/* The services port - the &FCA6 command mailbox (formerly "Discaccess").

   Three FRED registers set a 24-bit address into the JIM buffer, +3 is an
   auto-incrementing data port, and a write to +4 dispatches the page-aligned
   command block it names.  The FAT/SD commands were the first user, AUN
   (Econet over UDP) the second; a future Beeb-visible service should claim
   a command range here rather than a new FRED base.

   This header is the ONE authority for command-space allocation - a new
   service's range is added below, so a collision is a merge conflict
   instead of two switch statements silently fighting over a number. */

#include <stdint.h>
#include <stdbool.h>

#define SERVICE_CMD_FAT_FIRST    0u   /* FAT/SD access - fat_service.c     */
#define SERVICE_CMD_FAT_LAST    29u
#define SERVICE_CMD_AUN_FIRST   30u   /* Econet over AUN/UDP - AUN/        */
#define SERVICE_CMD_AUN_LAST    44u
/* 45..255 unallocated */

/* Handler for one service's command range.  FIQ context: called from the
   FRED write callback, so anything slow must be queued for the main loop
   (the AUN service is the pattern).  command_pointer is the absolute JIM
   offset of the page-aligned command block; addr is the FRED register the
   result is written back to (via Pi1MHz_MemoryWrite); data is the raw
   command-register value (the FAT service uses its low nibble as the
   file-handle index). */
typedef void (*service_command_fn)(uint32_t command_pointer, uint32_t addr,
                                   uint8_t data);

/* Claim [first, last] (inclusive).  Returns false when the range overlaps
   one already claimed or the table is full.  Commands nobody claims are
   ignored, exactly as unknown command numbers always were. */
bool services_register(uint8_t first, uint8_t last, service_command_fn handler);

void services_emulator_init(uint8_t instance, uint8_t address);

/* The FAT/SD service (commands 0-20 today; the range reserves up to 29). */
void fat_service_init(void);

#endif
