/*
    auds.h - the firmware's audio service ("AUDS") over VCHIQ: HDMI (or the
    Pi 3 headphone jack) output for bare metal.

    This is what Raspberry Pi OS's bcm2835-audio ALSA driver speaks. We
    hand the VideoCore int16 stereo PCM in chunks (a WRITE message followed
    by a bulk transfer of the samples) and the firmware does the rest:
    the HDMI MAI block, N/CTS clock regeneration, audio infoframes, and
    re-initialisation on hotplug. It answers each chunk with COMPLETE as
    it is consumed, which is our flow control.

    Needs the full start.elf (start_cd.elf has no VCHIQ). The sink is
    selected with Audio_out=hdmi in Pi1MHz.cfg; see docs/dev/audio-plan.md.

    Wire format from Linux drivers/staging/vc04_services/bcm2835-audio/
    vc_vchi_audioserv_defs.h (GPL-2.0); this is an independent
    reimplementation, like vchiq.c and vcsm.c.
*/

#ifndef RPI_AUDS_H
#define RPI_AUDS_H

#include <stdint.h>
#include <stdbool.h>

/* Where the firmware should route the sound */
#define AUDS_DEST_AUTO       0
#define AUDS_DEST_HEADPHONES 1
#define AUDS_DEST_HDMI       2

/* Bring the service up and configure it: false if there is no VCHIQ
   (start_cd.elf) or the firmware refuses. Safe to call again to change
   the rate; the stream is stopped and restarted. */
bool auds_start(uint32_t rate, uint32_t channels, int dest);
void auds_stop(void);
bool auds_running(void);

/* Largest single auds_write() */
#define AUDS_CHUNK_FRAMES_MAX 512u

/* Frames the service will accept right now (whole chunks free). */
uint32_t auds_free_frames(void);
/* Hand over up to one chunk of interleaved int16 stereo. Returns the
   frames actually taken (0 = no chunk free / could not send). */
uint32_t auds_write(const int16_t *pcm, uint32_t frames);

/* Pump VCHIQ and retire completed chunks. Call from the audio pump. */
void auds_poll(void);

/* Diagnostics */
uint32_t auds_chunks_sent(void);
uint32_t auds_bytes_completed(void);
uint32_t auds_completes(void);
uint32_t auds_bytes_sent(void);

#endif /* RPI_AUDS_H */
