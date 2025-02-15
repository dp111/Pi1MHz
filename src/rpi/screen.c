#include <stdint.h>
#include "base.h"
#include "mailbox.h"
#include "rpi.h"
#include <stdio.h>
#include <inttypes.h>
#include "../rpi/asm-helpers.h"
#include "../rpi/interrupts.h"

/*
    Interfaces to the HVS in the BCM2835

    Default usage :

    Plane 0 - YUV 768 x 576
    Plane 1 - RGB 320 x 240 256 colour
    Plane 2 - RGB 16x16  mouse pointer 2 colour
    Plane 3 - RGB 320x16  status 2 colour
*/

/* context memory layout 16Kbytes ( 0x4000 bytes)

    planes are 128bytes each 0x80
    assume 8 planes 0x400

    0xf00 polyphase filter coefficient

    f00, 7ebfc00
    f04, 7e3edf8
    f08, 4805fd
    f0c, 1dca432
    f10, 355769b
    f14, 1c6e3
    f18, 355769b
    f1c, 1dca432
    f20, 4805fd
    f24, 7e3edf8
    f28, 7ebfc00

    0x1000 palettes 256*4 = 0x400 each
    0x3000 4K spare ( used for other things)

LBM memory fixed at 768  bytes per line of each plane 8 planes
    RGB plane = 768*16

YUV plane = 768*8 + 768/2* 8
*/

// #define SCREEN_DEBUG

#define MAX_PLANES 4
#define PLOYPHASE_BASE (0xf00>>2)
#define PALETTE_BASE (0x1000)

// 48K words of plane memory
#define LBM_PLANE_SIZE (48*1024/MAX_PLANES)

typedef struct {
    rpi_reg_rw_t ctrl;      // 0x00
    rpi_reg_rw_t stat;      // 0x04
    rpi_reg_ro_t id;        // cppcheck-suppress unusedStructMember // 0x08
    rpi_reg_rw_t ectrl;     // cppcheck-suppress unusedStructMember // 0x0c
    rpi_reg_rw_t prof;      // cppcheck-suppress unusedStructMember // 0x10
    rpi_reg_rw_t dither;    // cppcheck-suppress unusedStructMember // 0x14
    rpi_reg_rw_t eoln;      // cppcheck-suppress unusedStructMember // 0x18
    rpi_reg_ro_t unused0;   // cppcheck-suppress unusedStructMember // 0x1c
    rpi_reg_rw_t list0;     // cppcheck-suppress unusedStructMember // 0x20
    rpi_reg_rw_t list1;     // 0x24   // default list used
    rpi_reg_rw_t list2;     // cppcheck-suppress unusedStructMember // 0x28
    rpi_reg_ro_t lstat;     // cppcheck-suppress unusedStructMember // 0x2c
    rpi_reg_rw_t lact0;     // cppcheck-suppress unusedStructMember // 0x30
    rpi_reg_rw_t lact1;     // cppcheck-suppress unusedStructMember // 0x34
    rpi_reg_rw_t lact2;     // cppcheck-suppress unusedStructMember // 0x38
    rpi_reg_ro_t unused1;   // cppcheck-suppress unusedStructMember // 0x3c
    rpi_reg_rw_t ctrl0;     // cppcheck-suppress unusedStructMember // 0x40
    rpi_reg_rw_t bkgnd0;    // cppcheck-suppress unusedStructMember // 0x44
    rpi_reg_ro_t stat0;     // cppcheck-suppress unusedStructMember // 0x48
    rpi_reg_rw_t base0;     // cppcheck-suppress unusedStructMember // 0x4c
    rpi_reg_rw_t ctrl1;     // 0x50
    rpi_reg_rw_t bkgnd1;    // cppcheck-suppress unusedStructMember // 0x54
    rpi_reg_ro_t stat1;     // cppcheck-suppress unusedStructMember // 0x58
    rpi_reg_rw_t base1;     // cppcheck-suppress unusedStructMember // 0x5c
    rpi_reg_rw_t ctrl2;     // cppcheck-suppress unusedStructMember // 0x60
    rpi_reg_rw_t bkgnd2;    // cppcheck-suppress unusedStructMember // 0x64
    rpi_reg_ro_t stat2;     // cppcheck-suppress unusedStructMember // 0x68
    rpi_reg_rw_t base2;     // cppcheck-suppress unusedStructMember // 0x6c
    rpi_reg_rw_t alpha2;    // cppcheck-suppress unusedStructMember // 0x70
    rpi_reg_ro_t unused2;   // cppcheck-suppress unusedStructMember // 0x74
    rpi_reg_rw_t gamaddr;   // cppcheck-suppress unusedStructMember // 0x78

} hvs_t;

typedef struct {
    rpi_reg_ro_t core_rev;  // cppcheck-suppress unusedStructMember   // 0x00
    rpi_reg_rw_t reset;     // cppcheck-suppress unusedStructMember   // 0x04
    rpi_reg_rw_t hotplug_int;  // cppcheck-suppress unusedStructMember   // 0x08
    rpi_reg_rw_t hotplug;   // 0x0c bit 0 hot plug state

} hdmi_t;

static hvs_t* const RPI_hvs = (hvs_t*) (PERIPHERAL_BASE + 0x400000);
static uint32_t* context_memory = (uint32_t*) (PERIPHERAL_BASE+ 0x402000);

