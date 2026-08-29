// Music 5000 emulation for B-Em
// Copyright (C) 2016 Darren Izzard
//
// based on the Music 5000 code from Beech
//
// Beech - BBC Micro emulator
// Copyright (C) 2015 Darren Izzard
//
// This program is free software; you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110 - 1301 USA.

// The code has been simplified to reduce the processor overheads of emulation

/*
Internal Waveform description
Use https://wavedrom.com/editor.html

{signal: [
  {name: 'clk',  wave: 'hlhlhlhlhlhlhlhlhlhlhlhlhlhlhlhlhlhlhlhlhlh.........................................'},
  {name: 'addr0',wave: 'l.h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.'},
  {name: 'addr1',wave: 'l...h...l...h...l...h...l...h...l...h...l...h'},
  {name: 'addr2',wave: 'l.......h.......l.......h.......l.......h.......'},
  {name: 'pa1'  ,wave: 'h.l...h...l...h...l...h...l...h...l...h...l...h'},
  {name: 'pa2'  ,wave: 'h.l.......h.......l.......h.......l.......h.......'},
  {name: 'nS0'  ,wave: 'l.h.............l.h............................'},
  {name: 'nS1'  ,wave: 'h.l.h............................................'},
  {name: 'nS4'  ,wave: 'h.......l.h............................................'},
  {name: 'nS6'  ,wave: 'h...........l.h............................................'},
  {name: 'nS7'  ,wave: 'h.............l.h............................................'},
  {name: 'ram addr', wave: 'x.3.4.3.4.3.4.3.4.3.', data: ['Freqlo', 'x4', 'Freqmid', 'waveform','FreqHigh','Amplitude','wavedata','Control']},
  {name: 'adder A', wave: 'x...3.4.3.4.3.4.3.4.3.', data: ['Freqlo', 'x4', 'Freqmid', 'waveform','FreqHigh','Amplitude','wavedata','Control']},

  {name: 'CY4'  ,wave: 'l.......................................................'},
  {name: 'CY4d' ,wave: 'l.......................................................'},
  {name: 'C4D'  ,wave: 'l.......................................................'},
  {name: 'nSx'  ,        wave: 'h..........................................................'},
  {name: 'phase noe'    ,wave: 'h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.l.h.'},
  {name: 'phase ICB4b 4',wave: 'h...l.h.l.h.l.h.....l.h.l.h.l.h.l.h.'},
  {name: 'phase nWE ',   wave: '.h...lh..lh..lh......lh.l.h.l.h.l.h.l.h.'},
  {name: 'phase addr',   wave: 'h.3...4...3...4...3...4.3.4.3.', data: ['0', '1', '2', '3','0','Amplitude','x3','Control']},
  {name: 'phase IC22mm',  wave: '.x...3x..3x..3xx..3...4.3.4.3.', data: ['sphl', 'sphm', 'sphh', 'amp','0','Amplitude','x3','Control']},
  {name: 'phase ramop',  wave: 'x.3.x.3.x.3.x.3.x.3...4.3.4.3.', data: ['phlo', 'phmid', 'phhigh', 'amp','0','Amplitude','x3','Control']},

  {name: 'adder B', wave: 'x.0.3.0.3.0.3.0.3.0..3...4.3.4.3.', data: ['phlo', 'phmid', 'phhigh', 'amp','0','Amplitude','x3','Control']},

  {name: 'sum bus', wave: 'x.0.3.0.3.0.3.0.3.0..3...4.3.4.3.', data: ['sphlo', 'sphmid', 'sphhigh', 'saudio','0','Amplitude','x3','Control']},


  {name: 'CY4'  ,    wave: 'l............h.......................................................'},
  {name: 'CY4d' ,    wave: 'l.............h.......................................................'},
  {name: 'C4D'  ,    wave: 'l..............h.......................................................'},
  {name: 'nSx'  ,    wave: 'h.............l.h............................................'},
  {name: 'phase noe',wave: 'h.l.h.l.h.l.h.....l.h.l.h.l.h.l.h.l.h.l.h.'},
  {name: 'phase ICB4b 4',wave: 'h...l.h.l.h.l...h...l.h.l.h.l.h.l.h.l.h.'},
  {name: 'phase nWE ', wave: '.h...lh..lh..lhlh....l.h.l.h.l.h.l.h.l.h.'},
  {name: 'phase addr', wave: 'h.3...4...3...4...3...4.3.4.3.', data: ['0', '1', '2', '3','0','Amplitude','x3','Control']},
  {name: 'phase IC22mm', wave: '.x...3x..3x..3x3x..3...4.3.4.3.', data: ['sphl', 'sphm', 'sphh', 'amp','0','Amplitude','x3','Control']},
  {name: 'phase ramop', wave: 'x.3.x.3.x.3.x.x...3...4.3.4.3.', data: ['phlo', 'phmid', 'phhigh', 'sum','0','Amplitude','x3','Control']},

  {name: 'adder B', wave: 'x.0.3.0.3.0.3.0.3.0..3...4.3.4.3.', data: ['phlo', 'phmid', 'phhigh', 'amp','0','Amplitude','x3','Control']},

  {name: 'sum bus', wave: 'x.0.3.0.3.0.3.0.3.0..3...4.3.4.3.', data: ['sphlo', 'sphmid', 'sphhigh', 'saudio','0','Amplitude','x3','Control']},

]}
*/

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "Pi1MHz.h"
#include "rpi/audio.h"
#include "rpi/gpio.h"
#include "rpi/info.h"
#include "config.h"
#include "BeebSCSI/fatfs/ff.h"

