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
static uint32_t beebsid_audio_range;

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

static uint32_t sample_to_pwm(int16_t s)
{
    int32_t mid = (int32_t)(beebsid_audio_range / 2u);
    int32_t v;

    if (mid <= 0) {
        return 0;
    }
    v = mid + (((int32_t)s * mid) / 32768);
    if (v < 0) {
        v = 0;
    }
    if (v > (int32_t)beebsid_audio_range) {
        v = (int32_t)beebsid_audio_range;
    }
    return (uint32_t)v;
}

static void beebsid_poll(void)
{
    size_t space = rpi_audio_buffer_free_space() >> 1; /* stereo frames */
    uint32_t *bufptr;
    size_t i;
    int16_t samples[DMA_BUFFER_SIZE / 2u];
    uint32_t cycles;
    size_t got;

    if (space == 0) {
        return;
    }
    if (space > (DMA_BUFFER_SIZE / 2u)) {
        space = DMA_BUFFER_SIZE / 2u;
    }

    cycles = (uint32_t)(((uint64_t)space * 1000000ull) / (uint64_t)beebsid_sample_rate);
    got = beebsid_sid_render(cycles, samples, space);
    bufptr = rpi_audio_buffer_pointer();

    for (i = 0; i < space; i++) {
        int16_t s = (i < got) ? samples[i] : 0;
        uint32_t word = sample_to_pwm(s);
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

    beebsid_sid_init(beebsid_sample_rate);
    beebsid_audio_range = rpi_audio_init(beebsid_sample_rate);

    for (i = 0; i < 32u; i++) {
        Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(address + i), beebsid_write);
    }
    Pi1MHz_Register_Poll(beebsid_poll);
}
