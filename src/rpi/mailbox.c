#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "rpi.h"
#include "mailbox.h"
#include "cache.h"
#include "asm-helpers.h"
#include "systimer.h"   /* bounded waits on the VideoCore - see RPI_Mailbox0Read */

/* The property interface protocol needs at least 16-byte alignment (only
   28 bits carry the buffer address), but the buffer is aligned to a full
   cache line (64) so the invalidate after the VC's response can never
   discard adjacent data sharing a line. */
__attribute__((aligned(64))) NOINIT_SECTION static uint32_t pt[PROP_BUFFER_SIZE];

static size_t pt_index;

#define PRINT_PROP_DEBUG 0

    /* For information about accessing mailboxes, see:
       https://github.com/raspberrypi/firmware/wiki/Accessing-mailboxes */

/* Map mail boxes to base addresses */
static mailbox_t* rpiMailbox0 = (mailbox_t*)RPI_MAILBOX0_BASE;
static mailbox_t* rpiMailbox1 = (mailbox_t*)RPI_MAILBOX1_BASE;

/* Every wait on the VideoCore is bounded, so a VC that stops answering can no
   longer hang the ARM forever inside init_hardware() - before UART gets past
   the first few lines, and long before USB or WiFi, with no recovery but
   pulling the power.  Chain-booting a kernel over MTP is one way to provoke
   it: the mailbox is left mid-conversation and the pending reply belongs to
   the previous kernel's buffer, at a different address because it was a
   different build.

   Three seconds, not the 100 ms this was first written with.  100 ms broke
   cold boots: the first property call of a cold boot happens while the VC is
   still finishing its own initialisation and evidently takes longer than
   that, so the ARM_MEMORY query behind arm_setup_heap_limit() timed out, the
   heap fell back to its 4 MB early limit, and USB and lwIP both failed to
   allocate while the main loop carried on. A chain-boot inherits a VC that is
   long since ready, which is why every chain-boot test passed.

   The bound exists only to break an infinite hang, so it should sit far above
   any legitimate latency rather than close to it. */
#define MAILBOX_TIMEOUT_US 3000000u

/* Returned by RPI_Mailbox0Read when the VC did not answer in time.  A real
   response is a 28-bit buffer address, so this cannot collide with one. */
#define MAILBOX_READ_TIMEOUT 0xFFFFFFFFu

static void RPI_Mailbox0Write( mailbox0_channel_t channel, const uint32_t * ptr )
{
    uint32_t start_us = RPI_GetSystemTime();

    _clean_cache_area(ptr, ptr[PT_OSIZE]);
// cppcheck-suppress constStatement
    rpiMailbox0->Data; // empty buffer in case anything is left over.
    /* Wait until the mailbox becomes available and then write to the mailbox
       channel */
// cppcheck-suppress constStatement
    while ( ( rpiMailbox1->Status & ARM_MS_FULL ) != 0 ) {
        rpiMailbox0->Data;
        if ( ( RPI_GetSystemTime() - start_us ) > MAILBOX_TIMEOUT_US )
            break;      /* a FULL bit that never clears will not clear by
                           waiting longer; post anyway and let the read time
                           out if the VC really has stopped listening */
    }
    /* Add the channel number into the lower 4 bits */
    /* Write the modified value + channel number into the write register */
   rpiMailbox1->Data = ((uint32_t)ptr ) | channel;
}

