#include "beebsid_sid.h"

#include <string.h>

#include "fastsid.h"
#include "maincpu.h"
#include "types.h"

CLOCK maincpu_clk;

static sound_t *g_psid;
static BYTE g_sidstate[32];
static uint32_t g_sample_rate;

void beebsid_sid_init(uint32_t sample_rate_hz)
{
    g_sample_rate = sample_rate_hz ? sample_rate_hz : 44100u;
    maincpu_clk = 0;
    memset(g_sidstate, 0, sizeof(g_sidstate));

    if (g_psid) {
        fastsid_hooks.close(g_psid);
        g_psid = NULL;
    }

    g_psid = fastsid_hooks.open(g_sidstate);
    /* factor 1000 = 1:1 sample generation (see fastsid_calculate_samples) */
    if (!fastsid_hooks.init(g_psid, (int)g_sample_rate, 1000000, 1000)) {
        fastsid_hooks.close(g_psid);
        g_psid = NULL;
        return;
    }
    fastsid_hooks.reset(g_psid, maincpu_clk);
}

void beebsid_sid_reset(void)
{
    if (!g_psid) {
        return;
    }
    fastsid_hooks.reset(g_psid, maincpu_clk);
}

void beebsid_sid_write(uint8_t reg, uint8_t value)
{
    if (!g_psid || reg > 24) {
        return;
    }
    fastsid_hooks.store(g_psid, (WORD)reg, (BYTE)value);
    g_sidstate[reg] = value;
}

size_t beebsid_sid_render(int16_t *out, size_t frames)
{
    int delta_t = 0; /* unused by FastSID at factor 1000; it returns exactly `frames` */
    int written;

    if (!g_psid || !out || frames == 0) {
        return 0;
    }

    /* Keep the SID master clock advancing in 1MHz cycles so register-write
     * timestamps stay monotonic; output rate is driven purely by `frames`. */
    maincpu_clk += (CLOCK)(((uint64_t)frames * 1000000u) / (uint64_t)g_sample_rate);

    written = fastsid_hooks.calculate_samples(g_psid, (SWORD *)out, (int)frames, 1, &delta_t);
    if (written < 0) {
        return 0;
    }
    return (size_t)written;
}

uint32_t beebsid_sid_sample_rate(void)
{
    return g_sample_rate;
}