static hdmi_t* const RPI_hdmi = (hdmi_t*) (PERIPHERAL_BASE + 0x902000);


//YUV plane ( YV12 format)
typedef struct {
    // bit 31 = 1 end of list
    // bit 30 = 1 valid list
    // bit 29..24 = 0x20 list element size in words
    // bit 22 rgb_transparency 1 = detect RGB for a transparency colour
    // bit 12:11 = 0x3 repeat  RGBA expansion
    // bit 10:8 = scl1_mode =0
    // bit 7:5 = scl0_mode =0 Hori interpolation vertical interpolation
    // bit 3:0 = pixel format 0xd = 8 bit RGB palette
    //                        0x2 = 16bit RGB 565
    //                        0x7 = 32bit RGBA
    //                        0x10 = YUV 4:2:2 separate Y U V planes

    rpi_reg_rw_t ctrl;      // 0x00
    // 31-24 = alpha 0xff bypasses alpha blending
    // 23-12 = stat_y  start scan line
    // 11- 0 = stat_x  start pixel
    rpi_reg_rw_t pos;       // 0x04
    // 27-16 = height of scaled image
    // 11-0 = width of scaled image
    rpi_reg_rw_t scale;     // 0x08
    // 31-28 = alpha modes etc
    // 27-16 = source height
    // 11- 0 = source width
    rpi_reg_rw_t src_size;     // 0x0c
    rpi_reg_ro_t src_context;  // 0x10

    rpi_reg_rw_t y_ptr;   // 0x14
    rpi_reg_rw_t cb_ptr;  // 0x18
    rpi_reg_rw_t cr_ptr;  // 0x1C
    rpi_reg_ro_t y_ctx;   // 0x20
    rpi_reg_ro_t cb_ctx;  // 0x24
    rpi_reg_ro_t cr_ctx;  // 0x28

    // 31-26 = pixels to drop at the beginning of each line
    // 25 = 0 Alpha [7:0] 1 Alpha [31:24]
    // 15:0 = pitch to next line
    rpi_reg_rw_t pitch;  // 0x2C

    // only in YUV mode
    // 31-25 alpha stuff
    // 15:0 = pitch to next line for CB
    rpi_reg_rw_t pitch1;  // 0x30
    //15:0 = pitch to next line for CR
    rpi_reg_rw_t pitch2;  // 0x34

    // colour space conversion
    // 601-5
    // 0x00F00000
    // 0xe73304A8 Cb_grn cr_grn yy cr_blu
    // 0x00066604 199 204     0 Cr_red Cb_blu
    rpi_reg_rw_t csc0;  // 0x38
    rpi_reg_rw_t csc1;  // 0x3C
    rpi_reg_rw_t csc2;  // 0x40
    // 31 enable LBM luma base address
    // 26..16 base luma for vertical scaling
    // 15..5 LBM base address for vertical scaling
    // 64 byte aligned so 4..0 = 0
    rpi_reg_rw_t LBM;  // 0x44

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = horizontal scaling factor
    //       =  (((1<<16)*src_width)/scl_width)
    // 6..0 = initial phase
    rpi_reg_rw_t hpf0;   // 0x48

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = vertical scaling factor
    //       =  (((1<<16)*src_height)/scl_height)
    // 6..0 = initial phase
    rpi_reg_rw_t vpf0;   // 0x4C
    rpi_reg_ro_t vpf0_ctx;   // 0x50

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = horizontal scaling factor
    //       =  (((1<<16)*src_width)/scl_width)
    // 6..0 = initial phase
    rpi_reg_rw_t hpf1;   // 0x54

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = vertical scaling factor
    //       =  (((1<<16)*src_height)/scl_height)
    // 6..0 = initial phase
    rpi_reg_rw_t vpf1;   // 0x58
    rpi_reg_ro_t vpf1_ctx;   // 0x5C

    rpi_reg_rw_t pfkph0;   // 0x60
    rpi_reg_rw_t pfkpv0;   // 0x64
    rpi_reg_rw_t pfkph1;   // 0x68
    rpi_reg_rw_t pfkpv1;   //   0x6C
} YUV_plane_t;

