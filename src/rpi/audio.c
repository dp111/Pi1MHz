#include "cache.h"
#include "systimer.h"
#include "audio.h"
#include "hdmi_audio.h"
#include "rpi.h"
#include "gpio.h"
#include "../Pi1MHz.h"   // for AUDIO_PIN
#include "../config.h"   // Audio_out
#include <string.h>

/* ------------------------------------------------------------------ */
/* A two-buffer DMA ring into a peripheral FIFO                        */
/*                                                                    */
/* Both sinks are the same machine: two control blocks chained forever,*/
/* each carrying AUDIO_DMA_FRAMES frames as word pairs; the DMA reads  */
/* them into the PWM FIFO (channel 5) or the HDMI MAI FIFO (channel 4) */
/* paced by the peripheral's DREQ; we refill whichever buffer the DMA  */
/* is not in, once it has entered the other.                           */
/* ------------------------------------------------------------------ */

struct bcm2708_dma_cb {
   uint32_t info;
   uint32_t src;
   uint32_t dst;
   uint32_t length;
   uint32_t stride;
   uint32_t next;
   uint32_t pad[2];
   uint32_t buffer[AUDIO_DMA_FRAMES_MAX * 2];   /* word pairs */
};

typedef struct {
   rpi_dmax_t *chan;                /* channel registers */
   uint32_t    chan_no;
   struct bcm2708_dma_cb *cb;       /* [2], 32-byte aligned, NOINIT */
   uint32_t    dst_bus;             /* FIFO bus address */
   uint32_t    permap;              /* DREQ source */

   /* buffer_state: bit 0/1 set = cb[0]/[1] filled and not yet played */
   uint8_t     buffer_state;
   uint32_t   *next_buffer;
   bool        was_in_first;
   uint32_t    last_handover_us;
   bool        stalled;
   uint32_t    block_period_us;

   uint32_t    blocks_played;       /* handovers seen - proves the sink is alive */
   uint32_t    underruns;           /* the DMA replayed a buffer nobody refilled */
   uint32_t    frames;              /* frames per block in use */
} dma_sink_t;

NOINIT_SECTION static __attribute__ ((aligned (0x20))) struct bcm2708_dma_cb pwm_cb[2];
NOINIT_SECTION static __attribute__ ((aligned (0x20))) struct bcm2708_dma_cb hdmi_cb[2];

static dma_sink_t pwm_sink  = { .chan = RPI_DMA5Base, .chan_no = 5, .cb = pwm_cb,  .permap = 5 };
static dma_sink_t hdmi_sink = { .chan = RPI_DMA4Base, .chan_no = 4, .cb = hdmi_cb, .permap = HDMI_AUDIO_DREQ };

static size_t dma_free_space(dma_sink_t *s)
{
   uint32_t src = s->chan->SRC_ADR;
   bool in_first = src < (((uint32_t)&s->cb[1].buffer[0]) | GPU_BASE);
   uint8_t playing_bit = in_first ? 1u : 2u;
   uint8_t other_bit   = in_first ? 2u : 1u;
   static uint32_t calls;

   if (in_first != s->was_in_first) {
      /* The DMA has moved on to the next buffer: that buffer is now being
         consumed. If nobody had refilled it since it last played, or no
         handover has been seen for longer than the two-buffer runway,
         the listener heard a replay - an underrun. */
      uint32_t now = RPI_GetSystemTime();
      s->was_in_first = in_first;
      s->blocks_played++;
      if (!s->stalled &&
          (!(s->buffer_state & playing_bit) ||
           (now - s->last_handover_us) > s->block_period_us * 5u / 2u))
         s->underruns++;
      s->last_handover_us = now;
      s->stalled = false;
      s->buffer_state &= (uint8_t)~playing_bit;
   } else if (((++calls) & 15u) == 0u && !s->stalled &&
              (RPI_GetSystemTime() - s->last_handover_us) > s->block_period_us * 5u / 2u) {
      /* No handover seen for 2.5 blocks: the main loop was away longer
         than the runway and the DMA has been replaying. The timer read is
         rate-limited so the idle loop does not pay for it. */
      s->stalled = true;
      s->underruns++;
      s->buffer_state = 0;          /* both buffers are stale now */
   }

   if (s->buffer_state & other_bit)
      return 0;                     /* other buffer filled and pending */

   s->next_buffer = in_first ? &s->cb[1].buffer[0] : &s->cb[0].buffer[0];
   return s->frames;
}

