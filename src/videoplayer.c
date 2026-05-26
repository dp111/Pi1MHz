/*

    Take video from SDCARD and displays it on the screen

*/

#include <stdint.h>
#include "rpi/screen.h"
#include "BeebSCSI/filesystem.h"

#define YUV_PLANE 0

void videoplayer_init(uint8_t instance, uint8_t address)
{
    static uint32_t handle;
#if 1
    screen_plane_enable(YUV_PLANE, false);
    screen_release_buffer(handle); // doesn't do anything if handle is NULL

    uint32_t buffer = screen_allocate_buffer( 768*576*2, &handle );
    filesystemReadFile("frame.yuv",(unsigned char *) buffer,768*576*2);
    screen_create_YUV_plane( YUV_PLANE, 768, 576, buffer );

    screen_plane_enable(YUV_PLANE, true);
   // while(1);
#endif
}