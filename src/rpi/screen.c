#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "base.h"
#include "mailbox.h"
#include "rpi.h"
#include "../rpi/asm-helpers.h"
#include "../rpi/interrupts.h"
#include "../rpi/systimer.h"   /* RPI_GetSystemTime - refresh-rate measurement */

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

#define MAX_PLANES 7
#define POLYPHASE_BASE (0xf00>>2)
#define PALETTE_BASE (0x1000)

/* Line-buffer memory for vertical scaling, 48K words in total.  It is NOT
   divided evenly: the video, computer and pointer planes keep the 12K each
   they were designed around (shrinking them corrupts the video plane), and
   the four VP5 dim strips get 512 words apiece - they scale a handful of
   constant source pixels, so they need almost nothing. */
#define LBM_PLANE_SIZE (12*1024)
#define LBM_STRIP_SIZE (512u)

static uint32_t screen_lbm_base(uint32_t planeno)
{
    if (planeno < 3u)
        return LBM_PLANE_SIZE * planeno;
    return (LBM_PLANE_SIZE * 3u) + ((planeno - 3u) * LBM_STRIP_SIZE);
}

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
static volatile uint32_t* context_memory = (volatile uint32_t*) (PERIPHERAL_BASE+ 0x402000);

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

/* Per-plane byte offsets (Y, Cb, Cr) that screen_create_YUV*_plane baked
   into the pointers for the vertical-overscan crop - screen_set_YUV_pointers
   must re-apply them or the picture jumps on displays where the scaled
   image exceeds the screen height. */
static uint32_t yuv_ptr_offset[MAX_PLANES][3];

/**
 * @brief Allocates a buffer from CMA aligned to a 4K page boundary.
 *
 * @param buffer_size Size of the buffer to allocate.
 * @param handle Pointer to store the handle of the allocated buffer.
 * @return uint32_t Address of the allocated buffer.
 */
uint32_t screen_allocate_buffer(uint32_t buffer_size, uint32_t *handle) {
    rpi_mailbox_property_t *mp;
    RPI_PropertyStart(TAG_ALLOCATE_MEMORY, 3);
    RPI_PropertyAddTwoWords(buffer_size, 4096);
    RPI_PropertyAdd((1 << 6) + (1 << 5) + (1 << 4) + (1 << 2)); // FLAGS
    RPI_PropertyProcess(true);
    if ((mp = RPI_PropertyGet(TAG_ALLOCATE_MEMORY))) {
        *handle = mp->data.buffer_32[0];
        RPI_PropertyStart(TAG_LOCK_MEMORY, 1);
        RPI_PropertyAdd(*handle);
        RPI_PropertyProcess(true);
        if ((mp = RPI_PropertyGet(TAG_LOCK_MEMORY))) {
            LOG_DEBUG("Allocated buffer at %" PRIx32 "\r\n", mp->data.buffer_32[0]);
            return mp->data.buffer_32[0] & 0x3FFFFFFF;
        }
    }
    return 0;
}

/**
 * @brief Releases a previously allocated buffer.
 *
 * @param handle Handle of the buffer to release.
 */
void screen_release_buffer(uint32_t handle) {
    RPI_PropertyStart(TAG_UNLOCK_MEMORY, 1);
    RPI_PropertyAdd(handle);
    RPI_PropertyProcess(true);
    if (RPI_PropertyGet(TAG_UNLOCK_MEMORY)) {
        RPI_PropertyStart(TAG_RELEASE_MEMORY, 1);
        RPI_PropertyAdd(handle);
        RPI_PropertyProcess(false);
    }
}

/**
 * @brief Sets up the polyphase filter coefficients.
 */
static void setup_polyphase(void) {
    context_memory[POLYPHASE_BASE + 0] = 0x7ebfc00;
    context_memory[POLYPHASE_BASE + 1] = 0x7e3edf8;
    context_memory[POLYPHASE_BASE + 2] = 0x4805fd;
    context_memory[POLYPHASE_BASE + 3] = 0x1dca432;
    context_memory[POLYPHASE_BASE + 4] = 0x355769b;
    context_memory[POLYPHASE_BASE + 5] = 0x1c6e3;
    context_memory[POLYPHASE_BASE + 6] = 0x355769b;
    context_memory[POLYPHASE_BASE + 7] = 0x1dca432;
    context_memory[POLYPHASE_BASE + 8] = 0x4805fd;
    context_memory[POLYPHASE_BASE + 9] = 0x7e3edf8;
    context_memory[POLYPHASE_BASE + 10] = 0x7ebfc00;
}

