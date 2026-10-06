/* Host stub for rpi/rpi.h and rpi/systimer.h as h264dec.c includes them;
   vchiq.h, vcsm.h, mmal_vc.h and h264dec.h are the real headers, and the
   test defines the functions they declare. */
#ifndef H264DEC_STUBS_H
#define H264DEC_STUBS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Never printed, but the arguments stay used, as on the Pi. */
#define LOG_INFO(...)  do { if (0) printf(__VA_ARGS__); } while (0)
#define LOG_DEBUG(...) do { if (0) printf(__VA_ARGS__); } while (0)

uint32_t RPI_GetSystemTime(void);

#endif