//NB ample software access the waveform ram with bit 7 and 8 equal
//Pi1MHz has the complete ram where as B-em masks bit 8 when the ram is written
#define I_WAVEFORM(n,a) (((unsigned int)((n)<<8)| ((unsigned int)(((n)<<7)&128)|((unsigned int)a))))
#define I_WFTOP (0x0E00)

#define I_FREQlo(c)  (*((c)+0x00))
#define I_FREQmid(c) (*((c)+0x10))
#define I_FREQhi(c)  (*((c)+0x20))
#define I_WAVESEL(c) (*((c)+0x50))
#define I_AMP(c)     (*((c)+0x60))
#define I_CTL(c)     (*((c)+0x70))
#define FREQ(c)      (I_FREQlo(c)|(I_FREQmid(c)<<8)|(I_FREQhi(c)<<16))
#define PHASESET(c)   (!!(I_FREQlo(c)&1))
#define AMP(c)       (I_AMP(c))
#define WAVESEL(c)   (I_WAVESEL(c)>>4)
#define CTL(c)       (I_CTL(c))
#define MODULATE(c)  (!!(CTL(c)&0x20))
#define INVERT(c)    (!!(CTL(c)&0x10))
#define PAN(c)       (CTL(c)&0xf)

#define DEFAULT_GAIN 3
#define M5000_SAMPLE_RATE 46875   /* 6 MHz master clock / 128 */

// These variables can be setup form the Pi1MHz.cfg file.

static int gain;
static uint8_t autorange;

static int32_t M5000_left_error;
static int32_t M5000_right_error;

static uint8_t fx_pointer;

static size_t ram_max;

#define M5000_REC_BASE 0x100000u
#define M5000_REC_END ( DISC_RAM_SIZE)

static const unsigned char wavfmt[] = {
   'R','I','F','F',
   0x00, 0x00, 0x00, 0x00,
   0x57, 0x41, 0x56, 0x45, // "WAVE"
   0x66, 0x6D, 0x74, 0x20, // "fmt "
   0x10, 0x00, 0x00, 0x00, // format chunk size
   0x01, 0x00,             // format 1=PCM
   0x02, 0x00,             // channels 2=stereo
   0x1B, 0xB7, 0x00, 0x00, // sample rate.
   0x6C, 0xDC, 0x02, 0x00, // byte rate.
   0x04, 0x00,             // block align.
   0x10, 0x00,             // bits per sample.
   0x64, 0x61, 0x74, 0x61, // "DATA".
   0x00, 0x00, 0x00, 0x00, // length filled in later
   0x00, 0x00, 0x00, 0x00  // dummy sample
};

static inline void put_le16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

struct synth {
    uint32_t phaseRAM[16];
    int sleft, sright;
    uint8_t amplitude[16];
    uint8_t * ram;
    uint8_t modulate;
};

static const uint8_t PanArray[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 6, 5, 4, 3, 2, 1 };

NOINIT_SECTION static struct synth m5000, m3000;

NOINIT_SECTION static int antilogtable[128];

static void synth_reset(struct synth *s, uint8_t * ptr)
{
   s->ram = ptr;
   s->modulate = 0;
   // Real hardware clears 0x3E00 for 128bytes
   // and random Waveform bytes depending on phaseRAM
   memset(&s->ram[I_WFTOP], 0, 128);
   memset(&s->amplitude[0], 0, 16);
}