#define MAX_PLANES_SIZE 0x80
#define PLANE_BASE (MAX_PLANES_SIZE>>2)

/**
 * @brief Gets the next available plane.
 *
 * @param planeno Plane number to get.
 * @return uint32_t* Pointer to the next available plane.
 */
static volatile uint32_t* screen_get_nextplane(uint32_t planeno) {
    static volatile uint32_t* plane;
    volatile uint32_t* returnplane;

    RPI_hvs->list1 = PLANE_BASE;
    context_memory[0] = 0x80000000; // set end of list bit and clear valid bit for other display lists

    for (uint32_t i = 0; i < planeno; i++) {
        if (plane_valid[i] == false) {
            // ensure previous planes are skipped if not used.
            context_memory[(MAX_PLANES_SIZE >> 2) * i + PLANE_BASE] = 0x00000000 + ((MAX_PLANES_SIZE >> 2) << 24); // clear valid and set list size
        }
    }

    plane = &context_memory[(MAX_PLANES_SIZE >> 2) * planeno + PLANE_BASE];

    // Clear the entire plane context to zero out stale fields (src_context, y_ctx, etc.)
    // left over from the GPU bootloader splash screen
    for (uint32_t i = 0; i < (MAX_PLANES_SIZE >> 2); i++)
        plane[i] = 0;

    *plane = 0x80000000; // set end of list bit and clear valid
    returnplane = plane;
    plane = plane + (MAX_PLANES_SIZE >> 2); // space for 32 words in context memory
    if (plane_valid[planeno + 1] == false) {
        *plane = 0x80000000; // set end of list bit and clear valid
    }

    return returnplane;
}

/**
 * @brief Calculates the scaling factors for the screen.
 *
 * @param width Width of the source image.
 * @param height Height of the source image.
 * @param par Pixel aspect ratio.
 * @param yuv Flag indicating if the image is YUV.
 * @param scale_height Height to scale to.
 * @param scaled_width Pointer to store the scaled width.
 * @param scaled_height Pointer to store the scaled height.
 * @param startpos Pointer to store the start position.
 * @param nsh Pointer to store the new scaled height.
 * @param nh Pointer to store the new height.
 * @return uint32_t Vertical offset.
 */
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
                case 768: yuv_scale = 3.0/2; break;  // 256 * 3 = 768
                case 800: yuv_scale = 3.0/2; break;  // 256 * 3 = 768
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
/**
 * @brief Calculates the phase magnitude bits for the VC4.
 *
 * @param src Source size.
 * @param dst Destination size.
 * @param xy Position.
 * @param channel Channel (1 for cbcr, 0 for y).
 * @return uint32_t Phase magnitude bits.
 */