//8 bit RGB palette
typedef struct {
    // bit 31 = 1 end of list
    // bit 30 = 1 valid list
    // bit 29..24 = 0x20 list element size in words
    // bit 22 rgb_transparency 1 = detect RGB for a transparency colour
    // bit 12:11 = 0x3 repeat  RGBA expansion
    // bit 10:8 = scl1_mode =0
    // bit 7:5 = scl0_mode =0 Hori interpolation vertical interpolation
    // bit 3:0 = pixel format 0xd = 8 bit RGB palette
    //                        0x2 = 16bit RGB 565
    //                        0x7 = 32bit RGBA
    //                        0x10 = YUV 4:2:2 separate Y U V planes

    rpi_reg_rw_t ctrl;      // 0x00
    // 31-24 = alpha 0xff bypasses alpha blending
    // 23-12 = stat_y  start scan line
    // 11-0 = stat_x  start pixel
    rpi_reg_rw_t pos;       // 0x04
    // 27-16 = height of scaled image
    // 11-0 = width of scaled image
    rpi_reg_rw_t scale;     // 0x08
    // 31-28 = alpha modes etc
    // 27-16 = source height
    // 11-0 = source width
    rpi_reg_rw_t src_size;  // 0x0c
    rpi_reg_ro_t src_context;  // 0x10

    rpi_reg_rw_t y_ptr;  // 0x14
    rpi_reg_rw_t y_ctx;  // 0x18

    // 31-26 = pixels to drop at the beginning of each line
    // 25 = 0 Alpha [7:0] 1 Alpha [31:24]
    // 15:0 = pitch to next line
    rpi_reg_rw_t pitch;  // 0x1C

    // 31:30 = 0 1bpp , 1 2bpp, 2 4bpp, 3 8bpp
    // 29..27 = initial pixel offset
    // 26 pal_order
    // 11..0 = palette base
    rpi_reg_rw_t palette;  // 0x20

    // rpi_reg_rw_t trans_rgb
    rpi_reg_rw_t LBM;   // 0x24

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = horizontal scaling factor
    //       =  (((1<<16)*src_width)/scl_width)
    // 6..0 = initial phase
    rpi_reg_rw_t hpf0;   // 0x28

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = vertical scaling factor
    //       =  (((1<<16)*src_height)/scl_height)
    // 6..0 = initial phase
    rpi_reg_rw_t vpf0;   // 0x2C
    rpi_reg_ro_t vpf0_ctx;   // 0x30

    rpi_reg_rw_t pfkph0;   // 0x34
    rpi_reg_rw_t pfkpv0;   // 0x38
} rgb_8bit_t;

//32 bit RGB
typedef struct {
    // bit 31 = 1 end of list
    // bit 30 = 1 valid list
    // bit 29..24 = 0x20 list element size in words
    // bit 22 rgb_transparency 1 = detect RGB for a transparency colour
    // bit 12:11 = 0x3 repeat  RGBA expansion
    // bit 10:8 = scl1_mode =0
    // bit 7:5 = scl0_mode =0 Hori interpolation vertical interpolation
    // bit 3:0 = pixel format 0xd = 8 bit RGB palette
    //                        0x2 = 16bit RGB 565
    //                        0x7 = 32bit RGBA
    //                        0xA = YUV 4:2:2 separate Y U V planes

    rpi_reg_rw_t ctrl;      // 0x00
    // 31-24 = alpha 0xff bypasses alpha blending
    // 23-12 = stat_y  start scan line
    // 11-0 = stat_x  start pixel
    rpi_reg_rw_t pos;       // 0x04
    // 27-16 = height of scaled image
    // 11-0 = width of scaled image
    rpi_reg_rw_t scale;     // 0x08
    // 31-28 = alpha modes etc
    // 27-16 = source height
    // 11-0 = source width
    rpi_reg_rw_t src_size;  // 0x0c
    rpi_reg_ro_t src_context;  // 0x10

    rpi_reg_rw_t y_ptr;  // 0x14
    rpi_reg_ro_t y_ctx;  // 0x18

    // 31-26 = pixels to drop at the beginning of each line
    // 25 = 0 Alpha [7:0] 1 Alpha [31:24]
    // 15:0 = pitch to next line
    rpi_reg_rw_t pitch;  // 0x1C

    // rpi_reg_rw_t trans_rgb
    rpi_reg_rw_t LBM;   // 0x20

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = horizontal scaling factor
    //       =  (((1<<16)*src_width)/scl_width)
    // 6..0 = initial phase
    rpi_reg_rw_t hpf0;   // 0x24

    // 31 = 0 interpolate to 64 phases
    // 30 = 0 use all four interpolated values
    // 24..8 = vertical scaling factor
    //       =  (((1<<16)*src_height)/scl_height)
    // 6..0 = initial phase
    rpi_reg_rw_t vpf0;   // 0x28
    rpi_reg_ro_t vpf0_ctx;   // 0x2c

    rpi_reg_rw_t pfkph0;   // 0x30
    rpi_reg_rw_t pfkpv0;   // 0x34
} rgb_t;

static float rgb_scale = 0.0f;
static uint32_t xoffset = 0;
static uint32_t yoffset = 0;

static bool plane_valid[8];

// Allocates a buffer from CMA aligned to a 4K page boundary

uint32_t screen_allocate_buffer( uint32_t buffer_size, uint32_t * handle )
{
    rpi_mailbox_property_t *mp;
    RPI_PropertyStart(TAG_ALLOCATE_MEMORY, 3);
    RPI_PropertyAddTwoWords(  buffer_size,  4096);
    RPI_PropertyAdd((1<<6) + (1<<5) + (1<<4) + (1<<2) ); // FLAGS
    RPI_PropertyProcess(true);
    if( ( mp = RPI_PropertyGet( TAG_ALLOCATE_MEMORY ) ) )
    {
        *handle = mp->data.buffer_32[0];
        RPI_PropertyStart(TAG_LOCK_MEMORY, 1);
        RPI_PropertyAdd( *handle );
        RPI_PropertyProcess(true);
        if( (mp = RPI_PropertyGet( TAG_LOCK_MEMORY ) ) )
        {
            LOG_DEBUG("Allocated buffer at %"PRIx32"\r\n", mp->data.buffer_32[0]);
            return mp->data.buffer_32[0] & 0x3FFFFFFF;
        }
    }
    return 0;
}

