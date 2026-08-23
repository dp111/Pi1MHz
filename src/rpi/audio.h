
#ifndef _BCM2708_AUDIO_H
#define _BCM2708_AUDIO_H

#include "base.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// PWM DMA buffer, in stereo frames (one frame = one PWM0 word + one PWM1
// word). Must be a multiple of 2 frames (init code) and of 8 words (DMA
// burst). Two buffers ring, so total runway is twice this.
//
//   @48000Hz   256 frames = 5.33ms
//   @46875Hz   224 frames = 4.78ms  (the Music 5000 updates every 10ms;
//                                    well under 5ms keeps it responsive)
#define AUDIO_DMA_FRAMES 224
/* The largest block a producer may ask for (dma_frames below) */
#define AUDIO_DMA_FRAMES_MAX 1024

#define PWM_BASE          (PERIPHERAL_BASE + 0x20C000) /* PWM controller */
#define CLOCK_BASE        (PERIPHERAL_BASE + 0x101000)

#define DMA_CONTROLLER_BASE (PERIPHERAL_BASE + 0x007000)

typedef struct
{
   rpi_reg_rw_t PWM_CONTROL;
   rpi_reg_rw_t PWM_STATUS;
   rpi_reg_rw_t PWM_DMAC;
   rpi_reg_ro_t reserved1;
   rpi_reg_rw_t PWM0_RANGE;
   rpi_reg_rw_t PWM0_DATA;
   rpi_reg_rw_t PWM_FIFO;
   rpi_reg_ro_t reserved2;
   rpi_reg_rw_t PWM1_RANGE;
   rpi_reg_rw_t PWM1_DATA;
} rpi_pwm_t;

static rpi_pwm_t* const RPI_PWMBase = (rpi_pwm_t*) PWM_BASE;

typedef struct
{
   rpi_reg_ro_t reserved1[40];
   rpi_reg_rw_t PWM_CTL;
   rpi_reg_rw_t PWM_DIV;
} rpi_clk_t;

static rpi_clk_t* const RPI_CLKBase = (rpi_clk_t*) CLOCK_BASE;

#define PM_PASSWORD 0x5A000000

#define BCM2835_PWMCLK_CNTL_OSCILLATOR 0x01
#define BCM2835_PWMCLK_CNTL_PLLA 0x04
#define BCM2835_PWMCLK_CNTL_PLLD 0x06
#define BCM2835_PWMCLK_CNTL_KILL (1<<5)
#define BCM2835_PWMCLK_CNTL_ENABLE (1<<4)

#define PWMDMAC_ENAB (1UL<<31)
#define PWMDMAC_THRSHLD ((4<<8)|(4<<0))

#define BCM2835_PWM1_MS_MODE    0x8000  /* Run in MS mode                  */
#define BCM2835_PWM1_USEFIFO    0x2000  /* Data from FIFO                  */
#define BCM2835_PWM1_REVPOLAR   0x1000  /* Reverse polarity                */
#define BCM2835_PWM1_OFFSTATE   0x0800  /* Output Off state                */
#define BCM2835_PWM1_REPEATFF   0x0400  /* Repeat last value if FIFO empty */
#define BCM2835_PWM1_SERIAL     0x0200  /* Run in serial mode              */
#define BCM2835_PWM1_ENABLE     0x0100  /* Channel Enable                  */

#define BCM2735_PWMx_CLRF       0x0040  /* clear FIFO                      */

#define BCM2835_PWM0_MS_MODE    0x0080  /* Run in MS mode                  */
#define BCM2835_PWM0_USEFIFO    0x0020  /* Data from FIFO                  */
#define BCM2835_PWM0_REVPOLAR   0x0010  /* Reverse polarity                */
#define BCM2835_PWM0_OFFSTATE   0x0008  /* Output Off state                */
#define BCM2835_PWM0_REPEATFF   0x0004  /* Repeat last value if FIFO empty */
#define BCM2835_PWM0_SERIAL     0x0002  /* Run in serial mode              */
#define BCM2835_PWM0_ENABLE     0x0001  /* Channel Enable                  */

#define BCM2835_BERR  0x100
#define BCM2835_GAPO4 0x80
#define BCM2835_GAPO3 0x40
#define BCM2835_GAPO2 0x20
#define BCM2835_GAPO1 0x10
#define BCM2835_RERR1 0x8
#define BCM2835_WERR1 0x4
#define BCM2835_EMPT1 0x2
#define BCM2835_FULL1 0x1

/* DMA CS Control and Status bits */
#define BCM2708_DMA_ACTIVE (1 << 0)
#define BCM2708_DMA_INT    (1 << 2)
#define BCM2708_DMA_ISPAUSED  (1 << 4) /* Pause requested or not active */
#define BCM2708_DMA_ISHELD (1 << 5)    /* Is held by DREQ flow control */
#define BCM2708_DMA_ERR    (1 << 8)
#define BCM2708_DMA_ABORT  (1 << 30)   /* stop current CB, go to next, WO */
#define BCM2708_DMA_RESET  (1UL << 31) /* WO, self clearing */

/* DMA control block "info" field bits */
#define BCM2708_DMA_INT_EN (1 << 0)
#define BCM2708_DMA_TDMODE (1 << 1)
#define BCM2708_DMA_WAIT_RESP (1 << 3)
#define BCM2708_DMA_D_INC  (1 << 4)
#define BCM2708_DMA_D_WIDTH   (1 << 5)
#define BCM2708_DMA_D_DREQ (1 << 6)
#define BCM2708_DMA_S_INC  (1 << 8)
#define BCM2708_DMA_S_WIDTH   (1 << 9)
#define BCM2708_DMA_S_DREQ (1 << 10)

