#pragma once
#include <stddef.h>
#include <stdint.h>

void beebsid_sid_init(uint32_t sample_rate_hz);
void beebsid_sid_reset(void);
void beebsid_sid_write(uint8_t reg /*0..24*/, uint8_t value);

/* Render `frames` mono int16 samples into out, advancing the SID clock to
 * match. FastSID (factor 1000) always produces exactly `frames`. Returns the
 * number of frames written. */
size_t beebsid_sid_render(int16_t *out, size_t frames);

uint32_t beebsid_sid_sample_rate(void);
