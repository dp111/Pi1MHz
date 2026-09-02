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
#define SERVICE_CMD_NET_FIRST   45u   /* IP sockets / N: device - net_service.c */
#define SERVICE_CMD_NET_LAST    79u   /* sockets 45-56, IRQ 57, N: dev 60-65   */
#define SERVICE_CMD_WIFI_FIRST 80u /* ElkWiFi compatibility service        */
/* Two commands are held for the UEF cluster's return: 86 (guard image) sits
   INSIDE the claimed range and is answered WIFI_SVC_ERR_UNSUPPORTED for now;
   93 (UEF stream) sits just past it, which is why the range stops at 92. */
#define SERVICE_CMD_WIFI_LAST  92u
#define SERVICE_CMD_SECURE_FIRST  94u /* RNG and managed SSH - secure_service.c */
#define SERVICE_CMD_SECURE_LAST  113u
/* 93 held (see above); 114..255 unallocated */

/* services_register() rejects an overlapping claim at run time, which is the
   backstop.  These catch the same mistake when the ranges above are edited -
   at compile time, in every build, rather than when a Beeb command silently
   reaches the wrong service. */
_Static_assert(SERVICE_CMD_FAT_FIRST     <= SERVICE_CMD_FAT_LAST,     "FAT range inverted");
_Static_assert(SERVICE_CMD_AUN_FIRST     <= SERVICE_CMD_AUN_LAST,     "AUN range inverted");
_Static_assert(SERVICE_CMD_NET_FIRST     <= SERVICE_CMD_NET_LAST,     "net range inverted");
_Static_assert(SERVICE_CMD_WIFI_FIRST <= SERVICE_CMD_WIFI_LAST, "ElkWiFi range inverted");
_Static_assert(SERVICE_CMD_SECURE_FIRST  <= SERVICE_CMD_SECURE_LAST,  "secure range inverted");
_Static_assert(SERVICE_CMD_FAT_LAST     < SERVICE_CMD_AUN_FIRST,     "FAT overlaps AUN");
_Static_assert(SERVICE_CMD_AUN_LAST     < SERVICE_CMD_NET_FIRST,     "AUN overlaps net");
_Static_assert(SERVICE_CMD_NET_LAST     < SERVICE_CMD_WIFI_FIRST, "net overlaps ElkWiFi");
_Static_assert(SERVICE_CMD_WIFI_LAST < SERVICE_CMD_SECURE_FIRST,  "ElkWiFi overlaps secure");

/* What a Beeb sees when a service is not there.  Three states, deliberately
   distinguishable, because a ROM has to tell them apart:

     - Range never claimed - the service is absent from the build, or tested
       its config key BEFORE services_register() and returned.  The dispatcher
       echoes the command byte back untouched, which a ROM reads as "this Pi
       has no such service".  The ElkWiFi ROM depends on exactly that.
     - Range claimed, service disabled at run time - it registered and then
       refuses, returning its own error code (net_service answers
       NET_ERR_DISABLED).  A Beeb can then distinguish "present but switched
       off" from "not present at all".
     - Range claimed and live - the handler's own result byte.

   So where a service tests its config key is an ABI decision, not a style
   one: before the claim to disappear, after it to refuse.  Disappearing also
   costs no poll slot, which is why a service with per-boot setup work (a
   profile read, a socket table) prefers it. */

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

/* Services-port IRQ support.  The port owns its IRQ status register at
   base+5 (so it tracks a relocated Services_addr instead of a hard-coded
   address) and the shared nIRQ line.  A service publishes a status byte and
   raises/clears its nIRQ contribution in one call, keyed by the nIRQ source
   id it was handed at init (the emulator-table `instance`).  status 0
   publishes 0 and clears the source.

   The single status byte currently carries AUN's AUNFS-ROM layout; a second
   service that needs nIRQ but must not disturb that byte should read its own
   state through its command (the FujiNet PROCEED-then-STATUS model) and this
   API can grow a line-only variant when that lands. */
void services_irq(uint8_t source, uint8_t status);

/* Line-only nIRQ: raise/lower this service's contribution to the shared
   nIRQ line WITHOUT touching the base+5 status byte (that byte is AUN's
   AUNFS-ROM ABI).  A service that signals events this way exposes the detail
   through its own status command (the FujiNet PROCEED-then-STATUS model)
   rather than a shared status register.  This is the path new services (the
   IP/net service) use; AUN keeps services_irq(). */
void services_irq_set(uint8_t source, bool asserted);

void services_emulator_init(uint8_t instance, uint8_t address);

/* The FAT/SD service (commands 0-20 today; the range reserves up to 29). */
void fat_service_init(void);

/* True while the Beeb holds host_path open through the FAT service, or
   host_path is a directory containing such a file.  Advisory (fails open
   for unrecordable paths) - the webserver's counterpart to
   filesystemHostPathBusy() for the SCSI LUN images. */
bool fat_service_file_in_use(const char *host_path);

#endif
