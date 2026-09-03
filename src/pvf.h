/*
    pvf.h - "Pi Video File" container for the Pi1MHz hardware video player

    A deliberately dumb, seekable container produced offline by
    tools/make_pvfv2.py. Goals: one SD seek per random-accessed frame,
    fixed-cost index lookups, zero parsing on the Pi.

    Layout (all little-endian):

        [pvf_header_t                      64 bytes ]
        [frame index: frame_count x u32    file offset of each record ]
        [records, 4-byte aligned:
            pvf_record_t { video_len, audio_len }
            video access unit  (complete Annex-B AU: SPS+PPS+IDR,
                                padded to 4 bytes)
            audio PCM          (audio_len bytes, s16le interleaved
                                stereo at audio_rate, the samples that
                                accompany this frame)
        ]

    Every access unit is a self-contained IDR picture (keyint=1), so ANY
    record can be decoded with no other data - that is what makes
    LaserDisc-style random access and freeze frame trivial.

    The audio is pre-resampled by the tool to 46875 Hz, the native rate
    of rpi/audio.c's PWM path, so playback needs no rate conversion:
    at 25 fps each record carries exactly 1875 stereo samples (7500
    bytes).

    32-bit offsets cap a file at 4 GB, which is also the FAT32 limit; a
    Domesday side (54000 frames) comes to roughly 1.5-2.5 GB.
*/

#ifndef PI1MHZ_PVF_H
#define PI1MHZ_PVF_H

#include <stdint.h>

#define PVF_MAGIC   0x31465650u      /* "PVF1" */
#define PVF_VERSION 1u

typedef struct {
    uint32_t magic;                  /* PVF_MAGIC */
    uint32_t version;                /* PVF_VERSION */
    uint32_t width;                  /* 832 (768 before make_pvfv2.py) */
    uint32_t height;                 /* 576 */
    uint32_t fps_num;                /* 25 */
    uint32_t fps_den;                /* 1 */
    uint32_t frame_count;
    uint32_t audio_rate;             /* 46875, or 0 = no audio */
    uint32_t audio_channels;         /* 2 */
    uint32_t audio_bytes_per_frame;  /* nominal: 1875 * 2ch * 2B = 7500 */
    uint32_t index_offset;           /* file offset of the u32 index */
    uint32_t data_offset;            /* file offset of the first record */
    uint32_t max_video_len;          /* largest AU, for buffer sizing */
    /* Pixel aspect ratio, added by tools/make_pvfv2.py.  832x576 is the
       Beeb's own sampling grid (52 us of PAL active line at its 16 MHz
       pixel clock), so the two rasters register pixel-for-pixel - but it
       is not square-pixel: a 4:3 picture over 832x576 has PAR 12/13.
       0/0 (every file written before this existed) means unspecified,
       which the player reads as square. */
    uint32_t par_num;
    uint32_t par_den;
    uint32_t reserved[1];
} pvf_header_t;

_Static_assert(sizeof(pvf_header_t) == 64, "pvf header size");

typedef struct {
    uint32_t video_len;              /* AU bytes (unpadded) */
    uint32_t audio_len;              /* PCM bytes following the AU */
} pvf_record_t;

#endif /* PI1MHZ_PVF_H */