void screen_release_buffer( uint32_t handle )
{
    RPI_PropertyStart(TAG_UNLOCK_MEMORY, 1);
    RPI_PropertyAdd(handle);
    RPI_PropertyProcess(true);
    if( RPI_PropertyGet( TAG_UNLOCK_MEMORY ) )
    {
        RPI_PropertyStart(TAG_RELEASE_MEMORY, 1);
        RPI_PropertyAdd(handle );
        RPI_PropertyProcess(false);
    }
}

static void setup_polyphase(void)
{
    context_memory[PLOYPHASE_BASE + 0] = 0x7ebfc00;
    context_memory[PLOYPHASE_BASE + 1] = 0x7e3edf8;
    context_memory[PLOYPHASE_BASE + 2] = 0x4805fd;
    context_memory[PLOYPHASE_BASE + 3] = 0x1dca432;
    context_memory[PLOYPHASE_BASE + 4] = 0x355769b;
    context_memory[PLOYPHASE_BASE + 5] = 0x1c6e3;
    context_memory[PLOYPHASE_BASE + 6] = 0x355769b;
    context_memory[PLOYPHASE_BASE + 7] = 0x1dca432;
    context_memory[PLOYPHASE_BASE + 8] = 0x4805fd;
    context_memory[PLOYPHASE_BASE + 9] = 0x7e3edf8;
    context_memory[PLOYPHASE_BASE +10] = 0x7ebfc00;
}

#define MAX_PLANES_SIZE 0x80
#define PLANE_BASE (MAX_PLANES_SIZE>>2)

static uint32_t* screen_get_nextplane( uint32_t planeno )
{
    static uint32_t* plane = 0;
    uint32_t* returnplane = 0;

    RPI_hvs->list1 = PLANE_BASE;
    context_memory[0] = 0x80000000; // set end of list bit and clear valid bit for other display lists

    for (uint32_t i=0; i<planeno; i++)
    {
        if (plane_valid[i] == false)
        {
            // ensure previous planes are skipped if not used.
            context_memory[ (MAX_PLANES_SIZE >>2 ) * i + PLANE_BASE ] = 0x00000000 + ((MAX_PLANES_SIZE>>2)<<24); // clear valid and set list size
        }
    }

    plane = &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];

    *plane = 0x80000000; // set end of list bit and clear valid
    returnplane = plane;
    plane = plane + (MAX_PLANES_SIZE>>2); // space for 32 words in context memory
    if (plane_valid[planeno+1] == false)
    {
        *plane = 0x80000000; // set end of list bit and clear valid
    }

    return returnplane;
}

