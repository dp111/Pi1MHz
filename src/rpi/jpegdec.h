#ifndef JPEGDEC_H
#define JPEGDEC_H

#include <stdint.h>
#include <stddef.h>

/*
 * Fast Software Baseline JPEG / MJPEG Decoder
 * Optimised for ARM1176JZF-S (Raspberry Pi 1).
 *
 * Decodes an MJPEG frame into planar YCbCr 4:2:2 at 768x576.
 *
 * Output layout (matches VideoCore HVS YUV plane):
 *   Y  plane: 768 * 576 bytes  (offset 0)
 *   Cr plane: 384 * 576 bytes  (offset 768*576)
 *   Cb plane: 384 * 576 bytes  (offset 768*576 + 384*576)
 */

#define JPEG_WIDTH   768
#define JPEG_HEIGHT  576

/* Initialise the JPEG decoder. Call once at startup. */
void jpegdec_init(void);

/*
 * Decode a baseline JPEG / MJPEG frame.
 *
 * jpeg_data   - pointer to the JPEG/JFIF frame in memory
 * jpeg_size   - size of the JPEG frame in bytes
 * yuv_output  - pointer to the output YUV buffer (ARM-accessible address)
 *               Must hold at least JPEG_WIDTH*JPEG_HEIGHT*2 bytes.
 *
 * Returns 0 on success, non-zero on error.
 */
int jpegdec_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                   uint8_t *yuv_output);

#endif /* JPEGDEC_H */
