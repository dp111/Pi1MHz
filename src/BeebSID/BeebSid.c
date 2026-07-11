#include "BeebSid.h"

#include "../Pi1MHz.h"
#include "beebsid_sid.h"
#include "../rpi/audio.h"

#include <stdint.h>

/* Match M5000's PWM sample clock. */
#ifndef BEEBSID_SAMPLE_RATE
#define BEEBSID_SAMPLE_RATE 46875u
#endif

static uint8_t beebsid_base;
static uint32_t beebsid_sample_rate;
static int32_t beebsid_dither_error;   /* error-feedback state for rpi_audio_pack */

static void beebsid_write(unsigned int gpio)
{
    uint8_t addr = (uint8_t)GET_ADDR(gpio);
    uint8_t data = (uint8_t)GET_DATA(gpio);
    uint8_t reg = (uint8_t)(addr - beebsid_base);

    if (reg <= 24u) {
        beebsid_sid_write(reg, data);
    }
    Pi1MHz_MemoryWrite(addr, data);
}

static void beebsid_poll(void)
{
    size_t space = rpi_audio_buffer_free_space() >> 1; /* stereo frames */
    int16_t samples[DMA_BUFFER_SIZE / 2u];
    uint32_t *bufptr;
    size_t i;

    if (space == 0) {
        return;
    }
    if (space > (DMA_BUFFER_SIZE / 2u)) {
        space = DMA_BUFFER_SIZE / 2u;
    }

    beebsid_sid_render(samples, space);
    bufptr = rpi_audio_buffer_pointer();

    for (i = 0; i < space; i++) {
        uint32_t word = rpi_audio_pack(samples[i], &beebsid_dither_error);
        bufptr[i * 2u] = word;
        bufptr[i * 2u + 1u] = word;
    }
    rpi_audio_samples_written();
}

void BeebSID_emulator_init(uint8_t instance, uint8_t address)
{
    unsigned int i;

    (void)instance;
    beebsid_base = address;
    beebsid_sample_rate = BEEBSID_SAMPLE_RATE;
    beebsid_dither_error = 0;

    beebsid_sid_init(beebsid_sample_rate);
    rpi_audio_init(beebsid_sample_rate);

    for (i = 0; i < 32u; i++) {
        Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(address + i), beebsid_write);
    }
    Pi1MHz_Register_Poll(beebsid_poll);
}