static uint32_t screen_scale ( uint32_t width, uint32_t height , float par, bool yuv, uint32_t scale_height, uint32_t* scaled_width, uint32_t* scaled_height, uint32_t* startpos,  uint32_t *nsh, uint32_t *nh)
{
    static float yuv_scale = 0.0f;
    static uint32_t offset = 0;
    // Calculate optimal overscan
    uint32_t h_display = ( RPI_hvs->ctrl1 >> 12 ) & 0xfff;
    uint32_t v_display = ( RPI_hvs->ctrl1       ) & 0xfff;
#ifdef SCREEN_DEBUG
    LOG_DEBUG("actual Display %"PRId32" x %"PRId32"\r\n", h_display, v_display);
#endif
   // TODO: this can be greatly improved!
    // It assumes you want to fill (or nearly fill) a 1280x1024 window on your physical display
    // It will work really badly with an 800x600 screen mode, say on a 1600x1200 monitor

    uint32_t h_corrected;
    uint32_t v_corrected;

    if (par > 1.0f) {
       // Wide pixels
       h_corrected = (uint32_t) (((float)width) * par);
       v_corrected = height;
    } else {
        if  (par < 1.0f)
        {
            // Narrow pixels
            h_corrected = width;
            v_corrected = (uint32_t) (((float)height) / par);
        }
        else {
            // Square pixels
            h_corrected = width;
            v_corrected = height;
        }
    }
#ifdef SCREEN_DEBUG
    LOG_DEBUG("corrected %"PRId32" x %"PRId32"\r\n", h_corrected, v_corrected);
#endif
    if (yuv)
    {

        if (yuv_scale < 0.1f)
        {
            switch ( v_display)
            {   // here we choose scaling factors that will give a good ratio for the RGB overlay
                case 480: yuv_scale = 1.75/2; break;  // 256 * 1.75 = 448
                case 576: yuv_scale = 1 ; break; // 256 * 2 = 512
                case 600: yuv_scale = 2.25/2; break;  // 256 * 2.25 = 576
                case 720: yuv_scale = 2.75/2; break;  // 256 * 2.75 = 704
                case 768: yuv_scale = 3/2; break;  // 256 * 3 = 768
                case 800: yuv_scale = 3/2; break;  // 256 * 3 = 768
                case 864: yuv_scale = 3.25/2; break;  // 256 * 3.25 = 832
                case 900: yuv_scale = 3.5/2; break;  // 256 * 3.5 = 896
                case 960: yuv_scale = 3.75/2; break;  // 256 * 3.75 = 960
                case 1024: yuv_scale = 4/2; break;  // 256 * 4 = 1024
                case 1050: yuv_scale = 4/2; break;  // 256 * 4 = 1024
                case 1080: yuv_scale = 4/2; break;  // 256 * 4 = 1024
                case 1200: yuv_scale = 4.5/2; break;  // 256 * 4.5 = 1152
                case 1440: yuv_scale = 5.5/2; break;  // 256 * 5.5 = 1408
                case 1536: yuv_scale = 6/2; break;  // 256 * 6 = 1536
                case 1600: yuv_scale = 6.25/2; break;  // 256 * 6.25 = 1600
                default: yuv_scale = ((float)v_display/256)/2; break;  // 256 * 2 = 512
            }
            rgb_scale = yuv_scale*2;
        }

        *scaled_width = (((uint32_t)(yuv_scale * (float)h_corrected)) & 0xfff);
        *scaled_height = (((uint32_t)(yuv_scale * (float)v_corrected)) & 0xfff);

        if (*scaled_height > v_display)
        {
            offset = (uint32_t) ((float )(( *scaled_height -v_display) /2) /yuv_scale);

            *nsh = v_display;
            *nh = (uint32_t)((float)v_display/yuv_scale);
            uint32_t h_overscan = (h_display - *scaled_width) / 2;
            *startpos = (h_overscan & 0xfff);
            return offset;
        }

        *nsh = *scaled_height;
        *nh = height;
        uint32_t h_overscan = (h_display - *scaled_width) / 2;
        uint32_t v_overscan = (v_display - *scaled_height) / 2;

        *startpos = ((v_overscan & 0xfff)<<12) + (h_overscan & 0xfff);
        return offset;
    }

    if ( rgb_scale < 0.1f)
    {

        uint32_t h_scale = 2 * h_display / h_corrected;
        uint32_t v_scale = 2 * v_display / v_corrected;

        rgb_scale = (h_scale < v_scale) ? (float)h_scale/2 : (float)v_scale/2;
    }

    float scale;
    if (scale_height)
        scale = (256/(float)scale_height) * rgb_scale ;
    else
        if (yuv_scale <0.1f)
        {
            uint32_t h_scale = 2 * h_display / h_corrected;
            uint32_t v_scale = 2 * v_display / v_corrected;

            rgb_scale = (h_scale < v_scale) ? (float)h_scale/2 : (float)v_scale/2;
            scale = rgb_scale;
        }
        else
            scale = rgb_scale ;

    if (((uint32_t)(scale * (float)h_corrected)) >  h_display)
        scale = scale/2;
    if (((uint32_t)(scale * (float)v_corrected)) >  v_display)
        scale = scale/2;
#ifdef SCREEN_DEBUG
    LOG_DEBUG("scale %f\r\n", (double) scale);
    LOG_DEBUG("rgb_scale %f\r\n", (double) rgb_scale);
#endif
    *scaled_width = (((uint32_t)(scale * (float)h_corrected)) & 0xfff);
    *scaled_height = (((uint32_t)(scale * (float)v_corrected)) & 0xfff);
#ifdef SCREEN_DEBUG
    LOG_DEBUG("scaled %"PRId32" x %"PRId32"\r\n", *scaled_width, *scaled_height);
#endif
    uint32_t h_overscan = (h_display - *scaled_width) / 2;
    uint32_t v_overscan = (v_display - *scaled_height) / 2;
#ifdef SCREEN_DEBUG
    LOG_DEBUG("overscan %"PRId32" x %"PRId32"\r\n", h_overscan, v_overscan);
#endif
    *startpos = ((v_overscan & 0xfff)<<12) + (h_overscan & 0xfff);

    if (!scale_height)
    {
        xoffset = h_overscan;
        yoffset = v_overscan;
    }
    return 0;
}

/* phase magnitude bits */
#define PHASE_BITS 6
// entry width or height, scaled width or height, x or y position, 1 for cbcr 0 for y
static uint32_t vc4_ppf( uint32_t src, uint32_t dst,
			 uint32_t xy, int channel)
{
	uint32_t scale = (src<<16) / dst;
	int offset, offset2;
	int phase;

	/*
	 * Start the phase at 1/2 pixel from the 1st pixel at src_x.
	 * 1/4 pixel for YUV.
	 */
	if (channel) {
		/*
		 * The phase is relative to scale_src->x, so shift it for
		 * display list's x value
		 */
		offset = (xy & 0x1ffff) >> (16 - PHASE_BITS) >> 1;
		offset += -(1 << PHASE_BITS >> 2);
	} else {
		/*
		 * The phase is relative to scale_src->x, so shift it for
		 * display list's x value
		 */
		offset = (xy & 0xffff) >> (16 - PHASE_BITS);
		offset += -(1 << PHASE_BITS >> 1);

		/*
		 * This is a kludge to make sure the scaling factors are
		 * consistent with YUV's luma scaling. We lose 1-bit precision
		 * because of this.
		 */
		scale &= ~(uint32_t)1;
	}

	/*
	 * There may be a also small error introduced by precision of scale.
	 * Add half of that as a compromise
	 */
	offset2 = (int) src - (int) dst * (int) scale;
	offset2 >>= 16 - PHASE_BITS;
	phase = offset + (offset2 >> 1);

	/* Ensure +ve values don't touch the sign bit, then truncate negative values */
	if (phase >= 1 << PHASE_BITS)
		phase = (1 << PHASE_BITS) - 1;

	phase &= 0x7F;

    return  (1<<30) + (scale << 8 ) + ( uint32_t) phase;
}

