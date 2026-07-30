#ifndef RPI_H
#define RPI_H

#ifndef __ASSEMBLER__
#include <stdio.h>

#ifdef DEBUG
#define LOG_DEBUG(...) printf(__VA_ARGS__)
#else
#define LOG_DEBUG(...)
#endif

#define LOG_INFO(...) printf(__VA_ARGS__)

#define LOG_WARN(...) printf(__VA_ARGS__)

/* Drain any stale VideoCore mailbox response inherited from a previous
   kernel; call once before the first property request.  See mailbox.c. */
void RPI_MailboxInit( void );

/* Boot progress marker.  Stamped at each milestone and reported on the NEXT
   boot, because the serial log cannot answer "where did it stop": printf
   buffers into a 64 KB ring drained only by the TX interrupt, so early boot
   reaches the wire long after it ran, the exception handler bypasses the ring
   entirely, and a reset discards whatever had not been sent.  The log shows
   where the WIRE got to, never where the CODE got to.  This marker lives in
   .noinit, which a watchdog reset does not clear. */
typedef enum {
   BOOT_STAGE_ENTRY = 1,      /* kernel_main reached                        */
   BOOT_STAGE_MAILBOX,        /* inherited mailbox drained                  */
   BOOT_STAGE_MMU,            /* MMU and caches enabled                     */
   BOOT_STAGE_HEAP,           /* heap sized                                 */
   BOOT_STAGE_INFO,           /* dump_useful_info done (DEBUG builds)       */
   BOOT_STAGE_CONFIG,         /* Pi1MHz.cfg parsed                          */
   BOOT_STAGE_EMULATORS,      /* every emulator init returned               */
   BOOT_STAGE_RUNNING         /* main poll loop entered                     */
} boot_stage_t;

void RPI_BootStage( boot_stage_t stage );
boot_stage_t RPI_BootStagePrevious( void );
#endif


/* Put large arrays in no init section saves bss time */

#define NOINIT_SECTION __attribute__ ((section (".noinit")))

#ifndef __ASSEMBLER__
typedef void (*func_ptr)(void);

#if (__ARM_ARCH >= 7 )
void start_core(int core, func_ptr func);
#endif

#endif

#endif
