#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <stdint.h>
#include <stdbool.h>

void videoplayer_init(uint8_t instance, uint8_t address);

/* ---- LaserDisc-player control surface, driven by the F-codes ---------- */

/* True when a .pvf video is open and the hardware decoder is running.
   When false the F-code layer keeps its old (static frame) behaviour. */
bool videoplayer_active(void);

/* Fxxxxx<op>: op = 'R' goto & still, 'N' goto & play, 'Q' goto & resume
   previous mode, 'S' load stop register, 'I' load info register.
   Pictures are 1-based, as on the disc. */
void videoplayer_goto(uint32_t picture, char op);

void videoplayer_play_fwd(void);     /* F-code 'N' bare  */
void videoplayer_play_rev(void);     /* F-code 'O'       */
void videoplayer_halt(void);         /* F-code '*'       */
void videoplayer_pause(void);        /* F-code '/' (halt + mute) */
void videoplayer_step(int delta);    /* F-codes 'L'/'M'  */

/* F-codes A0/A1 (channel 0) and B0/B1 (channel 1) */
void videoplayer_audio_enable(int channel, bool on);

/* Current picture number for the ?F status request (0 = unknown) */
uint32_t videoplayer_picture_number(void);

#endif
