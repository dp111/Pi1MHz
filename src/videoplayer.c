/*

Takes video from SDCARD and displays it on the screen

File name format

frames0.dat : 0- 4095
frames1.dat : 4096-  8191
...

File format

32bit index into file from frame 0
32bit index into file from frame 1
..
lz4 frame data
lz4 frame data
*/

#include <stdint.h>
#include <stdlib.h>
#include "rpi/screen.h"
#include "BeebSCSI/filesystem.h"
#include "BeebSCSI/fatfs/ff.h"
#include "rpi/decompress.h"
#include <stdio.h>
#include "rpi/rpi.h"


#define YUV_PLANE 0

NOINIT_SECTION static uint32_t frameindex[65536];
NOINIT_SECTION static char frame_filename[256];

FIL fileObject;
int last_frame = -1;

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

// create frame index

bool videoplayer_index()
{
    for(int i=0; i<65536; i++)
        frameindex[i] = 0;

    for(int i=0; i<65536/4096; i++)
    {
        sprintf(frame_filename, "/BeebVFS%d/frames%d.dat", filesystemGetVFSLunDirectory(), i);
        if (filesystemfopen(frame_filename,  fileObject))
        {
            if (!filesystemfread(fileObject, (uint8_t *) &frameindex[4096*i], 4*4096))
                break;
        }
        filesystemfclose(fileObject);
    }
    return true;
}

bool videoplayer_getframe(uint32_t frame)
{
    if (frame > 65536)
        return false;

    if (last_frame == frame)
        return true;




    last_frame = frame;

    uint32_t offset = frameindex[frame];
    uint32_t size = frameindex[frame+1] - offset;
    uint8_t * buf = malloc(size);
    if (buf)
    {
        sprintf(frame_filename, "/BeebVFS%d/frames%d.dat", filesystemGetVFSLunDirectory(), offset/4096);
        if (filesystemfopen(frame_filename, fileObject))
        {
            filesystemfread(fileObject, buf, size);
            filesystemfclose(fileObject);
            decompress_lz4(buf, (uint8_t *) screen_get_buffer(YUV_PLANE));
        }
        free(buf);
    }
    return true;
}