/*
    h264dec.h - hardware H264 decoder ("ril.video_decode") for bare metal

    Wraps the MMAL video_decode component into a small streaming API sized
    for the Pi1MHz video player: fixed 768x576, all-intra Annex-B H264
    (every access unit is SPS+PPS+IDR, prepared offline by
    tools/make_pvf.py), output as planar I420 written by the VideoCore
    directly into caller-supplied VC-heap buffers - which can be the very
    buffers the HVS is scanning out, so the ARM never touches a pixel.

    Usage:
      h264dec_init(768, 576, my_frame_cb);
      h264dec_add_output_buffer(phys_a, FRAME);             // double buffer
      h264dec_add_output_buffer(phys_b, FRAME);
      loop:
        p = h264dec_get_input_buffer(&max);       // NULL = both in flight
        ...read one access unit from SD into p (uncached VC heap)...
        h264dec_submit_input(len, pts, false);
        h264dec_poll();                           // pumps everything
      my_frame_cb(phys, ...) fires per decoded frame; display it, then
      h264dec_recycle_output(phys) once the previous one is off screen.

    All callbacks run inside h264dec_poll() - keep them trivial (record
    state, no MMAL calls).
*/

#ifndef RPI_H264DEC_H
#define RPI_H264DEC_H

#include <stdint.h>
#include <stdbool.h>

#define H264DEC_INPUT_BUFFERS   2
#define H264DEC_INPUT_BUF_SIZE  (512u * 1024u)
#define H264DEC_MAX_OUTPUT      3

/* A decoded frame is ready in the buffer at 'phys' (I420: Y plane
   width*height, then U then V at quarter size each). 'eos' marks the
   drain marker after an end-of-stream was submitted. */
typedef void (*h264dec_frame_cb)(uint32_t phys, int64_t pts, bool eos);

/* Create and start the decoder. False if the firmware lacks MMAL/SMEM
   (start_cd.elf) or the component cannot start - callers should fall back
   to the still-frame player. Frames are delivered zero-copy: the
   component decodes straight into the caller's buffers and only a memory
   handle crosses VCHIQ.

   NOTE: a missing vd_use_vpu0=1 is NOT reported here. Every call still
   succeeds; the component simply never issues a decode, so the frame
   count on /status stays 0 and no picture appears. */
bool h264dec_init(uint32_t width, uint32_t height, h264dec_frame_cb cb);

/* Register a VC-heap output buffer of at least width*height*3/2 bytes
   (from screen_allocate_buffer or vchiq_alloc_shared) as somewhere the
   decoder may write a picture. Call 2-3 times. */
bool h264dec_add_output_buffer(uint32_t phys, uint32_t size);

/* Hand a buffer back after its frame has been displayed/replaced. */
void h264dec_recycle_output(uint32_t phys);

/* Borrow a free input staging buffer (VC heap, uncached - read file data
   straight into it). NULL while both are in flight. */
uint8_t *h264dec_get_input_buffer(uint32_t *max_size);

/* Submit 'length' bytes previously written into the borrowed staging
   buffer as one complete access unit. When 'eos' is set a zero-length
   end-of-stream marker follows, forcing the decoder to flush out the
   frame - used for stills; see h264dec_resume() to continue after it. */
bool h264dec_submit_input(uint32_t length, int64_t pts, bool eos);

/* Give back a borrowed input buffer without submitting anything (e.g. a
   failed SD read). */
void h264dec_cancel_input(void);

/* Flush both ports - after an EOS still, or on a seek during playback,
   this discards anything in flight so the next submit starts clean. */
bool h264dec_resume(void);

/* Warm restart (e.g. Beeb reset re-running emulator inits): return all
   buffers, forget output registrations (so the caller may free its frame
   buffers), keep the component alive ready for new input. After this,
   re-register output buffers with h264dec_add_output_buffer(). */
void h264dec_reset(void);

/* Pump the decoder; call from the player poll task. */
void h264dec_poll(void);

/* Diagnostics (shown on /status) */
bool h264dec_running(void);
/* /status: output-port enable, pending reconfigure, registered/armed buffers */
void h264dec_output_state(bool *enabled, bool *reconf, uint32_t *registered, uint32_t *armed);
uint32_t h264dec_frames_decoded(void);

#endif /* RPI_H264DEC_H */