static void dma_samples_written(dma_sink_t *s)
{
   if (s->next_buffer < &s->cb[1].buffer[0])
      s->buffer_state |= 1;
   else
      s->buffer_state |= 2;
   // make sure the buffer is written out of cache
   _clean_cache_area(s->next_buffer, s->frames * 2u * sizeof(uint32_t));
}

static void dma_init_buffer(dma_sink_t *s, size_t buf, uint32_t fill)
{
   struct bcm2708_dma_cb *cb = &s->cb[buf];
   cb->info = BCM2708_DMA_PER_MAP(s->permap) | BCM2708_DMA_S_WIDTH | BCM2708_DMA_S_INC |
              BCM2708_DMA_D_DREQ | BCM2708_DMA_WAIT_RESP;
   cb->src = ((uint32_t)&cb->buffer[0]) | GPU_BASE;
   cb->dst = s->dst_bus;
   cb->length = s->frames * 2u * sizeof(uint32_t);
   cb->stride = 0;
   cb->next = (uint32_t)&s->cb[(buf + 1) % 2].info | GPU_BASE;
   cb->pad[0] = 0;
   cb->pad[1] = 0;
   for (size_t i = 0; i < s->frames * 2u; i++)
      cb->buffer[i] = fill;
   _clean_cache_area(cb, sizeof(*cb));
}

static void dma_start(dma_sink_t *s, uint32_t rate, uint32_t fill, uint32_t frames)
{
   s->frames = (frames && frames <= AUDIO_DMA_FRAMES_MAX) ? frames : AUDIO_DMA_FRAMES;
   s->buffer_state = 0;
   dma_init_buffer(s, 0, fill);
   dma_init_buffer(s, 1, fill);
   /* The DMA starts in buffer 0, so that one counts as being consumed */
   s->buffer_state = 2;
   s->was_in_first = true;
   s->block_period_us = (uint32_t)((uint64_t)s->frames * 1000000u / rate);
   s->last_handover_us = RPI_GetSystemTime();
   s->stalled = false;

   RPI_DMABase->Enable |= 1u << s->chan_no;
   s->chan->CS = BCM2708_DMA_RESET;
   usleep(10);
   s->chan->CS = BCM2708_DMA_INT | BCM2708_DMA_END;
   s->chan->ADDR = (uint32_t)&s->cb[0].info | GPU_BASE;
   s->chan->Debug = 7;              // clear debug error flags
   usleep(10);
   s->chan->CS = 0x10880000 | BCM2708_DMA_ACTIVE;  // go, mid priority, wait for outstanding writes
}

static void dma_stop(dma_sink_t *s)
{
   s->chan->CS = BCM2708_DMA_RESET;
}

/* ------------------------------------------------------------------ */
/* PWM sink (the Beeb pin / Pi jack)                                  */
/* ------------------------------------------------------------------ */

static uint32_t audio_range;   // PWM full-scale count, 0 = PWM not running
static uint32_t pwm_rate;
static bool beeb_muted;        // last state set via rpi_audio_mute_beeb()

static uint32_t pwm_frames;