static uint32_t vc4_ppf(uint32_t src, uint32_t dst, uint32_t xy, int channel) {
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

/* 4:2:0 3-plane plane (HVS pixel format 8) for the hardware H264 decoder,
   whose output is I420: a full-size Y plane followed by quarter-size Cb
   then Cr planes. Scaler channel 0 is chroma and channel 1 luma, so the
   chroma phase factors are computed against doubled scaled dimensions -
   horizontally because the chroma plane is half-width, vertically
   because it is also half-height. */
void screen_create_YUV420_plane( uint32_t planeno, uint32_t width, uint32_t height, uint32_t buffer )
{
    volatile uint32_t * plane =  screen_get_nextplane( planeno);
    LOG_DEBUG("plane %"PRIu32" (420)\r\n", planeno);
    buffer |= 0xC0000000;
        uint32_t scaled_width;
        uint32_t scaled_height;
        uint32_t startpos;
        uint32_t nsh;
        uint32_t nh;
        uint32_t vertical_offset = screen_scale(width, height , 1.0f, true,0, &scaled_width, &scaled_height, &startpos, &nsh, &nh);

        volatile YUV_plane_t* yuv = (volatile YUV_plane_t*) plane;
        /* Pixel order (bits 13-14), established empirically on Test Card F
           against a PC decode of the same access unit: with order 1 the
           HVS reads the SECOND pointer as Cr (the old code fed it Cb, so
           everything displayed with U/V swapped); order 2 fixes the
           chroma reading but kills luma (bit 14 evidently means something
           else). So: keep order 1 and load the chroma pointers Cr-first
           below and in screen_set_YUV_pointers. */
        yuv->ctrl = 0x00000000 + (0x20<<24) + (1<<13 ) + 0x8; // invalid list, 32 words, YCrCb order, YUV420 3-plane
        yuv->pos = startpos;
        yuv->scale = (nsh << 16) + scaled_width;
        yuv->src_size =  ((nh) << 16) + width;
        // I420 memory order is Y, Cb (U), Cr (V); chroma planes are
        // width/2 x height/2
        yuv_ptr_offset[planeno][0] = vertical_offset*width;
        yuv_ptr_offset[planeno][1] = (vertical_offset/2)*(width/2);
        yuv_ptr_offset[planeno][2] = (vertical_offset/2)*(width/2);
        yuv->y_ptr =  buffer + vertical_offset*width;
        /* Cr-first: the HVS's second pointer is Cr in this order mode (see
           the ctrl comment above); the decoder's I420 memory is Y,Cb,Cr */
        yuv->cb_ptr = buffer + width*height + (width/2)*(height/2) + (vertical_offset/2)*(width/2);
        yuv->cr_ptr = buffer + width*height + (vertical_offset/2)*(width/2);
        yuv->pitch = width;
        yuv->pitch1 = width/2;
        yuv->pitch2 = width/2;
        yuv->csc0 = 0x00F00000;
        yuv->csc1 = 0xe73304A8;
        yuv->csc2 = 0x00066604;
        yuv->LBM = screen_lbm_base(planeno);
        yuv->hpf0 = vc4_ppf(width, scaled_width*2, startpos & 0xFFF, 0 );  // chroma H: (w/2)/sw
        yuv->vpf0 = vc4_ppf(nh, nsh*2, startpos >>12, 0 );                 // chroma V: (h/2)/sh
        yuv->hpf1 = vc4_ppf(width, scaled_width, startpos & 0xFFF, 0 );    // luma H
        yuv->vpf1 = vc4_ppf(nh, nsh, startpos >>12, 0 );                   // luma V
        yuv->pfkph0 = POLYPHASE_BASE;
        yuv->pfkpv0 = POLYPHASE_BASE;
        yuv->pfkph1 = POLYPHASE_BASE;
        yuv->pfkpv1 = POLYPHASE_BASE;
        setup_polyphase();
    plane_valid[planeno] = true;
}

/* Retarget an existing YUV plane at a new frame - the video player's
   page flip. Pass PHYSICAL addresses (the 0xC0000000 alias is added
   here). Takes effect at the next HVS frame fetch; call around vsync
   (screen_check_vsync) for tear-free flips. */
void screen_set_YUV_pointers( uint32_t planeno, uint32_t y, uint32_t cb, uint32_t cr )
{
    volatile YUV_plane_t* yuv = (volatile YUV_plane_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];
    // re-apply the vertical-overscan crop baked in at plane creation
    yuv->y_ptr  = (y  + yuv_ptr_offset[planeno][0]) | 0xC0000000;
    /* Cr-first slot order - see the ctrl comment in screen_create_YUV420 */
    yuv->cb_ptr = (cr + yuv_ptr_offset[planeno][1]) | 0xC0000000;
    yuv->cr_ptr = (cb + yuv_ptr_offset[planeno][2]) | 0xC0000000;
}

// returns plane pointer
void screen_create_RGB_plane( uint32_t planeno, uint32_t width, uint32_t height, float par , uint32_t scale_height, uint32_t colour_depth, uint32_t buffer )
{
    volatile uint32_t * plane = screen_get_nextplane( planeno);
        uint32_t scaled_width;
        uint32_t scaled_height;
        uint32_t startpos;
        uint32_t nsh;
        uint32_t nh;
        screen_scale(width, height , par, false, scale_height,  &scaled_width, &scaled_height, &startpos, &nsh, &nh);
        buffer |= 0x80000000; // if we use &C then there is an error on the screen
        if (colour_depth == 3)
        {
            volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) plane;
            rgb->ctrl = 0x00000000 + (0x20<<24) + (3<<11) + 0xD; // invalid list, 32 words, 8 bit RGB
            rgb->pos = startpos + 0xFF000000;
            rgb->scale = (scaled_height << 16) + scaled_width;
            rgb->src_size =  ((height + 1) << 16) + width;  // +1 for guard line at top
            //rgb->src_context = 0;
            rgb->y_ptr = buffer ;
          //  rgb->y_ctx = buffer;
            rgb->pitch = width;
            rgb->palette = 0xC0000000 + PALETTE_BASE; // 8 bit palette
            rgb->LBM = screen_lbm_base(planeno);
            rgb->hpf0 = vc4_ppf(width, scaled_width, startpos & 0xFFF, 0 );
            rgb->vpf0 = vc4_ppf(height, scaled_height, startpos >>12, 0 );
            //rgb->vpf0_ctx = 0;
            rgb->pfkph0 = POLYPHASE_BASE;
            rgb->pfkpv0 = POLYPHASE_BASE;
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
            volatile rgb_t* rgb = (volatile rgb_t*) plane;
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
            rgb->src_size =  ((height + 1) << 16) + width;  // +1 for guard line at top
            //rgb->src_context = 0;
            rgb->y_ptr = buffer;

            rgb->LBM = screen_lbm_base(planeno);
            rgb->hpf0 = vc4_ppf(width, scaled_width, startpos & 0xFFF, 0 );
            rgb->vpf0 = vc4_ppf(height, scaled_height, startpos >>12, 0 );
            //rgb->vpf0_ctx = 0;
            rgb->pfkph0 = POLYPHASE_BASE;
            rgb->pfkpv0 = POLYPHASE_BASE;
        }


        setup_polyphase();
    plane_valid[planeno] = true;
}

