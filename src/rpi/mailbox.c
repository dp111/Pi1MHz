#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rpi.h"
#include "mailbox.h"
#include "cache.h"

/* Make sure the property tag buffer is aligned to a 16-byte boundary because
   we only have 28-bits available in the property interface protocol to pass
   the address of the buffer to the VC. */
__attribute__((aligned(64))) NOINIT_SECTION static uint32_t pt[PROP_BUFFER_SIZE];

static size_t pt_index;

#define PRINT_PROP_DEBUG 0

    /* For information about accessing mailboxes, see:
       https://github.com/raspberrypi/firmware/wiki/Accessing-mailboxes */

/* Map mail boxes to base addresses */
static mailbox_t* rpiMailbox0 = (mailbox_t*)RPI_MAILBOX0_BASE;
static mailbox_t* rpiMailbox1 = (mailbox_t*)RPI_MAILBOX1_BASE;

static void RPI_Mailbox0Write( mailbox0_channel_t channel, uint32_t * ptr )
{
    _clean_cache_area(ptr, ptr[PT_OSIZE]);
    /* Wait until the mailbox becomes available and then write to the mailbox
       channel */
    while ( ( rpiMailbox1->Status & ARM_MS_FULL ) != 0 ) { }
    /* Add the channel number into the lower 4 bits */
    /* Write the modified value + channel number into the write register */
   rpiMailbox1->Data = ((uint32_t)ptr ) | channel;
}

static uint32_t RPI_Mailbox0Read( mailbox0_channel_t channel )
{
    uint32_t value ;

    /* Keep reading the register until the desired channel gives us a value */

    do {
        /* Wait while the mailbox is not empty because otherwise there's no value
           to read! */

        while ( rpiMailbox0->Status & ARM_MS_EMPTY ) {}
        /* Extract the value from the Read register of the mailbox. The value
           is actually in the upper 28 bits */
        value = rpiMailbox0->Data;
    } while ( ( value & 0xF ) != channel );
    /* Return just the value (the upper 28-bits) */
    return value >> 4;
}

rpi_mailbox_property_t* RPI_PropertyGetWord(rpi_mailbox_tag_t tag, uint32_t data)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    pt[pt_index++] = 8;
    pt[pt_index++] = 0; /* Request */
    pt[pt_index++] = data;
    pt_index += 1;
    pt[pt_index] = 0;
    RPI_PropertyProcess();
    return RPI_PropertyGet(tag);
}

void RPI_PropertySetWord(rpi_mailbox_tag_t tag, uint32_t id, uint32_t data)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    pt[pt_index++] = 8;
    pt[pt_index++] = 0; /* Request */
    pt[pt_index++] = id;
    pt[pt_index++] = data;
    pt[pt_index] = 0;
    RPI_PropertyProcess();
}

rpi_mailbox_property_t* RPI_PropertyGetBuffer(rpi_mailbox_tag_t tag)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    /* Provide a 1024-byte buffer */
    pt[pt_index++] = PROP_SIZE;
    pt[pt_index++] = 0; /* Request */
    pt_index += PROP_SIZE >> 2;
    pt[pt_index] = 0;
    RPI_PropertyProcess();
    return RPI_PropertyGet(tag);
}

void RPI_PropertySetBuffer(rpi_mailbox_tag_t tag, uint32_t *buffer, uint32_t length)
{
    pt_index = 2;
    pt[pt_index++] = tag;
    pt[pt_index++] = 8 + length * 4;
    pt[pt_index++] = 0; /* Request */
    for (uint32_t i = 0; i < length; i++)
        pt[pt_index++] = buffer[i];

    pt[pt_index] = 0;
    RPI_PropertyProcess();
}

int RPI_PropertyProcess( void )
{
    int result;

#if( PRINT_PROP_DEBUG == 1 )
    LOG_INFO( "%s Length: %d\r\n", __func__, pt[PT_OSIZE] );
#endif
    /* Fill in the size of the buffer */
    pt[PT_OSIZE] = ( pt_index + 1 ) << 2;
    pt[PT_OREQUEST_OR_RESPONSE] = 0;

#if( PRINT_PROP_DEBUG == 1 )
    for ( int i = 0; i < (pt[PT_OSIZE] >> 2); i++ )
        LOG_INFO( "Request: %3d %8.8X\r\n", i, pt[i] );
#endif
    RPI_Mailbox0Write( MB0_TAGS_ARM_TO_VC, pt );

    result = RPI_Mailbox0Read( MB0_TAGS_ARM_TO_VC );

#if( PRINT_PROP_DEBUG == 1 )
    for ( int i = 0; i < (pt[PT_OSIZE] >> 2); i++ )
        LOG_INFO( "Response: %3d %8.8X\r\n", i, pt[i] );
#endif
    return result;
}

rpi_mailbox_property_t* RPI_PropertyGet( rpi_mailbox_tag_t tag)
{
    NOINIT_SECTION static rpi_mailbox_property_t property;
    uint32_t * tag_buffer = NULL;

    property.tag = tag;

    /* Get the tag from the buffer. Start at the first tag position  */
    uint32_t index = 2;

    while ( index < ( pt[PT_OSIZE] >> 2 ) )
    {
        /* LOG_DEBUG( "Test Tag: [%d] %8.8X\r\n", index, pt[index] ); */
        if ( pt[index] == tag )
        {
           tag_buffer = &pt[index];
           break;
        }

        /* Progress to the next tag if we haven't yet discovered the tag */
        index += ( pt[index + 1] >> 2 ) + 3;
    }

    /* Return NULL of the property tag cannot be found in the buffer */
    if ( tag_buffer == NULL )
        return NULL;

    /* Return the required data */
    property.byte_length = tag_buffer[T_ORESPONSE] & 0xFFFF;
    memcpy( property.data.buffer_8, &tag_buffer[T_OVALUE], property.byte_length );

    return &property;
}