static void pwm_start(uint32_t samplerate, uint32_t frames)
{
   pwm_frames = frames;
   // hardcoded constant clock rate 500MHz
   // Clock is divided by two to feed the PWM block ( 250MHz )
   audio_range = 500000000 / (2 * samplerate) ;
   pwm_rate = samplerate;

   RPI_CLKBase->PWM_CTL = PM_PASSWORD | BCM2835_PWMCLK_CNTL_KILL;
   RPI_PWMBase->PWM_CONTROL = 0;

   // samplerate = 500000000 / 2 / range

   // Bits 0..11 Fractional Part Of Divisor = 0, Bits 12..23 Integer Part Of Divisor = 2

   RPI_CLKBase->PWM_DIV = PM_PASSWORD | (0x2000);
   RPI_CLKBase->PWM_CTL = PM_PASSWORD | BCM2835_PWMCLK_CNTL_ENABLE | BCM2835_PWMCLK_CNTL_PLLD ;

   usleep(1);

   RPI_PWMBase->PWM0_RANGE = audio_range;
   RPI_PWMBase->PWM1_RANGE = audio_range;

   pwm_sink.dst_bus = ((uint32_t)(&RPI_PWMBase->PWM_FIFO) & 0x00ffffff) | PERIPHERAL_BASE_GPU;
   dma_start(&pwm_sink, samplerate, audio_range >> 1, frames);   /* mid-rail = silence */

   usleep(1);

   RPI_PWMBase->PWM_DMAC = PWMDMAC_ENAB | PWMDMAC_THRSHLD;

   // it feels as that we should have | BCM2835_PWM0_REPEATFF | BCM2835_PWM1_REPEATFF  enabled
   //but this appears to half the output frequency.  May be we need to set up PWMDMAC_THRSHLD differently
   RPI_PWMBase->PWM_CONTROL = BCM2835_PWM1_USEFIFO | BCM2835_PWM1_ENABLE |
                              BCM2835_PWM0_USEFIFO | BCM2835_PWM0_ENABLE | BCM2735_PWMx_CLRF ;
}

// Scale [-32768..32767] onto [0..audio_range] centred at mid-rail, keeping
// 16 fractional bits so the sub-step remainder can be fed back as dither
// (the PWM range is only ~10 bits, so plain truncation is audibly coarse).
static inline uint32_t pwm_pack(int32_t sample, int32_t *error)
{
   int32_t range = (int32_t)audio_range;
   int32_t acc   = sample * range + (range << 15) + *error;
   int32_t out   = acc >> 16;

   *error = acc - (out << 16);   // carry fractional part into the next sample
   if (out < 0)          { out = 0;     *error = 0; }
   else if (out > range) { out = range; *error = 0; }
   return (uint32_t)out;
}

void rpi_audio_mute_beeb(bool mute)
{
   // The Pi's PWM audio and the Beeb's own audio share AUDIO_PIN. Setting it
   // to an input (hi-Z) turns off Beeb audio; ALT0 routes PWM1 back to it.
   beeb_muted = mute;
   RPI_SetGpioPinFunction(AUDIO_PIN, mute ? FS_INPUT : FS_ALT0);
}

bool rpi_audio_beeb_muted(void)
{
   return beeb_muted;
}

/* ------------------------------------------------------------------ */
/* The ring and its owner                                             */
/* ------------------------------------------------------------------ */

/* 16384 stereo frames = 64 KB: the video player wants ~8 video frames of
   lead (15360 frames at 48 kHz); the synths use a few hundred. Power of
   two for the index masks. */
#define RING_FRAMES 16384u
NOINIT_SECTION static int16_t ring[RING_FRAMES * 2];
static uint32_t ring_wr, ring_rd;             /* monotonic frame counts */

static const audio_producer_t *owner;
static bool mute_l, mute_r;
static int32_t err_l, err_r;                  /* pwm_pack dither state */
static uint32_t hdmi_rate;

/* Which sink: PWM to the Beeb pin (default) or HDMI, driven directly
   (rpi/hdmi_audio.c). Audio_out=hdmi in Pi1MHz.cfg. If HDMI cannot be
   started (DVI link, no display) we fall back to PWM so there is always
   sound somewhere. Read once, on the first claim. */
