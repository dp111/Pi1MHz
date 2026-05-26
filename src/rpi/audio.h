
#ifndef _BCM2708_AUDIO_H
#define _BCM2708_AUDIO_H

#include "base.h"
#include <stddef.h>

// NB Buffer size must be a multiple of 32 bytes
// buffer contains both left and right sample words
// @46875Hz
// buffer of 2048 21.84ms
// buffer of 1024 10.92ms
// buffer of  512  5.46ms
// buffer of  256  2.73ms

// Music 5000 updates @ 10mS
// so we ideally want to be <5ms so lets choose a buffer of 448 with is 4.77mS

// NB b-em has a buffer of 1500 which is a delay of 32ms
#define DMA_BUFFER_SIZE 448

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

#define BCM2835_PWM1_MS_MODE    0x8000  /*  Run in MS mode                   */
#define BCM2835_PWM1_USEFIFO    0x2000  /*  Data from FIFO                   */
#define BCM2835_PWM1_REVPOLAR   0x1000  /* Reverse polarity             */
#define BCM2835_PWM1_OFFSTATE   0x0800  /* Ouput Off state             */
#define BCM2835_PWM1_REPEATFF   0x0400  /* Repeat last value if FIFO empty   */
#define BCM2835_PWM1_SERIAL     0x0200  /* Run in serial mode             */
#define BCM2835_PWM1_ENABLE     0x0100  /* Channel Enable             */

#define BCM2735_PWMx_CLRF       0x0040  /* clear FIFO */

#define BCM2835_PWM0_MS_MODE    0x0080  /* Run in MS mode             */
#define BCM2835_PWM0_USEFIFO    0x0020  /* Data from FIFO             */
#define BCM2835_PWM0_REVPOLAR   0x0010  /* Reverse polarity             */
#define BCM2835_PWM0_OFFSTATE   0x0008  /* Ouput Off state             */
#define BCM2835_PWM0_REPEATFF   0x0004  /* Repeat last value if FIFO empty   */
#define BCM2835_PWM0_SERIAL     0x0002  /* Run in serial mode             */
#define BCM2835_PWM0_ENABLE     0x0001  /* Channel Enable             */

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
#define BCM2708_DMA_ISPAUSED  (1 << 4)  /* Pause requested or not active */
#define BCM2708_DMA_ISHELD (1 << 5)  /* Is held by DREQ flow control */
#define BCM2708_DMA_ERR    (1 << 8)
#define BCM2708_DMA_ABORT  (1 << 30) /* stop current CB, go to next, WO */
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

size_t rpi_audio_buffer_free_space();
uint32_t * rpi_audio_buffer_pointer();
void rpi_audio_samples_written();
uint32_t rpi_audio_init(uint32_t samplerate );

#endif