/* Mix domain -> int16, 16.16 fixed point. The old PWM path clipped the
   mix at +/-(512*range) around mid-rail (full span 1024*range), so that
   AMPLITUDE maps to int16 full scale with
   K = 32768 * 65536 / (512 * range) = 2^22 / range,
   referenced to the range the PWM has at 46875 Hz whatever sink is
   active. (2^21 - "full scale was 1024*range" - halved the output by
   confusing the span with the amplitude.) */
static int32_t M5000_s16_scale;

/* .rate is set at init: the hardware's own 46875 Hz normally, or 48000
   when the sound goes to HDMI - HDMI sinks assume a standard rate and
   play 46875 a quartertone (2.4%) sharp. 46875/48000 = 125/128 exactly,
   so pitch is preserved by scaling every channel's phase increment by
   125/128 (m5000_freq_mul below); the error is under 1 LSB of the
   24-bit accumulator, well below a cent. */
static audio_producer_t m5000_producer = {
   .rate = M5000_SAMPLE_RATE,
   /* One DMA block of lead: enough that a block is always ready when the
      DMA frees one, no more - every extra frame here is delay between a
      register write and the speaker. */
   .latency_frames = AUDIO_DMA_FRAMES,
   .name = "Music 5000",
};

/* Phase increments are (FREQ * mul) >> 7: 128 = native rate (exact
   no-op), 125 = running the synth at 48000. */
static uint32_t m5000_freq_mul = 128;
/* True when the output path sums L+R onto the Beeb pin (autorange must
   then watch the sum); HDMI and the hi-Z jack keep the channels apart. */
static bool m5000_pin_sums;

// in Pi1MHz.cfg M5000_Gain=16 set the default audio gain
// if gain has >1000 then gain = gain - 1000 and auto ranging
// is OFF

static void M5000_gain(void) {
   const char *prop = config_get("M5000_Gain");

   if (prop)
      gain = atoi(prop);
   else
      gain = DEFAULT_GAIN;

   if ( gain >1000 )
   {
      autorange = 0;
      gain = gain - 1000;
   }
   else
      autorange = 1;

   /* Bound the multiplier: v*gain is computed in 32 bits before widening,
      and the mix reaches ~2^22, so anything above this wraps into
      opposite-sign full-scale clicks. Far beyond any musical setting. */
   if (gain > 256)
      gain = 256;
   if (gain < 0)
      gain = 0;       /* a negative M5000_Gain= would wrap in the mixer */
}

static void update_channels(struct synth *s)
{
    int sleft = 0;
    int sright = 0;
    uint8_t modulate = s->modulate;

    for (int i = 0; i < 16; i++) {
      int c4d; // c4d is used for "Synchronization" e.g. the "Wha" instrument
      const uint8_t * c = s->ram + I_WFTOP + modulate + i;
      if  (!PHASESET(c))
         {
            uint32_t sum = (((uint32_t)FREQ(c) * m5000_freq_mul) >> 7) + s->phaseRAM[i];
            s->phaseRAM[i] = sum & 0xffffff;
            c4d = sum & ( 1 << 24 );
            // only if there is a carry ( waveform crossing ) do we update the amplitude
            if (c4d)
               s->amplitude[i] = AMP(c);
         }
         else
         {
            s->phaseRAM[i] = ((uint32_t)FREQ(c) * m5000_freq_mul) >> 7;
            c4d = 0;
         }

      int sample = s->ram[ I_WAVEFORM( WAVESEL(c) , ( s->phaseRAM[i] >> 17) ) ];
      int sign = sample & 0x80;

      // The amplitude operates in the log domain
      // - sam holds the wave table output which is 1 bit sign and 7 bit magnitude
      // - amp holds the amplitude which is 1 bit sign and 8 bit magnitude (0x00 being quite, 0x7f being loud)
      // The real hardware combines these in a single 8 bit adder, as we do here
      //
      // Consider a positive wav value (sign bit = 1)
      //       wav: (0x80 -> 0xFF) + amp: (0x00 -> 0x7F) => (0x80 -> 0x7E)
      // values in the range 0x80...0xff are very small are clamped to zero
      //
      // Consider a negative wav vale (sign bit = 0)
      //       wav: (0x00 -> 0x7F) + amp: (0x00 -> 0x7F) => (0x00 -> 0xFE)
      // values in the range 0x00...0x7f are very small are clamped to zero
      //
      // In both cases:
      // - zero clamping happens when the sign bit stays the same
      // - the 7-bit result is in bits 0..6
      //
      // Note:
      // - this only works if the amp < 0x80
      // - amp >= 0x80 causes clamping at the high points of the waveform
      // - this behavior matches the FPGA implementation, and we think the original hardware

      sample += s->amplitude[i];
      modulate = ( MODULATE(c) && (!!(sign) || !!(c4d))) ? 128u : 0u ;

      if ((sign ^ sample) & 0x80) {
         uint8_t pan = PanArray[PAN(c)];
         // sign bits being different is the normal case
         sample &= 0x7f;

         //sam is now an 7-bit log value
         sample =  antilogtable[sample];

         if (INVERT(c)) {
            sign ^= 0x80;
         }

         if (sign) {
            // sign being zero is negative
            sample =-sample;
         }
         // sample is now a 14-bit linear sample
         // Apply panning. Divide by 6 taken out of the loop as a common subexpression
         sleft  += sample * pan;
         sright += sample * (6 - pan);
      }

    }
    s->sleft  = sleft;
    s->sright = sright;
    s->modulate = modulate;
}