static enum { SINK_UNSET, SINK_PWM, SINK_HDMI } sink;
static dma_sink_t *active;

static void sink_select(void)
{
   if (sink != SINK_UNSET)
      return;
   const char *p = config_get("Audio_out");
   sink = (p && strcasecmp(p, "hdmi") == 0) ? SINK_HDMI : SINK_PWM;
}

uint32_t audio_queued_frames(void)
{
   return ring_wr - ring_rd;
}

void audio_flush(void)
{
   ring_rd = ring_wr;
}

void audio_claim(const audio_producer_t *p)
{
   owner = p;
   audio_flush();
   mute_l = mute_r = false;
   err_l = err_r = 0;
   sink_select();
   uint32_t frames = p->dma_frames ? p->dma_frames : AUDIO_DMA_FRAMES;
   if (sink == SINK_HDMI) {
      if (hdmi_rate != p->rate || hdmi_sink.frames != frames) {
         if (hdmi_rate)
            dma_stop(&hdmi_sink);
         if (hdmi_audio_start(p->rate)) {
            hdmi_sink.dst_bus = hdmi_audio_fifo_bus();
            dma_start(&hdmi_sink, p->rate, 0, frames);   /* zero subframes = silence */
            hdmi_rate = p->rate;
            active = &hdmi_sink;
         } else {
            LOG_INFO("audio: HDMI unavailable, using the Beeb pin\r\n");
            sink = SINK_PWM;
         }
      }
   }
   /* The PWM block ALWAYS runs, whichever sink is selected. Hardware-proven
      2026-08-23: with the PWM never started, every FRED/JIM read from the
      Beeb returns &7F and ADFS hangs - the bus handler's shared window only
      reads once the clock manager's PWM clock (PLLD) is enabled, which
      rpi_audio_init() had been doing as a side effect for years. Enabling
      the clock alone is enough; running the whole block costs nothing and
      keeps the Beeb pin at a quiet mid-rail when HDMI has the sound. */
   if (audio_range == 0 || pwm_rate != p->rate || (sink == SINK_PWM && pwm_frames != frames))
      pwm_start(p->rate, sink == SINK_PWM ? frames : AUDIO_DMA_FRAMES);
   if (sink == SINK_PWM)
      active = &pwm_sink;
   /* The core drains the ring from the idle loop itself, whoever the
      producer is (registration is idempotent). */
   Pi1MHz_Register_Poll(audio_pump);
}

void audio_release(const audio_producer_t *p)
{
   if (owner == p) {
      owner = NULL;
      audio_flush();
   }
}

bool audio_owner_is(const audio_producer_t *p)
{
   return owner == p;
}

uint32_t audio_free_frames(void)
{
   if (!owner)
      return 0;
   uint32_t queued = ring_wr - ring_rd;
   uint32_t limit = owner->latency_frames;
   if (limit > RING_FRAMES)
      limit = RING_FRAMES;
   return queued >= limit ? 0 : limit - queued;
}

int16_t *audio_write_ptr(const audio_producer_t *p, uint32_t *contig_frames)
{
   if (p != owner) {
      *contig_frames = 0;
      return NULL;
   }
   uint32_t pos = ring_wr & (RING_FRAMES - 1u);
   uint32_t free = audio_free_frames();
   uint32_t to_wrap = RING_FRAMES - pos;
   *contig_frames = free < to_wrap ? free : to_wrap;
   return &ring[pos * 2u];
}

static int32_t peak_level;                    /* decaying |sample| max, for /status */

void audio_commit(uint32_t frames)
{
   /* A level meter is the only way to see, from the web, that a producer
      is making sound at all. ~450 compares per block - negligible. */
   uint32_t pos = ring_wr & (RING_FRAMES - 1u);
   int32_t pk = peak_level - (peak_level >> 6);  /* decay */
   for (uint32_t i = 0; i < frames; i++) {
      int32_t l = ring[pos * 2u], r = ring[pos * 2u + 1u];
      pos = (pos + 1u) & (RING_FRAMES - 1u);
      if (l < 0) l = -l;
      if (r < 0) r = -r;
      if (l > pk) pk = l;
      if (r > pk) pk = r;
   }
   peak_level = pk;
   ring_wr += frames;
}