#if 0
static void tpz( uint32_t src, uint32_t scl, uint32_t *ptr)
{
    uint32_t tpz = (src << 16) / scl;
    *ptr++ = tpz <<8;
    *ptr   = 0xffffffff / tpz
}
#endif

// returns plane pointer
void screen_create_YUV_plane( uint32_t planeno, uint32_t width, uint32_t height, uint32_t buffer )
{
    uint32_t * plane =  screen_get_nextplane( planeno);
    LOG_DEBUG("plane %"PRIu32"\r\n", planeno);
    buffer |= 0xC0000000;
    if (plane)
    {
        uint32_t scaled_width;
        uint32_t scaled_height;
        uint32_t startpos;
        uint32_t nsh;
        uint32_t nh;
        uint32_t vertical_offset = screen_scale(width, height , 1.0f, true,0, &scaled_width, &scaled_height, &startpos, &nsh, &nh);
#if 0
        LOG_DEBUG("scaled %"PRId32" x %"PRId32"\r\n", scaled_width, scaled_height);

        LOG_DEBUG("startpos %"PRIx32"\r\n", startpos);
        LOG_DEBUG("nsh %"PRId32"\r\n", nsh);
        LOG_DEBUG("nh %"PRId32"\r\n", nh);
#endif
        YUV_plane_t* yuv = (YUV_plane_t*) plane;
        yuv->ctrl = 0x00000000 + (0x20<<24) + (1<<13 ) + 0xA; // invalid list, 32 words, YCrcb format , YUV
        yuv->pos = startpos;
        yuv->scale = (nsh << 16) + scaled_width;
        yuv->src_size =  ((nh) << 16) + width;
        //yuv->src_context = 0;
        yuv->y_ptr =  buffer + vertical_offset*width;
        yuv->cb_ptr = buffer + width*height + width*height/2 + vertical_offset*width/2;
        yuv->cr_ptr = buffer + width*height + vertical_offset*width/2;
        //yuv->y_ctx = 0;
        //yuv->cb_ctx = 0;
        //yuv->cr_ctx = 0;
        yuv->pitch = width;
        yuv->pitch1 = width/2;
        yuv->pitch2 = width/2;
        yuv->csc0 = 0x00F00000;
        yuv->csc1 = 0xe73304A8;
        yuv->csc2 = 0x00066604;
        yuv->LBM = (LBM_PLANE_SIZE * planeno);
        yuv->hpf0 = vc4_ppf(width, scaled_width*2, startpos & 0xFFF, 0 );
        yuv->vpf0 = vc4_ppf(nh, nsh, startpos >>12, 0 );
        //yuv->vpf0_ctx = 0;

        yuv->hpf1 = vc4_ppf(width, scaled_width, startpos & 0xFFF, 0 );
        yuv->vpf1 = vc4_ppf(nh, nsh, startpos >>12, 0 );;
        //yuv->vpf1_ctx = 0;
        yuv->pfkph0 = PLOYPHASE_BASE;
        yuv->pfkpv0 = PLOYPHASE_BASE;
        yuv->pfkph1 = PLOYPHASE_BASE;
        yuv->pfkpv1 = PLOYPHASE_BASE;
#if 0
        LOG_DEBUG("ctrl %"PRIx32"\r\n", yuv->ctrl);
        LOG_DEBUG("pos %"PRIx32"\r\n", yuv->pos);
        LOG_DEBUG("scale %"PRIx32"\r\n", yuv->scale);
        LOG_DEBUG("src_size %"PRIx32"\r\n", yuv->src_size);

        LOG_DEBUG("y_ptr %"PRIx32"\r\n", yuv->y_ptr);
        LOG_DEBUG("cb_ptr %"PRIx32"\r\n", yuv->cb_ptr);
        LOG_DEBUG("cr_ptr %"PRIx32"\r\n", yuv->cr_ptr);
        LOG_DEBUG("pitch %"PRIx32"\r\n", yuv->pitch);
        LOG_DEBUG("pitch1 %"PRIx32"\r\n", yuv->pitch1);
        LOG_DEBUG("pitch2 %"PRIx32"\r\n", yuv->pitch2);
        LOG_DEBUG("csc0 %"PRIx32"\r\n", yuv->csc0);
        LOG_DEBUG("csc1 %"PRIx32"\r\n", yuv->csc1);
        LOG_DEBUG("csc2 %"PRIx32"\r\n", yuv->csc2);
        LOG_DEBUG("LBM %"PRIx32"\r\n", yuv->LBM);
        LOG_DEBUG("hpf0 %"PRIx32"\r\n", yuv->hpf0);
        LOG_DEBUG("vpf0 %"PRIx32"\r\n", yuv->vpf0);
        LOG_DEBUG("hpf1 %"PRIx32"\r\n", yuv->hpf1);
        LOG_DEBUG("vpf1 %"PRIx32"\r\n", yuv->vpf1);
        LOG_DEBUG("pfkph0 %"PRIx32"\r\n", yuv->pfkph0);
        LOG_DEBUG("pfkpv0 %"PRIx32"\r\n", yuv->pfkpv0);
        LOG_DEBUG("pfkph1 %"PRIx32"\r\n", yuv->pfkph1);
        LOG_DEBUG("pfkpv1 %"PRIx32"\r\n", yuv->pfkpv1);
#endif
        setup_polyphase();
#ifdef DEBUG
        uint32_t *cache = (uint32_t *) (PERIPHERAL_BASE+ 0xe02000 + 0x10C);
        LOG_DEBUG("cache L1 %"PRIx32"\r\n", *cache);

        uint32_t *hvs = (uint32_t *) (PERIPHERAL_BASE+ 0x04000 + 0x08);
        LOG_DEBUG("HVS pri %"PRIx32"\r\n", *hvs);
#endif
        //RPI_hvs->ectrl = 0x713f0000; // reduce maximum best burst to 8 beats to give time for GPU to read data
    }
    plane_valid[planeno] = true;
}