void screen_set_plane_position( uint32_t planeno, int32_t x, int32_t y )
{
    // we can cheat here as we are only changing the position
    volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];

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

/* Overlay translucency for the palettized (8-bit) plane.
   alpha = 0xFF: per-pixel palette alpha only (hard key - black clear,
   graphics opaque). alpha < 0xFF: HVS fixed-nonzero mode - the fixed
   alpha is applied to every pixel whose palette alpha is non-zero, so
   black stays fully transparent and graphics mix over the video at
   alpha/255. Used by the VP415 VP4/VP5 superimpose modes. */
void screen_plane_alpha( uint32_t planeno, uint32_t alpha )
{
    volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];

    if ( (rgb->ctrl & 0xF) != 0xD)
        return; // only the palettized overlay carries per-pixel alpha

    unsigned int cpsr = _disable_interrupts_cspr();
    rgb->pos = (rgb->pos & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
    uint32_t mode = (alpha >= 0xFFu) ? 0u : 2u; // per-pixel : fixed-nonzero
    rgb->src_size = (rgb->src_size & 0x3FFFFFFFu) | (mode << 30);
    _restore_cpsr(cpsr);
}

/* Two layers of visibility, kept separate so neither has to know about
   the other: screen_plane_enable() is what a plane's OWNER wants
   (framebuffer MODE changes, mouseredirect pointer moves, the video
   player's first frame), and screen_plane_gate() is what the VP415
   video mixer allows - VP1 gates the computer planes, VP2 gates the
   video plane. A plane is shown only when wanted AND not gated, so an
   owner re-asserting its plane (a pointer move, a MODE change) can
   never resurface a layer the mixer has hidden. */
static bool plane_wanted[MAX_PLANES];
static bool plane_gated[MAX_PLANES];

static void screen_plane_apply( uint32_t planeno )
{
    volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];
    bool show = plane_wanted[planeno] && !plane_gated[planeno];

    /* Never touch the display list slot of a plane that has not been
       created: at boot the context memory still belongs to the GPU
       bootloader's own list, and an uncreated slot holds skip/end
       markers that must stay intact. The wanted/gated flags are already
       recorded; creation + the owner's enable will honour them. */
    if (!plane_valid[planeno])
        return;

    if (show)
    {
        // Deliberately inverted: our direct-HVS planes only own the display
        // when HDMI is NOT connected (hotplug bit 0 clear) - with a monitor
        // attached the firmware drives the display instead
        if (~(RPI_hdmi->hotplug)&1)
            rgb->ctrl |= (uint32_t)0x40000000;
    }
    else
    {
        rgb->ctrl &= ~(uint32_t)0x40000000;
    }
}

