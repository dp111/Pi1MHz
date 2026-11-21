#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <stdint.h>
#include <stdbool.h>

void videoplayer_init(uint8_t instance, uint8_t address);
void videoplayer_vsync_flip(void);   /* vsync IRQ: commit the armed frame */

/* ---- LaserDisc-player control surface, driven by the F-codes ---------- */

/* True when a .pvf video is open and the hardware decoder is running -
   which says nothing about a decoded frame: the video plane is the
   player's own and comes on only with its first real one. When false
   there is no disc picture: ?F answers X and the jumps do nothing. */
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

/* Chapters, from the side's chapters.txt (src/chapters.h).  Chapter
   numbers are the disc's (0..79); pictures are 1-based, as for goto. */
bool videoplayer_has_chapters(void);                              /* ?P */
bool videoplayer_chapter_first(uint32_t chapter, uint32_t *picture); /* Qxx */
/* The chapter's last picture, for QxxS's stop register; the last chapter
   gives UINT32_MAX, past the disc's end, which the register clamps to it. */
bool videoplayer_chapter_last(uint32_t chapter, uint32_t *picture);
int  videoplayer_chapter_at(uint32_t picture);   /* ?C: chapter, or -1 */
/* F-codes C0/C1: the chapter number display (C1 turns D's off) */
void videoplayer_show_chapter_number(bool on);

/* F-codes A0/A1 (channel 0) and B0/B1 (channel 1) */
void videoplayer_audio_enable(int channel, bool on);

void videoplayer_set_video(bool on);        /* F-codes E0 / E1  */
bool videoplayer_take_stop_reached(void);  /* stop register hit, once */
/* Current picture number for the ?F status request (0 = unknown) */
uint32_t videoplayer_picture_number(void);

/* kernel.now, just before the jump (chainboot.c): close the player, take
   the decoder down and hand the VideoCore connection to the next kernel.
   Never fails the jump; true if the VideoCore let go of everything. */
bool videoplayer_shutdown(void);

/* One-line state dump for /status */
const char *videoplayer_status(void);

#endif
