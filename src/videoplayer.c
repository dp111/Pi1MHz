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

NOINIT_SECTION static uint32_t frameindex[54001];
NOINIT_SECTION static char frame_filename[256];
NOINIT_SECTION static uint8_t frame_load_buffer[1024*1024];

FIL fileObject;
static uint32_t last_frame;
static uint32_t fbuffer;

// load frame index

bool videoplayer_index()
{
    for(size_t i=0; i<sizeof(frameindex)/sizeof(frameindex[0]); i++)
        frameindex[i] = 0;

    last_frame = 65536; // force a reload
    f_close(&fileObject);
    sprintf(frame_filename, "/BeebVFS%d/index.dat", filesystemGetVFSLunDirectory());
    if (f_open( &fileObject, frame_filename, FA_READ) != FR_OK)
    {
        LOG_DEBUG("videoplayer_index: ERROR: Could not open video index file\r\n");
        return false;
    }

    UINT length;
    if (f_read(&fileObject, (uint8_t *) frameindex, 54000*4,&length) !=FR_OK)
    {
        LOG_DEBUG("videoplayer_index: ERROR: Could not read video index\r\n");
        f_close(&fileObject);
        return false;
    }

    f_close(&fileObject);
    LOG_DEBUG("videoplayer_index: Index Loaded\r\n");
    return true;
}

static bool videoplayer_getframe(uint32_t frame, uint8_t * fb)
{
    if (frame > 54000)
        return false;

    if (last_frame == frame)
        return true;

    LOG_DEBUG("videoplayer_getframe: frame %lu last frame %lu\r\n", frame, last_frame);
    if ( (frame>>12) != (last_frame>>12) )
    {
        f_close(&fileObject);
        // read the next frame
        sprintf(frame_filename, "/BeebVFS%d/frames%d.dat",(int) filesystemGetVFSLunDirectory(),(int) frame/4096);
        LOG_DEBUG("videoplayer_getframe: open %s\r\n", frame_filename);
        if  (f_open(&fileObject, frame_filename, FA_READ) != FR_OK)
        {
            LOG_DEBUG("videoplayer_getframe: ERROR: Could not open video file frame%d.dat\r\n",(int)frame/4096);
            return false;
        }
    }

    last_frame = frame;

    uint32_t offset = frameindex[frame];
    if (offset == 0)
    {
        LOG_DEBUG("videoplayer_getframe: ERROR: frame %lu not found\r\n", frame);
        return false;
    }
    uint32_t size;
    if (frameindex[frame+1] == 0)
       size = 1024*1024; // read to the end of the file
    else
       size = frameindex[frame+1] - offset;
    LOG_DEBUG("videoplayer_getframe: offset %lu size %lu\r\n", offset, size);
    UINT length;
    FRESULT res;
    //LOG_DEBUG(" %p, %p, %d, %p, %p\r\n", &fileObject.obj, fileObject.obj.fs, fileObject.obj.fs->fs_type, fileObject.obj.id, fileObject.obj.fs->id);

    res = f_lseek(&fileObject, offset);

    if (res!=FR_OK)
    {
        LOG_DEBUG("videoplayer_getframe: ERROR: Could not seek to frame %lu %d\r\n", frame, res);
        return false;
    }
    if (f_read(&fileObject, frame_load_buffer, size, &length) != FR_OK)
    {
        LOG_DEBUG("videoplayer_getframe: ERROR: Could not read frame %lu\r\n", frame);
        return false;
    }
    decompress_lz4(frame_load_buffer,fb);

    return true;
}

void videoplayer_init(uint8_t instance, uint8_t address)
{
    static uint32_t handle;
    videoplayer_index();
    screen_plane_enable(YUV_PLANE, false);
    if (!handle)
    {

        screen_release_buffer(handle); // doesn't do anything if handle is NULL
        fbuffer = screen_allocate_buffer( 768*576*2, &handle );
        LOG_DEBUG("videoplayer_init\r\n");
#if 1
        videoplayer_getframe(3000,(uint8_t *) fbuffer);
        screen_create_YUV_plane( YUV_PLANE, 768, 576, fbuffer );
        screen_plane_enable(YUV_PLANE, true);
#endif
   }

   LOG_DEBUG("videoplayer_init done\r\n");

}

bool videoplayer_frame(uint32_t frame)
{
    if (!videoplayer_getframe(frame, (uint8_t *) fbuffer))
        return false;
    //screen_create_YUV_plane( YUV_PLANE, 768, 576, fbuffer );
    //screen_plane_enable(YUV_PLANE, true);
    return true;
}