void screen_plane_gate( uint32_t planeno, bool gated )
{
    if (planeno >= MAX_PLANES)
        return;
    plane_gated[planeno] = gated;
    screen_plane_apply(planeno);
}

void screen_plane_enable( uint32_t planeno , bool enable )
{
    LOG_DEBUG("plane %"PRIu32" %s\r\n", planeno, enable ? "enable" : "disable");
    volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];

    if (planeno < MAX_PLANES) {
        plane_wanted[planeno] = enable;
        if (!plane_valid[planeno])
            return;              /* flag recorded; slot not ours to touch */
        enable = enable && !plane_gated[planeno];
    }
    if (enable)
    {
        // Deliberately inverted: our direct-HVS planes only own the display
        // when HDMI is NOT connected (hotplug bit 0 clear) - with a monitor
        // attached the firmware drives the display instead
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

/* VP5 *VOHIGHLIGHT (AIV User Guide p.33): the player's picture is dimmed
   EXCEPT where the computer's image is non-black - the graphic is a
   stencil that spotlights the video, not a layer drawn over it. In
   highlight mode the keyed palette inverts: black entries become
   half-opaque black (they dim the video) and every colour becomes fully
   transparent (the video shows through at full brightness). */
static bool screen_highlight;

/* How far *VOHIGHLIGHT dims the video outside the computer's image, as the
   alpha of the black it lays over it: 0x80 is a half-and-half mix (only a
   2:1 brightup, which reads as "not very highlighted"), 0xC0 leaves a
   quarter of the picture for a 4:1 contrast against the windows. */
#define VP5_DIM_ALPHA 0xA0u

/* The video player dims the frame outside the computer's box by the same
   amount, in software - see dim_frame_border() there. */
uint32_t screen_highlight_alpha( void ) { return VP5_DIM_ALPHA; }

static uint32_t palette_keyed_entry( uint32_t entry, uint32_t colour )
{
    bool black = (colour == 0) && ((entry & 255u) <= 15u);
    if (screen_highlight)
        return black ? (VP5_DIM_ALPHA << 24) : 0u;
    return black ? colour : (0xff000000u | colour);
}

void screen_update_palette_entry( uint32_t entry, uint32_t r , uint32_t g , uint32_t b )
{
    // palette 0 is normal colours
    // palette 1 is flash colours
    // palette 2 is black is alpha 0 (or the VP5 highlight inverse)
    // palette 3 is flash and black is alpha 0

    uint32_t colour = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);

    context_memory[(PALETTE_BASE>>2) + entry] = 0xff000000 | colour;

    context_memory[(PALETTE_BASE>>2) + entry + (256*2)] =
        palette_keyed_entry(entry, colour);
}

/* ------------------------------------------------------------------ */
/* VP5 dim strips                                                      */
/*                                                                     */
/* *VOHIGHLIGHT dims the video wherever the computer's picture is black,
   which on a real AIV includes everything outside the computer's raster -
   the signal there is blanking.  Inside its rectangle the computer plane
   does that itself; these four planes cover the band around it.
   A plane's RECTANGLE edge is hard - only content edges inside a scaled
   plane are feathered - so each strip is a few constant source pixels
   blown up to fill its rect: a hard, pixel-exact border for 16 bytes of
   source, no filtering artefacts, and nothing written into the decoder's
   frame buffers.  Earlier attempts at one big mask plane (scaled: soft
   edge; 1:1: the HVS runs out of per-line fetch and drops the right of
   every line) and at dimming the decoded frame in software (races with
   the VideoCore still writing it - visible as streaks) are why this is
   done with plane geometry instead. */
void screen_geometry_report( uint32_t planeno, uint32_t *disp_w, uint32_t *disp_h,
                             uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h,
                             uint32_t *src_w, uint32_t *src_h );
