/*

    Take video from SDCARD and displays it on the screen

*/

#include <stdint.h>
#include <stdlib.h>
#include "rpi/screen.h"
#include "BeebSCSI/filesystem.h"
#include "rpi/decompress.h"
#include "stdio.h"
#include "rpi/rpi.h"

#define YUV_PLANE 0

void videoplayer_init(uint8_t instance, uint8_t address)
{
    static uint32_t handle;
    static uint32_t buffer;
#if 1
    screen_plane_enable(YUV_PLANE, false);
    if (!handle)
    {
        screen_release_buffer(handle); // doesn't do anything if handle is NULL
        buffer = screen_allocate_buffer( 768*576*2, &handle );
        uint8_t * buf = malloc(768*576*2);
        if (buf)
        {
            LOG_DEBUG("videoplayer_init frame\r\n");
            if (filesystemReadFile("frame.lz",&buf,768*576*2))
                decompress_lz4(buf, ( uint8_t*) buffer);
        }

        // filesystemReadFile("frame.yuv",(unsigned char *) (buffer),768*576*2);
   }

    screen_create_YUV_plane( YUV_PLANE, 768, 576, buffer );

    screen_plane_enable(YUV_PLANE, true);
   // while(1);

   LOG_DEBUG("videoplayer_init done\r\n");
#endif
}