uint32_t audio_peak(void)
{
   return (uint32_t)peak_level;
}

uint32_t audio_ring_snapshot(int16_t *dst, uint32_t max_frames)
{
   /* The most recently WRITTEN frames, in order: what the producer is
      making, whether or not any sink has played it yet. */
   if (max_frames > RING_FRAMES)
      max_frames = RING_FRAMES;
   uint32_t start = ring_wr - max_frames;
   for (uint32_t i = 0; i < max_frames; i++) {
      uint32_t pos = (start + i) & (RING_FRAMES - 1u);
      dst[i * 2u]      = ring[pos * 2u];
      dst[i * 2u + 1u] = ring[pos * 2u + 1u];
   }
   return max_frames;
}

void audio_set_channel_mute(bool left, bool right)
{
   mute_l = left;
   mute_r = right;
}

/* ------------------------------------------------------------------ */
/* Ring -> sink                                                       */
/* ------------------------------------------------------------------ */

void audio_pump_until(volatile const bool *abort)
{
   dma_sink_t *s = active;
   if (!s)
      return;

   while (dma_free_space(s)) {
      uint32_t *dst = s->next_buffer;
      uint32_t avail = ring_wr - ring_rd;
      uint32_t blk = s->frames;
      uint32_t n = avail < blk ? avail : blk;
      uint32_t pos = ring_rd & (RING_FRAMES - 1u);

      for (uint32_t i = 0; i < blk; i++) {
         int32_t l = 0, r = 0;
         if (abort && (i & 15u) == 0u && *abort) {
            /* Whoever we were waiting for has arrived: the rest of this
               block is silence (a dropout, not a desync) and the frames
               not taken stay in the ring. */
            n = i < n ? i : n;
         }
         if (i < n) {
            l = ring[pos * 2u];
            r = ring[pos * 2u + 1u];
            pos = (pos + 1u) & (RING_FRAMES - 1u);
            if (mute_l) l = 0;
            if (mute_r) r = 0;
         }
         /* past the data: silence pads the block */

         if (s == &hdmi_sink) {
            hdmi_audio_pack((int16_t)l, (int16_t)r, dst);
            dst += 2;
         } else if (beeb_muted) {
            /* pin hi-Z: L/R separately, stereo on a Pi 3B+ jack */
            *dst++ = pwm_pack(l, &err_l);
            *dst++ = pwm_pack(r, &err_r);
         } else {
            /* the Beeb pin (PWM1) is mono: it gets L+R, unhalved and
               saturated, as the Music 5000 has always summed */
            int32_t sum = l + r;
            if (sum > 32767) sum = 32767; else if (sum < -32768) sum = -32768;
            uint32_t w = pwm_pack(sum, &err_l);
            *dst++ = w;
            *dst++ = w;
         }
      }
      ring_rd += n;
      dma_samples_written(s);

      /* Nothing queued: the buffer just written is silence; do not fill
         the second one too or a producer that writes a moment later waits
         two buffers before it is heard */
      if (avail == 0 || (abort && *abort))
         break;
   }
}

void audio_pump(void)
{
   audio_pump_until(NULL);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

const char *audio_owner_name(void)
{
   return owner ? owner->name : "none";
}

uint32_t audio_rate(void)
{
   return active == &hdmi_sink ? hdmi_rate : pwm_rate;
}

const char *audio_sink_name(void)
{
   if (active == &hdmi_sink)
      return "HDMI";
   return beeb_muted ? "Beeb pin off (stereo jack)" : "Beeb pin L+R";
}

uint32_t audio_underruns(void)
{
   return active ? active->underruns : 0;
}

uint32_t audio_blocks_played(void)
{
   return active ? active->blocks_played : 0;
}
