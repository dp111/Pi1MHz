#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <stdint.h>
#include <stdbool.h>

void videoplayer_init(uint8_t instance, uint8_t address);
void videoplayer_vsync_flip(void);   /* vsync IRQ: commit the armed frame */

/* ---- LaserDisc-player control surface, driven by the F-codes ---------- */

/* True when a .pvf video is open and the hardware decoder is running.
   When false the F-code layer keeps its old (static frame) behaviour. */
bool videoplayer_active(void);
bool videoplayer_seeking(void);            /* ?P: goto outstanding */
bool videoplayer_audio_enabled(int channel);

/* The VFS jukebox directory changed or the card was remounted: reopen
   /BeebVFS<n>/video.pvf from the poll task. Safe from any context. */
void videoplayer_media_changed(void);

/* Fxxxxx<op>: op = 'R' goto & still, 'N' goto & play, 'Q' goto & resume
   previous mode, 'S' load stop register, 'I' load info register.
   Pictures are 1-based, as on the disc. */
void videoplayer_goto(uint32_t picture, char op);

void videoplayer_play_fwd(void);     /* F-code 'N' bare  */
void videoplayer_play_rev(void);     /* F-code 'O'       */
void videoplayer_halt(void);         /* F-code '*'       */
void videoplayer_pause(void);        /* F-code '/' (halt + mute) */
void videoplayer_step(int delta);    /* F-codes 'L'/'M'  */

/* F-code 'SxxxF' / 'SxxxS' (VP415 manual): two registers. Fast: 2..40,
   speed = xxx/2 x normal. Slow: 2..250, speed = 2/xxx x normal (250 =
   5 s per picture). */
void videoplayer_speed(uint32_t value, bool fast);
void videoplayer_slow_fwd(void);     /* F-code 'U'       */
void videoplayer_slow_rev(void);     /* F-code 'V' bare  */
void videoplayer_fast_fwd(void);     /* F-code 'W'       */
void videoplayer_fast_rev(void);     /* F-code 'Z'       */
void videoplayer_clear(void);        /* F-code 'X': stop/info registers */

/* F-codes D0/D1: the player's own picture-number display */
void videoplayer_show_picture_number(bool on);

/* F-codes A0/A1 (channel 0) and B0/B1 (channel 1) */
void videoplayer_audio_enable(int channel, bool on);

/* Current picture number for the ?F status request (0 = unknown) */
void videoplayer_set_video(bool on);        /* F-codes E0 / E1  */
bool videoplayer_take_stop_reached(void);  /* stop register hit, once */
uint32_t videoplayer_picture_number(void);

/* One-line state dump for /status */
const char *videoplayer_status(void);

#endif