// returns plane pointer
void screen_create_RGB_plane( uint32_t planeno, uint32_t width, uint32_t height, float par , uint32_t scale_height, uint32_t colour_depth, uint32_t buffer )
{
    uint32_t * plane = screen_get_nextplane( planeno);
    if (plane)
    {
        uint32_t scaled_width;
        uint32_t scaled_height;
        uint32_t startpos;
        uint32_t nsh;
        uint32_t nh;
        screen_scale(width, height , par, false, scale_height,  &scaled_width, &scaled_height, &startpos, &nsh, &nh);
        buffer |= 0x80000000; // if we use &C then there is an error on the screen
        if (colour_depth == 3)
        {
            rgb_8bit_t* rgb = (rgb_8bit_t*) plane;
            rgb->ctrl = 0x00000000 + (0x20<<24) + (3<<11) + 0xD; // invalid list, 32 words, 8 bit RGB
            rgb->pos = startpos + 0xFF000000;
            rgb->scale = (scaled_height << 16) + scaled_width;
            rgb->src_size =  (height << 16) + width;
            //rgb->src_context = 0;
            rgb->y_ptr = buffer ;
            rgb->y_ctx = buffer;
            rgb->pitch = width;
            rgb->palette = 0xC0000000 + PALETTE_BASE; // 8 bit palette
            rgb->LBM = (LBM_PLANE_SIZE * planeno);
            rgb->hpf0 = vc4_ppf(width, scaled_width, startpos & 0xFFF, 0 );
            rgb->vpf0 = vc4_ppf(height, scaled_height, startpos >>12, 0 );
            //rgb->vpf0_ctx = 0;
            rgb->pfkph0 = PLOYPHASE_BASE;
            rgb->pfkpv0 = PLOYPHASE_BASE;

#ifdef SCREEN_DEBUG
            LOG_DEBUG("plane %"PRIu32"\r\n", planeno);
            LOG_DEBUG("scaled %"PRId32" x %"PRId32"\r\n", scaled_width, scaled_height);

            LOG_DEBUG("startpos %"PRIx32"\r\n", startpos);
          //  LOG_DEBUG("nsh %"PRId32"\r\n", nsh);
          //  LOG_DEBUG("nh %"PRId32"\r\n", nh);
            LOG_DEBUG("ctrl %"PRIx32"\r\n", rgb->ctrl);
            LOG_DEBUG("pos %"PRIx32"\r\n", rgb->pos);
            LOG_DEBUG("scale %"PRIx32"\r\n", rgb->scale);
            LOG_DEBUG("src_size %"PRIx32"\r\n", rgb->src_size);
            LOG_DEBUG("y_ptr %"PRIx32"\r\n", rgb->y_ptr);
            LOG_DEBUG("palette %"PRIx32"\r\n", rgb->palette);
            LOG_DEBUG("pitch %"PRIx32"\r\n", rgb->pitch);
            LOG_DEBUG("LBM %"PRIx32"\r\n", rgb->LBM);
            LOG_DEBUG("hpf0 %"PRIx32"\r\n", rgb->hpf0);
            LOG_DEBUG("vpf0 %"PRIx32"\r\n", rgb->vpf0);
            LOG_DEBUG("pfkph0 %"PRIx32"\r\n", rgb->pfkph0);
            LOG_DEBUG("pfkpv0 %"PRIx32"\r\n", rgb->pfkpv0);
#endif

        }
        else
        {
            rgb_t* rgb = (rgb_t*) plane;
            if (colour_depth == 4)
            {
                rgb->ctrl = 0x00000000 + (0x20<<24) + (2<<13) + 0x4; // invalid list, 32 words, RGB format, 16 bit RGB
                rgb->pitch = width <<1;
            }
            else
            {
                rgb->ctrl = 0x00000000 + (0x20<<24) + (3<<13) + 0x7; // invalid list, 32 words, ABGR format, 32 bit RGB
                rgb->pitch = width <<2;
            }
            rgb->pos = startpos;
            rgb->scale = (scaled_height << 16) + scaled_width;
            rgb->src_size =  (height << 16) + width;
            //rgb->src_context = 0;
            rgb->y_ptr = buffer;

            rgb->LBM = (LBM_PLANE_SIZE * planeno);
            rgb->hpf0 = vc4_ppf(width, scaled_width, startpos & 0xFFF, 0 );
            rgb->vpf0 = vc4_ppf(height, scaled_height, startpos >>12, 0 );
            //rgb->vpf0_ctx = 0;
            rgb->pfkph0 = PLOYPHASE_BASE;
            rgb->pfkpv0 = PLOYPHASE_BASE;
        }


        setup_polyphase();
    }
    plane_valid[planeno] = true;
}