/* One channel of the mix to int16 with error-feedback dither. The sum for
   the Beeb's mono pin, and the stereo split for a Pi 3B+ jack, are the
   audio core's job now (rpi/audio.c): the synth always emits L and R. */
static inline int16_t music5000_to_s16(int v, int32_t *err, int *clip)
{
   // the range of sleft/right is (-8031..8031) i.e. 14 bits
   // so summing 16 channels gives an 18 bit output
   // now times 6 as pan division has been taken out of loop
   // so 21bits
   //
   // Worst case we should divide by 256 to get 20bits down to 12.x bits.
   // But this does loose dynamic range.
   //
   // Even loud tracks like In Concert by Pilgrim Beat rarely use
   // the full 18 bits:
   //
   //   L:-25086..26572 (rms  3626) R:-23347..21677 (rms  3529)
   //   L:-25795..31677 (rms  3854) R:-22592..21373 (rms  3667)
   //   L:-20894..20989 (rms  1788) R:-22221..17949 (rms  1367)
   //
   // So lets try a crude adaptive clipping system, and see what feedback we get!
   // 64-bit: the 21-bit mix times gain times the 9-bit scale can pass
   // 2^31 on a loud clip, and it is those samples that must saturate,
   // not wrap. One SMULL on ARM.
   int64_t acc = (int64_t)(v * gain) * M5000_s16_scale + *err;
   int32_t out = (int32_t)(acc >> 16);
   *err = (int32_t)(acc - ((int64_t)out << 16));
   if (out < -32768) { out = -32768; *err = 0; *clip = 1; }
   if (out >  32767) { out =  32767; *err = 0; *clip = 1; }
   return (int16_t)out;
}

static void music5000_store_sample(int sl, int sr, int16_t *out)
{
   int clip = 0;

   out[0] = music5000_to_s16(sl, &M5000_left_error,  &clip);
   out[1] = music5000_to_s16(sr, &M5000_right_error, &clip);

   /* The Beeb pin carries L+R summed (saturated silently by the sink),
      so the pin clips before either channel does on correlated content:
      autorange must watch the sum - but ONLY where summing happens.
      HDMI and the hi-Z stereo jack carry the channels separately, and
      autorange is a one-way ratchet: a false trigger costs 6 dB for the
      session. */
   if (m5000_pin_sums) {
      int32_t sum = (int32_t)out[0] + out[1];
      if (sum > 32767 || sum < -32768)
         clip = 1;
   }

   if (clip && autorange) {
      M5000_left_error = 0 ;
      M5000_right_error = 0 ;
      if (gain > 1) gain /= 2;
      LOG_DEBUG("Music 5000 clipped, halving gain (multiplier now %i)\r\n", gain);
   }
}

static bool record = false;
static bool rec_started;
static uint32_t Audio_Index;

static void music5000_rec_start(void)
{
    Audio_Index = M5000_REC_BASE + sizeof(wavfmt);
    rec_started = false;
    if (Pi1MHz->JIM_ram_size <= ( M5000_REC_END/(JIM_RAM_STEP)))
      return;
    ram_max = (size_t)Pi1MHz->JIM_ram_size * JIM_RAM_STEP - M5000_REC_END ;
    record = true;
}

