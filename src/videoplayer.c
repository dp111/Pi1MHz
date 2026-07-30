/*

    Take video from SDCARD and displays it on the screen

*/

#include <stdint.h>
#include <stdlib.h>
#include "rpi/screen.h"
#include "BeebSCSI/filesystem.h"
#include "rpi/decompress.h"
#include <stdio.h>
#include "rpi/rpi.h"
#include <string.h>

#define YUV_PLANE 0

/* Where the YUV buffer handle is parked so the NEXT kernel can hand it back.
 *
 * The VideoCore keeps its allocations across an ARM warm restart, so a
 * kernel.now chain-boot inherits the previous kernel's buffer - while `handle`
 * below is a static that starts at zero, so nothing ever released it.  The
 * ~1.2 MB leaked per chain-boot was measured on the wire: the address returned
 * by screen_allocate_buffer() walked down 1F7F8000, 1F6CE000, 1F5A4000,
 * 1F47A000, 1F34F000, 1F225000, 1F0FB000 and the eighth chain-boot got 0.
 *
 * A fixed address, deliberately not .noinit and not a plain static: the kernel
 * that allocates and the kernel that releases are DIFFERENT builds, and
 * .noinit moves the moment .text changes size (the same trap as PageTable[] in
 * the chain-boot handover).  0x7C20 is in the spare KB of low RAM below the
 * 0x8000 load address that mailbox.c already uses for the boot-stage marker,
 * clear of the vectors at 0 and the ATAGS at 0x100.
 *
 * This survives exactly where it needs to: an ARM-only kernel.now chain-boot,
 * which is the case that leaks.  A reboot that goes through the VideoCore
 * resets the GPU and frees everything anyway, and the magic stops uninitialised
 * RAM from being mistaken for a handle.
 */
#define VIDEOBUF_PERSIST_BASE 0x00007C20u
#define videobuf_magic   (((volatile uint32_t *)VIDEOBUF_PERSIST_BASE)[0])
#define videobuf_handle  (((volatile uint32_t *)VIDEOBUF_PERSIST_BASE)[1])
#define VIDEOBUF_MAGIC   0x56425546u   /* 'VBUF' */

void videoplayer_init(uint8_t instance, uint8_t address)
{
    static uint32_t handle;
    static uint32_t buffer;

    screen_plane_enable(YUV_PLANE, false);
    if (!handle)
    {
        /* Give back whatever the previous kernel left allocated, before asking
           for our own - otherwise each chain-boot loses another 1.2 MB of GPU
           memory and the pool runs dry.  screen_release_buffer() does not check
           its argument, so do not call it with a handle we do not have. */
        if (videobuf_magic == VIDEOBUF_MAGIC && videobuf_handle != 0u)
        {
            screen_release_buffer(videobuf_handle);
            videobuf_handle = 0u;
        }

        buffer = screen_allocate_buffer( 768*576*2, &handle );

        if (!buffer)
        {
            /* CHECK THIS.  The old code did not, and then memset 442 KB
               starting at address 0 - over the exception vectors and straight
               through 0x8000, where the running kernel itself lives.  The
               machine erased its own code and hung until the watchdog reset
               it, about 27 s round trip, and it happened on every eighth
               chain-boot once the leak above had drained the GPU pool.

               A missing video plane is a far better outcome than a destroyed
               kernel, so give up here with the plane left disabled. */
            if (handle)
            {
                screen_release_buffer(handle);
                handle = 0;
            }
            LOG_INFO("videoplayer_init: no GPU buffer; video plane disabled\r\n");
            return;
        }

        videobuf_magic  = VIDEOBUF_MAGIC;
        videobuf_handle = handle;

        uint8_t * buf = malloc(768*576*2);
        if (buf)
        {
            LOG_DEBUG("videoplayer_init frame\r\n");
            if (filesystemReadFile("frame.lz",&buf,768*576*2))
                decompress_lz4(buf, ( uint8_t*) buffer);
            else
            {
                // Create a black planar YCbCr frame (Y=0, Cb=128, Cr=128)
                uint8_t *dst = (uint8_t *)(uintptr_t)buffer;
                memset(dst, 0, 768*576);                  // Y plane
                memset(dst + 768*576, 0x80, 768*576);     // Cr + Cb planes
            }

            free(buf);
        }
        else
        {
            LOG_INFO("videoplayer_init: ERROR: Unable to allocate memory for video frame\r\n");
        }

        // filesystemReadFile("frame.yuv",(unsigned char *) (buffer),768*576*2);
   }

    screen_create_YUV_plane( YUV_PLANE, 768, 576, buffer );

    screen_plane_enable(YUV_PLANE, true);

   LOG_DEBUG("videoplayer_init done\r\n");
}
