#pragma once
#include <stddef.h>
#include <stdint.h>

void beebsid_sid_init(uint32_t sample_rate_hz);
void beebsid_sid_reset(void);
void beebsid_sid_write(uint8_t reg /*0..24*/, uint8_t value);

/* Advance SID by cpu_cycles at 1MHz; write mono int16 frames into out.
 * Returns frames written (may be < max_frames). */
size_t beebsid_sid_render(uint32_t cpu_cycles, int16_t *out, size_t max_frames);

uint32_t beebsid_sid_sample_rate(void);