static uint32_t RPI_Mailbox0Read( mailbox0_channel_t channel )
{
    uint32_t value ;
    uint32_t start_us = RPI_GetSystemTime();

    /* Keep reading the register until the desired channel gives us a value */

    do {
        /* Wait while the mailbox is not empty because otherwise there's no value
           to read! */

        while ( rpiMailbox0->Status & ARM_MS_EMPTY ) {
            if ( ( RPI_GetSystemTime() - start_us ) > MAILBOX_TIMEOUT_US )
                return MAILBOX_READ_TIMEOUT;
        }
        /* Extract the value from the Read register of the mailbox. The value
           is actually in the upper 28 bits */
        value = rpiMailbox0->Data;
    } while ( ( value & 0xF ) != channel
              && ( RPI_GetSystemTime() - start_us ) <= MAILBOX_TIMEOUT_US );

    if ( ( value & 0xF ) != channel )
        return MAILBOX_READ_TIMEOUT;
    /* Return just the value (the upper 28-bits) */
    return value >> 4;
}

/* Throw away anything still queued, so a call that gave up does not leave a
   late reply to be mistaken for the next call's answer.  Without this one
   timeout would put every later transaction permanently one message behind. */
/* Discard anything the VideoCore left in the mailbox before the first
   property call of this kernel.  A kernel.now chain-boot inherits whatever
   the previous kernel had in flight - the mailbox is VideoCore state and does
   not reset on a warm jump - and a stale response puts every later exchange
   one message behind: each then reads the previous call's reply, mismatches,
   and pays the full 3 s timeout before recovering.  Observed as a debug boot
   stopping dead part-way through dump_useful_info() and sitting there for
   17 s (five timeouts) until the watchdog reset it. */
static void RPI_Mailbox0Drain( void );

void RPI_MailboxInit( void )
{
   RPI_Mailbox0Drain();
}

static void RPI_Mailbox0Drain( void )
{
    uint32_t start_us = RPI_GetSystemTime();

    while ( ( rpiMailbox0->Status & ARM_MS_EMPTY ) == 0 ) {
// cppcheck-suppress constStatement
        rpiMailbox0->Data;
        if ( ( RPI_GetSystemTime() - start_us ) > MAILBOX_TIMEOUT_US )
            break;
    }
}

rpi_mailbox_property_t* RPI_PropertyGetWord(rpi_mailbox_tag_t tag, uint32_t data)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    pt[pt_index++] = 8;
    pt[pt_index++] = 0; /* Request */
    pt[pt_index++] = data;
    pt_index += 1;
    unsigned int irq = _disable_interrupts_cspr();
    RPI_PropertyProcess(true);
    rpi_mailbox_property_t* result = RPI_PropertyGet(tag);
    _restore_cpsr(irq);
    return result;
}

void RPI_PropertySetWord(rpi_mailbox_tag_t tag, uint32_t id, uint32_t data)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    pt[pt_index++] = 8;
    pt[pt_index++] = 0; /* Request */
    pt[pt_index++] = id;
    pt[pt_index++] = data;
    RPI_PropertyProcess(false);
}

rpi_mailbox_property_t* RPI_PropertyGetBuffer(rpi_mailbox_tag_t tag)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    /* Provide a 1024-byte buffer */
    pt[pt_index++] = PROP_SIZE;
    pt[pt_index++] = 0; /* Request */
    pt_index += PROP_SIZE >> 2;
    unsigned int irq = _disable_interrupts_cspr();
    RPI_PropertyProcess(true);
    rpi_mailbox_property_t* result = RPI_PropertyGet(tag);
    _restore_cpsr(irq);
    return result;
}

void RPI_PropertyStart(rpi_mailbox_tag_t tag, uint32_t length)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    pt[pt_index++] = length * 4;
    pt[pt_index++] = 0; /* Request */
}

void RPI_PropertyAdd(uint32_t data)
{
    pt[pt_index++] = data;
}

void RPI_PropertyAddTwoWords(uint32_t data, uint32_t data2)
{
    pt[pt_index++] = data;
    pt[pt_index++] = data2;
}

void RPI_PropertyNewTag(rpi_mailbox_tag_t tag, uint32_t length)
{
    pt[pt_index++] = tag;
    pt[pt_index++] = length * 4;
    pt[pt_index++] = 0; /* Request */
}