#define  BCM2708_DMA_BURST(x) (((x)&0xf) << 12)
#define  BCM2708_DMA_PER_MAP(x)  ((x) << 16)
#define  BCM2708_DMA_WAITS(x) (((x)&0x1f) << 21)

#define BCM2708_DMA_DREQ_EMMC 11
#define BCM2708_DMA_DREQ_SDHOST  13

typedef struct
{
   rpi_reg_rw_t CS;
   rpi_reg_rw_t ADDR;// write address of a bcm2708_dma_cb here
/* the current control block appears in the following registers - read only */
   rpi_reg_ro_t INFO;
   rpi_reg_ro_t SRC_ADR;
   rpi_reg_ro_t DES_ADR;
   rpi_reg_ro_t TX_LEN;
   rpi_reg_ro_t STRIDE;
   rpi_reg_ro_t NEXTCB;
   rpi_reg_rw_t Debug;
} rpi_dmax_t;

static rpi_dmax_t* const RPI_DMA4Base = (rpi_dmax_t*) (DMA_CONTROLLER_BASE + 0x400);

static rpi_dmax_t* const RPI_DMA5Base = (rpi_dmax_t*) (DMA_CONTROLLER_BASE + 0x500);

typedef struct
{
   rpi_reg_rw_t Int_Status;
   rpi_reg_ro_t reserved1[3];
   rpi_reg_rw_t Enable;
} rpi_dma_t;

static rpi_dma_t* const RPI_DMABase = (rpi_dma_t*) (DMA_CONTROLLER_BASE + 0xFE0);

#define BCM2708_DMA_TDMODE_LEN(w, h) ((h) << 16 | (w))

// Missing from original kernel file:
#define BCM2708_DMA_END             (1<<1 )
#define BCM2708_DMA_NO_WIDE_BURSTS  (1<<26)

/* ---------------------------------------------------------------------
   Audio core: one producer at a time writes int16 stereo PCM into a ring;
   audio_pump() drains the ring into the active sink (PWM today; HDMI is
   the audio plan's next step). Producers never see the sink's format.

   The Beeb pin (PWM1, GPIO13) always carries L+R summed. With
   BeebAudio_Off=1 the pin is hi-Z and L/R go out separately, which on a
   Pi 3B+ is stereo on the headphone jack - the same rule the Music 5000
   used to apply itself.
   --------------------------------------------------------------------- */

typedef struct {
   uint32_t rate;             /* Hz - the producer's native rate          */
   uint32_t latency_frames;   /* how far ahead of the sink it may write;
                                 audio_free_frames() never offers more     */
   const char *name;          /* for /status                              */
   uint32_t dma_frames;       /* DMA block size to run the sink with, 0 =
                                 AUDIO_DMA_FRAMES. A synth wants small
                                 blocks (latency); a player with a deep
                                 ring wants big ones (SD latency spikes) */
} audio_producer_t;

/* Become the producer. Supersedes whoever held it: their audio_write_ptr()
   returns NULL from now on. Re-programs the sink if the rate changed. */
void audio_claim(const audio_producer_t *p);
/* Give it back (no-op unless p is the owner). The sink keeps running. */
void audio_release(const audio_producer_t *p);
bool audio_owner_is(const audio_producer_t *p);

/* Frames the owner may write now - ring space capped by latency_frames. */
uint32_t audio_free_frames(void);
/* Pointer to the next write position and how many frames fit before the
   ring wraps (<= audio_free_frames()). NULL if not the owner. Write
   interleaved L,R int16 pairs, then audio_commit(). */
int16_t *audio_write_ptr(const audio_producer_t *p, uint32_t *contig_frames);
void audio_commit(uint32_t frames);
/* Discard everything queued (a seek). */
void audio_flush(void);
uint32_t audio_queued_frames(void);

/* Mute a channel at the output, without affecting what is queued - the
   LaserDisc A/B soundtrack switches, which must stay in sync when
   re-enabled. Both false by default. */
void audio_set_channel_mute(bool left, bool right);

/* Move PCM from the ring into the sink. Called from the idle poll; safe
   to call from anywhere that is merely spinning (it touches no FatFs and
   no producer state). */
void audio_pump(void);
/* As audio_pump, but gives up as soon as *abort becomes true (checked
   every few samples), padding the block with silence. For use inside a
   wait where the thing being waited for must be answered within a
   microsecond or two once it arrives. */
void audio_pump_until(volatile const bool *abort);

/* Mute (disconnect) or restore the Pi's PWM feed on the shared audio pin.
   mute==true sets the pin hi-Z (turns off Beeb audio, as M5000 does);
   mute==false routes PWM1 back to the pin. Set once at config time. */
void rpi_audio_mute_beeb(bool mute);
bool rpi_audio_beeb_muted(void);

/* Diagnostics for /status */
const char *audio_owner_name(void);
const char *audio_sink_name(void);
uint32_t audio_rate(void);
uint32_t audio_underruns(void);
uint32_t audio_peak(void);        /* recent |sample| maximum, 0..32767 */
/* Copy the last max_frames written to the ring (interleaved L/R) into
   dst; returns frames copied. For /audio.wav. */
uint32_t audio_ring_snapshot(int16_t *dst, uint32_t max_frames);
uint32_t audio_blocks_played(void);

#endif