static void music5000_rec_stop(void)
{
   if (config_beeb_write_protected()) {     // write-protect: don't record to the SD card
      record = false;
      fx_register[fx_pointer] = 0;
      return;
   }

   char fn[22];
   FRESULT result;
   int number = 0;
   FIL music5000_fp;

   do {
      sprintf(fn,"Musics%.3i.wav",number);
      result = f_open( &music5000_fp, fn, FA_CREATE_NEW  | FA_WRITE);
      LOG_DEBUG("Music5000 Filename : %s\r\n",fn);
      number++;
   } while ( result != FR_OK && number < 1000 );

   if ( result != FR_OK )
   {
      LOG_DEBUG("Music5000 recording stopped as we could not create a file\r\n");
   }
   else {
      memcpy(&Pi1MHz->JIM_ram[M5000_REC_BASE], wavfmt, sizeof(wavfmt));

      uint32_t size = Audio_Index - M5000_REC_BASE;
      put_le32(&Pi1MHz->JIM_ram[M5000_REC_BASE+4], size - 8u);
      put_le32(&Pi1MHz->JIM_ram[M5000_REC_BASE+40], size - 44u);

      UINT temp;
      f_write(&music5000_fp, &Pi1MHz->JIM_ram[M5000_REC_BASE],Audio_Index - M5000_REC_BASE , &temp);
      f_close(&music5000_fp);
   }
   record = false;
   fx_register[fx_pointer] = 0;
}

static void store_samples(int sl, int sr)
{
   if (rec_started || sl || sr)
    {
      sl = (sl * gain * 12) / 1024; // +/- 2666.6 so scale to fit +/- 32,768
      sr = (sr * gain * 12) / 1024;
      if ((Audio_Index+4) >= ram_max)
      {
         LOG_INFO("Music 5000 recording stopped as we have run out of JIM RAM\r\n");
         music5000_rec_stop();
         return;
      }
      put_le16(&Pi1MHz->JIM_ram[Audio_Index], (uint16_t)sl);
      put_le16(&Pi1MHz->JIM_ram[Audio_Index+2], (uint16_t)sr);
      Audio_Index += 4;
      rec_started = true;
    }
}

static void music5000_emulate(void)
{
   if ((record == false ) && (fx_register[fx_pointer] != 0))
   {
      music5000_rec_start();
   }

   if ((record ) && (fx_register[fx_pointer] == 0))
   {
      music5000_rec_stop();
   }

   /* Not the audio owner (the video player has the output): synthesise
      nothing - this loop is where the Music 5000's CPU time goes. */
   uint32_t space;
   int16_t *bufptr = audio_write_ptr(&m5000_producer, &space);
   if (bufptr && space)
   {
      for (size_t sample = space; sample !=0 ; sample--) {
         update_channels(&m5000);
         update_channels(&m3000);
         int sl = m5000.sleft  + m3000.sleft;
         int sr = m5000.sright + m3000.sright;

         music5000_store_sample(sl, sr, bufptr);
         if (record)
            store_samples(sl, sr);
         bufptr += 2;
      }
      audio_commit(space);
   }
}

void M5000_emulator_init(uint8_t instance, uint8_t address)
{
   if (record)
   {
      // stop recording
      music5000_rec_stop();
   }
   fx_pointer = instance ;
   fx_register[fx_pointer] = 0;

   for (uint32_t n = 0; n <(sizeof(antilogtable)/sizeof(antilogtable[0])) ; n++) {
      // 12-bit antilog as per AM6070 datasheet
      // this actually has a 13 bit fsd ( sign bit makes it 14bit)
      int S = n & 15;
      uint32_t C = n >> 4;
      antilogtable[n] = ( (1<<C)*((S<<1) + 33) - 33);
   }

   M5000_gain();

   synth_reset(&m5000, &Pi1MHz->JIM_ram[0x3000]);
   synth_reset(&m3000, &Pi1MHz->JIM_ram[0x5000]);

   /* The clip point stays referenced to the PWM range at the native
      rate whatever rate the synth actually runs at */
   M5000_s16_scale = (int32_t)((1 << 22) / (250000000 / M5000_SAMPLE_RATE));
   M5000_left_error = M5000_right_error = 0;
   if (audio_out_is_hdmi()) {
      m5000_producer.rate = 48000;
      m5000_freq_mul = 125;
   } else {
      m5000_producer.rate = M5000_SAMPLE_RATE;
      m5000_freq_mul = 128;
   }
   m5000_pin_sums = !audio_out_is_hdmi() && !rpi_audio_beeb_muted();
   audio_claim(&m5000_producer);

   // register polling function
   Pi1MHz_Register_Poll(music5000_emulate, "m5000");
}

uint8_t M5000_emulator_read_instance(void)
{
   return fx_pointer;
}