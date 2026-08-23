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

static const audio_producer_t beebsid_producer = {
    .rate = BEEBSID_SAMPLE_RATE,
    .latency_frames = AUDIO_DMA_FRAMES,      /* one block of lead, as M5000 */
    .name = "BeebSID",
    .mono = true,                            /* pin takes the sample as-is */
};

static void beebsid_poll(void)
{
    int16_t mono[256];
    uint32_t space;
    int16_t *out = audio_write_ptr(&beebsid_producer, &space);

    if (!out || space == 0) {
        return;
    }
    if (space > sizeof(mono) / sizeof(mono[0])) {
        space = sizeof(mono) / sizeof(mono[0]);
    }

    beebsid_sid_render(mono, space);
    /* Full scale into both channels: the producer's mono flag stops the
       Beeb-pin path from summing them (which would clip), and the jack
       and HDMI get the full-level sample on both sides as before. */
    for (uint32_t i = 0; i < space; i++) {
        out[i * 2u] = mono[i];
        out[i * 2u + 1u] = mono[i];
    }
    audio_commit(space);
}

void BeebSID_emulator_init(uint8_t instance, uint8_t address)
{
    unsigned int i;

    (void)instance;
    beebsid_base = address;
    beebsid_sample_rate = BEEBSID_SAMPLE_RATE;

    beebsid_sid_init(beebsid_sample_rate);
    audio_claim(&beebsid_producer);

    for (i = 0; i < 32u; i++) {
        Pi1MHz_Register_Memory(WRITE_FRED, (address + i), beebsid_write);
    }
    Pi1MHz_Register_Poll(beebsid_poll);
}