void screen_set_palette( uint32_t planeno, uint32_t palette, uint32_t flags );

#define DIM_STRIP_FIRST  3u
#define DIM_STRIP_COUNT  4u
#define DIM_STRIP_SRC    4u     /* 4x4 constant source, scaled to the rect */

static uint32_t dim_strip_handle;
static uint32_t dim_strip_buffer;
static bool     dim_strips_on;

static void dim_strip_place(uint32_t planeno, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{
    if (!w || !h) {                       /* empty band: nothing to show */
        screen_plane_enable(planeno, false);
        return;
    }
    volatile uint32_t *plane = screen_get_nextplane(planeno);
    volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) plane;
    rgb->ctrl = 0x00000000 + (0x20<<24) + (3<<11) + 0xD;   /* 8 bit palette */
    rgb->pos = 0xFF000000 | ((y & 0xFFFu) << 12) | (x & 0xFFFu);
    rgb->scale = (h << 16) + w;
    rgb->src_size = (DIM_STRIP_SRC << 16) + DIM_STRIP_SRC;
    rgb->y_ptr = dim_strip_buffer | 0x80000000u;
    rgb->pitch = DIM_STRIP_SRC;
    rgb->palette = 0xC0000000 + PALETTE_BASE;
    rgb->LBM = screen_lbm_base(planeno);
    rgb->hpf0 = vc4_ppf(DIM_STRIP_SRC, w, x, 0);
    rgb->vpf0 = vc4_ppf(DIM_STRIP_SRC, h, y, 0);
    rgb->pfkph0 = POLYPHASE_BASE;
    rgb->pfkpv0 = POLYPHASE_BASE;
    plane_valid[planeno] = true;
    screen_set_palette(planeno, 0, 2);    /* the keyed (alpha) bank */
    screen_plane_enable(planeno, true);
}

void screen_dim_strips( bool on )
{
    if (!on) {
        if (dim_strips_on) {
            for (uint32_t i = 0; i < DIM_STRIP_COUNT; i++)
                screen_plane_enable(DIM_STRIP_FIRST + i, false);
            dim_strips_on = false;
        }
        return;
    }

    uint32_t disp_w, disp_h, x, y, w, h, sw, sh;
    screen_geometry_report(1u, &disp_w, &disp_h, &x, &y, &w, &h, &sw, &sh);
    if (!disp_w || !disp_h || !w || !h)
        return;                           /* no computer plane to frame */

    if (!dim_strip_buffer) {
        dim_strip_buffer = screen_allocate_buffer(DIM_STRIP_SRC * DIM_STRIP_SRC,
                                                  &dim_strip_handle);
        if (!dim_strip_buffer)
            return;
        /* palette 0 is black: transparent in the keyed bank normally, the
           dimming value once highlight inverts it */
        uint8_t *px = (uint8_t *)(uintptr_t)dim_strip_buffer;
        for (uint32_t i = 0; i < DIM_STRIP_SRC * DIM_STRIP_SRC; i++)
            px[i] = 0u;
    }

    uint32_t right = x + w, bottom = y + h;
    dim_strip_place(DIM_STRIP_FIRST + 0u, 0u, 0u, disp_w, y);                 /* top */
    dim_strip_place(DIM_STRIP_FIRST + 1u, 0u, bottom, disp_w,
                    (bottom < disp_h) ? (disp_h - bottom) : 0u);              /* bottom */
    dim_strip_place(DIM_STRIP_FIRST + 2u, 0u, y, x, h);                       /* left */
    dim_strip_place(DIM_STRIP_FIRST + 3u, right, y,
                    (right < disp_w) ? (disp_w - right) : 0u, h);             /* right */
    dim_strips_on = true;
}

/* /status forensics: the display size and each plane's source/destination
   rectangle, straight from the HVS display list - the only ground truth
   for overlay geometry (an HDMI capture may scale what it shows). */
