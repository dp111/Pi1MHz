/*
    hdmi_audio.h - HDMI audio on VideoCore IV, driven directly from the ARM.

    The firmware's own audio service (AUDS over VCHIQ) cannot be used:
    its renderer runs on VPU core 1, the core Pi1MHz's 1MHz bus handler
    occupies, and there is no config.txt option to move it (see
    docs/dev/audio-plan.md). So this programs the HDMI MAI (audio input)
    block itself and the audio core DMAs IEC958 subframes into its FIFO,
    exactly as the PWM sink DMAs words into the PWM FIFO.

    Recipe after Circle's CHDMISoundBaseDevice (GPLv3, R. Stange) and the
    Linux vc4_hdmi driver: MAI sample clock as a rational of the HSM
    clock, N/CTS from the pixel clock read back from PLLH, the audio
    infoframe via the packet RAM, IEC958 consumer subframes with a B
    preamble every 192 frames.

    Requires the display to be in HDMI (not DVI) mode: hdmi_drive=2 in
    config.txt when the sink's EDID does not advertise audio (capture
    dongles). Pi 1 / Zero / 2 / 3 only (VC4); the Pi 4 HDMI block differs.
*/

#ifndef RPI_HDMI_AUDIO_H
#define RPI_HDMI_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/* Program the MAI for 2-channel PCM at 'rate' and enable it. False if
   the firmware has not brought HDMI up with packets enabled (DVI mode or
   no display). The DMA feed is the caller's (audio.c). */
bool hdmi_audio_start(uint32_t rate);
void hdmi_audio_stop(void);

/* Bus address of the MAI data FIFO, for the DMA destination */
uint32_t hdmi_audio_fifo_bus(void);
#define HDMI_AUDIO_DREQ 17

/* Convert one stereo frame to two IEC958 subframes, advancing the
   192-frame block counter. Samples are 16-bit. */
void hdmi_audio_pack(int16_t l, int16_t r, uint32_t *out);

/* Diagnostics */
uint32_t hdmi_audio_hsm_hz(void);
uint32_t hdmi_audio_pixel_hz(void);
uint32_t hdmi_audio_mai_ctl(void);

#endif /* RPI_HDMI_AUDIO_H */