unsigned int RPI_PropertyProcess( bool wait )
{
    unsigned int result;

    /* Fill in the size of the buffer */
    pt[PT_OSIZE] = ( pt_index + 1 ) << 2;
    pt[PT_OREQUEST_OR_RESPONSE] = 0;
    /* Make sure the tags are 0 terminated to end the list and update the buffer size */
    pt[pt_index] = 0;

#if( PRINT_PROP_DEBUG == 1 )
    LOG_INFO( "%s Length: %"PRIx32"\r\n", __func__, pt[PT_OSIZE] );
    for ( int i = 0; i < (pt[PT_OSIZE] >> 2); i++ )
        LOG_INFO( "Request: %3d %8.8"PRIx32"\r\n", i, pt[i] );
#endif
    RPI_Mailbox0Write( MB0_TAGS_ARM_TO_VC, pt );

    //if (wait == false)
    //    return 0;

    { // make sure the response is for us
       uint32_t start_us = RPI_GetSystemTime();

       bool accepted = false;
       do {
          result = RPI_Mailbox0Read( MB0_TAGS_ARM_TO_VC );
          if ( result == MAILBOX_READ_TIMEOUT ) {
             RPI_Mailbox0Drain();
             pt[PT_OREQUEST_OR_RESPONSE] = 0u;   /* not a response */
             return 0;
          }
          if ((uint32_t) result == ((uint32_t) pt) >> 4) {
             /* A matching token is necessary but not sufficient: there is
                only one property buffer, so a LATE reply from a previous
                timed-out exchange carries the same address.  Accept only
                once the VC has written the response bit for THIS exchange -
                a clear bit is a stale token, keep reading. (This is the
                mechanism behind the 0.0C temperature after a BBC reset.) */
             _invalidate_cache_area(pt, 8);
             if (pt[PT_OREQUEST_OR_RESPONSE] & 0x80000000u) {
                accepted = true;
                break;
             }
          }
       } while (( RPI_GetSystemTime() - start_us ) <= MAILBOX_TIMEOUT_US);

       if (!accepted) {
          RPI_Mailbox0Drain();
          pt[PT_OREQUEST_OR_RESPONSE] = 0u;      /* not a response */
          return 0;
       }
    }

    // pt[] is in ordinary cacheable RAM: the prefetcher can refill its lines
    // while we spin waiting for the VC response, so always discard them
    _invalidate_cache_area(pt, pt[0]);

#if( PRINT_PROP_DEBUG == 1 )
    for ( int i = 0; i < (pt[PT_OSIZE] >> 2); i++ )
        LOG_INFO( "Response: %3d %8.8"PRIx32"\r\n", i, pt[i] );
#endif
    return result;
}

rpi_mailbox_property_t* RPI_PropertyGet( rpi_mailbox_tag_t tag)
{
    /* Get the tag from the buffer. Start at the first tag position  */
    uint32_t index = 2;

    /* No response bit means the VideoCore never wrote into the buffer, so the
       tags still sitting there are our own request.  Returning one of those
       looks to the caller exactly like a successful read - the tag matches -
       but the value field of a GetWord request is never written, and pt is in
       .noinit, so what comes back is whatever was in that RAM.  Callers do
       check for NULL; give them the NULL they are checking for. */
    if ( ( pt[PT_OREQUEST_OR_RESPONSE] & 0x80000000u ) == 0u )
        return NULL;

    while ( index < ( pt[PT_OSIZE] >> 2 ) )
    {
        if ( pt[index] == tag )
        {
            /* Return the required data */
            (&pt[index])[T_ORESPONSE] = (&pt[index])[T_ORESPONSE] & 0xFFFF;
            return (rpi_mailbox_property_t*) &pt[index];
        }

        /* Progress to the next tag if we haven't yet discovered the tag */
        index += ( pt[index + 1] >> 2 ) + 3;
    }

    /* Return NULL of the property tag cannot be found in the buffer */
    return NULL;
}