void screen_geometry_report( uint32_t planeno, uint32_t *disp_w, uint32_t *disp_h,
                             uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h,
                             uint32_t *src_w, uint32_t *src_h )
{
    *disp_w = ( RPI_hvs->ctrl1 >> 12 ) & 0xfff;
    *disp_h = ( RPI_hvs->ctrl1       ) & 0xfff;
    if (planeno >= MAX_PLANES || !plane_valid[planeno]) {
        *x = *y = *w = *h = *src_w = *src_h = 0;
        return;
    }
    volatile YUV_plane_t* p = (volatile YUV_plane_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];
    *x = p->pos & 0xFFF;
    *y = (p->pos >> 12) & 0xFFF;
    *w = p->scale & 0xFFF;
    *h = (p->scale >> 16) & 0xFFF;
    *src_w = p->src_size & 0xFFF;
    *src_h = (p->src_size >> 16) & 0xFFF;
}

void screen_set_highlight( bool on );

/* Beeb reset: the mixer state (VP gates + highlight palette) returns to
   the power-on default - all layers shown, normal keying. */
void screen_mixer_reset( void )
{
    for (uint32_t planeno = 0; planeno < MAX_PLANES; planeno++)
        screen_plane_gate(planeno, false);
    screen_dim_strips(false);
    screen_set_highlight(false);
}

void screen_set_highlight( bool on )
{
    if (screen_highlight == on)
        return;
    screen_highlight = on;
    /* Recast BOTH keyed banks (normal + flash twin: entries 0-511 map to
       banks 2 and 3) from the colour banks, or the framebuffer's flash
       timer alternates the plane between an inverted and an un-inverted
       palette and the whole overlay blinks.  A VDU 19 issued in either
       mode stays consistent (screen_update_palette_entry consults the
       flag for later changes). */
    for (uint32_t entry = 0; entry < 512u; entry++) {
        uint32_t colour = context_memory[(PALETTE_BASE>>2) + entry] & 0x00FFFFFFu;
        context_memory[(PALETTE_BASE>>2) + entry + (256*2)] =
            palette_keyed_entry(entry, colour);
    }
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
    volatile rgb_8bit_t* rgb = (volatile rgb_8bit_t*) &context_memory[ (MAX_PLANES_SIZE >>2 ) * planeno + PLANE_BASE ];

    if ( (rgb->ctrl & 0xF) == 0xD)
    {
        unsigned int cpsr = _disable_interrupts_cspr();
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
        _restore_cpsr(cpsr);
    }
}

void screen_set_vsync( bool enable )
{
    if (enable)
    {
        RPI_hvs->ctrl = (RPI_hvs->ctrl &0xffff0000) | ((1 <<9) + ( 1<<2 )+ 1); // end of frame and enable IRQs
        RPI_GetIrqController()->Enable_IRQs_2 = RPI_HVS_IRQ;
    }
    else
    {
        RPI_hvs->ctrl &= (uint32_t)~( (1<<9) + 1);
        RPI_GetIrqController()->Disable_IRQs_2 = RPI_HVS_IRQ;
    }
}

/* Measured refresh rate.  The pixel clock alone cannot tell 1080p50 from
   1080p60 (both are 148.5 MHz - only the blanking differs), and the whole
   point of a 50 Hz mode here is that 25 fps video then maps to exactly two
   refreshes per frame. Counting the end-of-frame interrupts says which
   mode actually negotiated. */
static uint32_t vsync_count;
static uint32_t vsync_window_start_us;
static uint32_t vsync_window_count;
static uint32_t vsync_rate_mhz;      /* refresh in millihertz */

bool screen_check_vsync( void )
{
    if (RPI_hvs->stat & ( 1 << 16)) // check for end of frame
    {
        RPI_hvs->stat = ( 1 << 16); // clear the interrupt
        vsync_count++;
        return true;
    }
    return false;
}

/* End-of-frame count: the video player flips on a change of this, so each
   picture starts at a frame boundary. */
uint32_t screen_vsync_count( void )
{
    return vsync_count;
}

/* Refresh in millihertz, 0 until the first second has been measured. */
uint32_t screen_refresh_mhz( void )
{
    uint32_t now = RPI_GetSystemTime();
    uint32_t elapsed = now - vsync_window_start_us;
    if (elapsed >= 1000000u) {
        uint32_t frames = vsync_count - vsync_window_count;
        vsync_rate_mhz = (uint32_t)(((uint64_t)frames * 1000000000u) / elapsed);
        vsync_window_start_us = now;
        vsync_window_count = vsync_count;
    }
    return vsync_rate_mhz;
}
