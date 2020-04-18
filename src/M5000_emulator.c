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

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "Pi1MHz.h"
#include "rpi/rpi-audio.h"
#include "rpi/rpi-gpio.h"
#include "rpi/info.h"

// buffer size of 1500 16ms @ 46.875KHz (must be multiple of 3)

//NB ample software access the waveform ram with bit 7 and 8 equal
//Pi1MHz has the complete ram where as B-em masks bit 8 when the ram is written
#define I_WAVEFORM(n,a) (((n)<<8)| (((n)<<7)&128)|(a))
#define I_WFTOP (0x0E00)

#define I_FREQlo(c)  (*((c)+0x00))
#define I_FREQmid(c) (*((c)+0x10))
#define I_FREQhi(c)  (*((c)+0x20))
#define I_WAVESEL(c) (*((c)+0x50))
#define I_AMP(c)     (*((c)+0x60))
#define I_CTL(c)     (*((c)+0x70))
#define FREQ(c)      (I_FREQlo(c)|(I_FREQmid(c)<<8)|(I_FREQhi(c)<<16))
#define DISABLE(c)   (!!(I_FREQlo(c)&1))
#define AMP(c)       (I_AMP(c))
#define WAVESEL(c)   (I_WAVESEL(c)>>4)
#define CTL(c)       (I_CTL(c))
#define MODULATE(c)  (!!(CTL(c)&0x20))
#define INVERT(c)    (!!(CTL(c)&0x10))
#define PAN(c)       (CTL(c)&0xf)

#define DEFAULT_GAIN 16

// These variables can be setup form the config.txt file.

static int stereo;
static int gain;
static int autorange;

struct synth {
    uint32_t phaseRAM[16];
    int sleft,sright;
    uint8_t * ram;
};

static const uint8_t PanArray[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 6, 5, 4, 3, 2, 1 };

NOINIT_SECTION static struct synth m5000, m3000;

NOINIT_SECTION static int antilogtable[128];

static void synth_reset(struct synth *s, uint8_t * ptr)
{
   s->ram = ptr;
   // Real hardware clears 0x3E00 for 128bytes and random
   // Waveform bytes depending on phaseRAM
   // we only need to clear the disable bytes   
   memset(&s->ram[I_WFTOP], 1, 16);
}

static int32_t audio_range;

// in config.txt M5000_Gain=16 set the default audio gain
// if gain has >1000 then gain = gain - 1000 and auto ranging
// is disabled

static void M5000_gain() {
   char *prop = get_cmdline_prop("M5000_Gain");

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
}

// in command.txt M5000_BeebAudio_Off=1 turns off beeb Audio

static void M5000_BeebAudio() {
   char *prop = get_cmdline_prop("M5000_BeebAudio_Off");

   if (prop)
      stereo = atoi(prop);
   else
      stereo = 0;

   if (stereo == 1 )
      RPI_SetGpioPinFunction(AUDIO_PIN, FS_INPUT); // Turn off Beeb audio
}

static void update_channels(struct synth *s)
{
   int sleft = 0;
   int sright = 0;
   uint8_t modulate = 0; // in real hardware modulate wraps from channel 16 to 0 but is never used

   for (int i = 0; i < 16; i++)
   {
      uint8_t * c = s->ram + I_WFTOP + modulate + i;

      // In the real hardware the disable bit works by forcing the
      // phase accumulator to zero.
      if (DISABLE(c)) {
          s->phaseRAM[i] = 0;
		  // A slight differnce as modulation is still calculated in real hardware
		  // but not here
		  modulate = 0;
      } else {
		  int c4d, sign, sample;
          unsigned int sum = s->phaseRAM[i] + FREQ(c);
          s->phaseRAM[i] = sum & 0xffffff;
          // c4d is used for "Synchronization" e.g. the "Wha" instrument
          c4d = sum & (1<<24);

		  sample = s->ram[I_WAVEFORM(WAVESEL(c),( s->phaseRAM[i] >> 17))];
		  {
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
		  }
		  sign = sample & 0x80;
		  sample += AMP(c);
		  modulate = (( MODULATE(c) && (!!(sign) || !!(c4d)))? 128:0);
		  if ((sign ^ sample) & 0x80) {
			 // sign bits being different is the normal case
			 sample &= 0x7f;
		  } else {
			 // sign bits being the same indicates underflow so clamp to zero
			 sample = 0;
		  }

		  if (INVERT(c)) {
			 sign ^= 0x80;
		  }
		  //sam is now an 8-bit log value
		  sample =  antilogtable[sample];
		  if (!(sign))
		  {
			 // sign being zero is negative
			 sample =-sample;
		  }
		  // sample is now a 14-bit linear sample
		  uint8_t pan = PanArray[PAN(c)];

		  // Apply panning. Divide by 6 taken out of the loop as a common subexpression
		  sleft  += ((sample*pan));
		  sright += ((sample*(6 - pan)));
	  }
   }
   s->sleft  = sleft; // should really divide by six but as that is just gain
   s->sright = sright; // so we can do it later
}

static void music5000_get_sample(uint32_t *left, uint32_t *right)
{
    // the range of sleft/right is (-8191..8191) i.e. 14 bits
    // so summing 16 channels gives an 18 bit output
    // now times 6 as pan division has been taken out of loop
    int sl = m5000.sleft  + m3000.sleft;
    int sr = m5000.sright + m3000.sright;

    if (!stereo)
    {
       sl = sl + sr;
       sr = sl;
    }

    // Worst case we should divide by 64 to get 18 bits down to 12.x bits.
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
    sl = ( sl * gain ) / 1024;

    int clip = 0;
    if (sl < -audio_range) {
        sl = -audio_range;
        clip = 1;
    }
    if (sl >  audio_range) {
        sl =  audio_range;
        clip = 1;
    }
    *left = sl + audio_range;

    sr = ( sr * gain ) / 1024;
    if (sr < -audio_range) {
        sr = -audio_range;
        clip = 1;
    }
    if (sr >  audio_range) {
        sr =  audio_range;
        clip = 1;
    }
    *right = sr + audio_range;
    if (clip && autorange) {
        gain /= 2;
        LOG_DEBUG("Music 5000 clipped, reducing gain by 3dB (divisor now %i)\r\n", gain);
    }
}

void music5000_emulate(void)
{
   size_t space = rpi_audio_buffer_free_space();
   if (space)
   {
    uint32_t *bufptr = rpi_audio_buffer_pointer();
    for (size_t sample = (space>>1); sample !=0 ; sample--) {
        update_channels(&m5000);
        update_channels(&m3000);
        music5000_get_sample(bufptr, bufptr + 1);
        bufptr +=2;
    }
    rpi_audio_samples_written();
   }
}

void M5000_emulator_init()
{
   for (uint32_t n = 0; n <(sizeof(antilogtable)/sizeof(antilogtable[0])) ; n++) {
       //12-bit antilog as per AM6070 datasheet
       // this actually has a 13 bit fsd ( sign bit makes it 14bit)
       int S = n & 15, C = n >> 4;
       antilogtable[n] = ( (1<<C)*((S<<1) + 33) - 33);
   }
   
   M5000_gain();
   M5000_BeebAudio();

   synth_reset(&m5000, &JIM_ram[0x3000]);
   synth_reset(&m3000, &JIM_ram[0x5000]);

   audio_range = rpi_audio_init(46875)>>1;

   // register polling function
   Pi1MHz_Register_Poll(music5000_emulate);
}