void screen_set_plane_position( uint32_t planeno, int32_t x, int32_t y )
{
    // we can cheat here as we are only changing the position
    rgb_8bit_t* rgb = (rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];;

// we should clip the plane to the screen size

    int newy = (int) (( float) y * rgb_scale) + (int) yoffset;
    if (newy < 0)
    {
        newy = 0;
    }

    int newx = (int) (( float) x * rgb_scale) + (int)xoffset;
    if (newx < 0)
    {
        newx = 0;
    }
    rgb->pos = (  ((uint32_t)newy&0xfff) << 12) +(  ((uint32_t)newx &0xfff) ) ;
}

void screen_plane_enable( uint32_t planeno , bool enable )
{
    LOG_DEBUG("plane %"PRIu32" %s\r\n", planeno, enable ? "enable" : "disable");
    rgb_8bit_t* rgb = (rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];

    if (enable)
    {
        if (~(RPI_hdmi->hotplug)&1)
            rgb->ctrl |= (uint32_t)0x40000000;
    }
    else
    {
        rgb->ctrl &= ~(uint32_t)0x40000000;
    }
#ifdef SCREEN_DEBUG
    LOG_DEBUG("plane %"PRIu32"\r\n", planeno);
    LOG_DEBUG("ctrl %"PRIx32"\r\n", rgb->ctrl);
    LOG_DEBUG("pos %"PRIx32"\r\n", rgb->pos);
    LOG_DEBUG("scale %"PRIx32"\r\n", rgb->scale);
    LOG_DEBUG("src_size %"PRIx32"\r\n", rgb->src_size);
    LOG_DEBUG("y_ptr %"PRIx32"\r\n", rgb->y_ptr);
    LOG_DEBUG("palette %"PRIx32"\r\n", rgb->palette);
    LOG_DEBUG("pitch %"PRIx32"\r\n", rgb->pitch);
    LOG_DEBUG("LBM %"PRIx32"\r\n", rgb->LBM);
    LOG_DEBUG("hpf0 %"PRIx32"\r\n", rgb->hpf0);
    LOG_DEBUG("vpf0 %"PRIx32"\r\n", rgb->vpf0);
    LOG_DEBUG("pfkph0 %"PRIx32"\r\n", rgb->pfkph0);
    LOG_DEBUG("pfkpv0 %"PRIx32"\r\n", rgb->pfkpv0);
#endif
}

void screen_update_palette_entry( uint32_t entry, uint32_t r , uint32_t g , uint32_t b )
{
    // palette 0 is normal colours
    // palette 1 is flash colours
    // palette 2 is black is alpha 0
    // palette 3 is flash and black is alpha 0

    uint32_t colour = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);

    context_memory[(PALETTE_BASE>>2) + entry] = 0xff000000 | colour;

    if (colour || ((entry&255) >15 ))
        colour |= 0xff000000; // set alpha to 0xff if not black

    context_memory[(PALETTE_BASE>>2) + entry + (256*2)] = colour;
}

uint32_t screen_get_palette_entry( uint32_t entry )
{
    return context_memory[(PALETTE_BASE>>2) + entry];
}

// flags
// 0 set palette
// 1 set palette ( preserve alpha)
// 2 set alpha palette
// 3 clear alpha
// 4 flash palette

void screen_set_palette( uint32_t planeno, uint32_t palette, uint32_t flags )
{
    rgb_8bit_t* rgb = (rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];
    unsigned int cpsr = _disable_interrupts_cspr();
    if ( (rgb->ctrl & 0xF) == 0xD)
    {
        uint32_t old_palette = ((rgb->palette & 0x00003fff) - PALETTE_BASE)/0x400;

        switch (flags)
        {
            case 0:
                rgb->palette = ( 0xc0000000 ) | ((palette*0x400) + PALETTE_BASE);
                break;
            case 1:
                rgb->palette = ( 0xc0000000 ) | ((((old_palette & 2) | palette)*0x400) + PALETTE_BASE);
                break;
            case 2:
                rgb->palette = ( 0xc0000000 ) | (((old_palette | 2)*0x400) + PALETTE_BASE);
                break;
            case 3:
                rgb->palette = ( 0xc0000000 ) | (((old_palette & 1 )*0x400) + PALETTE_BASE);
                break;
            case 4:
                rgb->palette = ( 0xc0000000 ) | (((old_palette ^ 1 )*0x400) + PALETTE_BASE);
                break;
        }
    }
    _restore_cpsr(cpsr);
}

void screen_set_vsync( bool enable )
{
    if (enable)
    {
        RPI_hvs->ctrl |=  (1 <<9) + 1; // end of frame and enable IRQs
        RPI_GetIrqController()->Enable_IRQs_2 = RPI_HVS_IRQ;
    }
    else
    {
        RPI_hvs->ctrl &= (uint32_t)~( (1<<9) + 1);
        RPI_GetIrqController()->Disable_IRQs_2 = RPI_HVS_IRQ;
    }
}

bool screen_check_vsync( void )
{
    if (RPI_hvs->stat & ( 1 << 16)) // check for end of frame
    {
        RPI_hvs->stat = ( 1 << 16); // clear the interrupt
        return true;
    }
    return false;
}
