/* Native resolution at 50 Hz, whatever the monitor.

   The Pi firmware picks the monitor's preferred mode from its EDID, which is
   the native resolution at 60 Hz; its own tables only have 50 Hz modes at
   the television sizes, and there is no "prefer 50 Hz" in config.txt.  The
   Domesday video is 25 frames a second, so 50 Hz is the rate at which every
   frame is shown exactly twice and pans do not judder.

   The firmware does export the mode-set path its fake-KMS Linux driver uses:
   SET_TIMING takes a complete timing (pixel clock, active, sync, total,
   refresh, flags) and retimes the HDMI output, and GET_EDID_BLOCK reads the
   monitor's EDID; the timing in force is read back from the pixel valve
   itself, since the tag for that answers with zeros.  So, at boot: read
   the EDID's preferred detailed timing, keep its geometry (the panel's
   native size and its own blanking) and rescale the pixel clock for the
   target refresh, then ask the firmware for that.  A television lists its
   real 50 Hz modes in the EDID's CEA block; those are sent as the CEA
   timing with the video code, not a rescaled 60 Hz one it may refuse.

   Only when needed: if the mode in force is already at the target rate
   (a config.txt hdmi_mode that is 50 Hz, say) nothing is sent and the
   monitor does not resync.  A monitor whose range-limits descriptor puts
   its minimum refresh above the target is left alone.  Display_refresh=off
   turns the whole thing off and config.txt decides as before. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_mode.h"
#include "mailbox.h"
#include "rpi.h"
#include "../config.h"
#include "hdmi_audio.h"           /* hdmi_pixel_clock_hz */

#define TAG_GET_EDID_BLOCK_    0x30020u
#define TAG_SET_TIMING         0x48017u

#define HDMI_DISPLAY_ID 2u          /* the firmware's number for HDMI0 */

/* The firmware's timing record (struct set_timings in Linux's
   vc4_firmware_kms.c), 36 bytes, all little-endian. */
typedef struct {
    uint8_t  display;
    uint8_t  padding;
    uint16_t video_id_code;         /* CEA VIC, 0 for a non-CEA mode */
    uint32_t clock;                 /* kHz */
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint16_t vrefresh, padding2;
    uint32_t flags;
} fw_timing_t;
_Static_assert(sizeof(fw_timing_t) == 36, "firmware timing record is 36 bytes");

#define TF_H_SYNC_POS   (1u << 0)
#define TF_V_SYNC_POS   (1u << 1)
#define TF_INTERLACE    (1u << 2)
#define TF_ASPECT_4_3   (1u << 4)
#define TF_ASPECT_16_9  (2u << 4)
#define TF_RGB_LIMITED  (1u << 8)

static char report[128] = "not run";
static uint8_t edid_blocks[2][128];         /* block 0 and the first extension */
static unsigned edid_bytes;                 /* 0, 128 or 256 */

/* SET_TIMING's reply carries nothing (length 0) whether or not the firmware
   acted on it, and RPI_PropertyGet masks off the per-tag response bit, so
   the reply cannot say; the caller judges by reading the timing back. */
static void fw_timing_set(const fw_timing_t *t)
{
    const uint32_t *w = (const uint32_t *)t;
    RPI_PropertyStart((rpi_mailbox_tag_t)TAG_SET_TIMING, sizeof *t / 4u);
    for (unsigned i = 0; i < sizeof *t / 4u; i++)
        RPI_PropertyAdd(w[i]);
    RPI_PropertyProcess(true);
}

/* The timing in force, read from the hardware: pixel valve 2 drives HDMI
   on the Pi 0-3 and holds the porches, syncs and active sizes; the pixel
   clock comes from PLLH.  (The firmware's GET_DISPLAY_TIMING tag answers
   with zeros on this firmware, so it is not used.)  False with no display. */
static bool pv_timing_get(fw_timing_t *t)
{
    volatile uint32_t *pv = (volatile uint32_t *)(PERIPHERAL_BASE + 0x807000u);
    uint32_t ctrl = pv[0], vctrl = pv[1], horza = pv[3], horzb = pv[4], verta = pv[5], vertb = pv[6];
    memset(t, 0, sizeof *t);
    if ((ctrl & 1u) == 0u) return false;                  /* pixel valve off */
    t->hdisplay = (uint16_t)(horzb & 0xFFFFu);
    t->htotal   = (uint16_t)((horza >> 16) + (horza & 0xFFFFu) + (horzb >> 16) + t->hdisplay);
    t->vdisplay = (uint16_t)(vertb & 0xFFFFu);
    t->vtotal   = (uint16_t)((verta >> 16) + (verta & 0xFFFFu) + (vertb >> 16) + t->vdisplay);
    t->clock    = hdmi_pixel_clock_hz() / 1000u;
    /* Interlaced: the valve holds one field, so these are field figures and
       timing_mhz() gives the FIELD rate.  1080i50 reads as 1920x540 at
       50 Hz - which is not a 50 Hz frame, and the planes are laid out for a
       progressive frame, so it must never pass as "already there". */
    if (vctrl & (1u << 4)) {
        t->flags |= TF_INTERLACE;
        t->vdisplay = (uint16_t)(t->vdisplay * 2u);
    }
    return t->hdisplay != 0u && t->htotal != 0u && t->vtotal != 0u && t->clock != 0u;
}

/* Refresh in millihertz from a timing record. */
static uint32_t timing_mhz(const fw_timing_t *t)
{
    uint32_t total = (uint32_t)t->htotal * t->vtotal;
    if (total == 0u) return 0u;
    return (uint32_t)(((uint64_t)t->clock * 1000000u) / total);
}

/* One 128-byte EDID block into edid[]; false if the firmware has none.
   The firmware fetches it over DDC on request, and a read can fail on the
   wire (seen once on a long monitor lead), so try a few times. */
static bool edid_read_once(uint32_t block, uint8_t edid[128]);
static bool edid_read(uint32_t block, uint8_t edid[128])
{
    for (unsigned attempt = 0; attempt < 4u; attempt++)
        if (edid_read_once(block, edid))
            return true;
    return false;
}

static bool edid_read_once(uint32_t block, uint8_t edid[128])
{
    RPI_PropertyStart((rpi_mailbox_tag_t)TAG_GET_EDID_BLOCK_, 34u);
    RPI_PropertyAdd(block);
    for (unsigned i = 1; i < 34u; i++)
        RPI_PropertyAdd(0u);
    RPI_PropertyProcess(true);
    const rpi_mailbox_property_t *mp = RPI_PropertyGet((rpi_mailbox_tag_t)TAG_GET_EDID_BLOCK_);
    if (mp == NULL || mp->byte_length < 136u || mp->data.buffer_32[1] != 0u)
        return false;                          /* [1] is the status word */
    memcpy(edid, &mp->data.buffer_8[8], 128u);
    return true;
}

/* Both EDID blocks into edid_blocks[], once; false if there is no EDID. */
static bool edid_fetch(void)
{
    if (edid_bytes != 0u)
        return true;
    uint8_t *edid = edid_blocks[0];
    if (!edid_read(0, edid) || memcmp(edid, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", 8) != 0)
        return false;
    edid_bytes = 128u;
    if (edid[126] != 0u && edid_read(1, edid_blocks[1]))
        edid_bytes = 256u;
    return true;
}

/* The preferred detailed timing (EDID bytes 54-71) as a firmware record
   at the given refresh: the panel's own geometry, pixel clock rescaled. */
static bool edid_preferred_timing(const uint8_t *e, uint32_t hz, fw_timing_t *t)
{
    const uint8_t *d = e + 54;
    uint32_t pclk = (uint32_t)d[0] | ((uint32_t)d[1] << 8);   /* 10 kHz */
    if (pclk == 0u) return false;
    uint32_t ha = d[2] | ((d[4] & 0xF0u) << 4), hb = d[3] | ((d[4] & 0x0Fu) << 8);
    uint32_t va = d[5] | ((d[7] & 0xF0u) << 4), vb = d[6] | ((d[7] & 0x0Fu) << 8);
    uint32_t hso = d[8] | ((d[11] & 0xC0u) << 2), hsw = d[9] | ((d[11] & 0x30u) << 4);
    uint32_t vso = (d[10] >> 4) | ((d[11] & 0x0Cu) << 2), vsw = (d[10] & 0x0Fu) | ((d[11] & 0x03u) << 4);
    uint32_t wmm = d[12] | ((d[14] & 0xF0u) << 4), hmm = d[13] | ((d[14] & 0x0Fu) << 8);
    uint8_t  fl = d[17];
    if (fl & 0x80u) return false;              /* interlaced: leave it alone */
    if (ha == 0u || va == 0u || hb == 0u || vb == 0u) return false;
    if (hso + hsw > hb || vso + vsw > vb) return false;   /* sync outside the blanking */

    memset(t, 0, sizeof *t);
    t->display     = HDMI_DISPLAY_ID;
    t->hdisplay    = (uint16_t)ha;
    t->hsync_start = (uint16_t)(ha + hso);
    t->hsync_end   = (uint16_t)(ha + hso + hsw);
    t->htotal      = (uint16_t)(ha + hb);
    t->vdisplay    = (uint16_t)va;
    t->vsync_start = (uint16_t)(va + vso);
    t->vsync_end   = (uint16_t)(va + vso + vsw);
    t->vtotal      = (uint16_t)(va + vb);
    t->vrefresh    = (uint16_t)hz;
    t->clock       = (uint32_t)(((uint64_t)t->htotal * t->vtotal * hz + 500u) / 1000u);
    /* Sync polarity bits are only defined for digital separate sync. */
    if ((fl & 0x18u) == 0x18u)
        t->flags = ((fl & 0x02u) ? TF_H_SYNC_POS : 0u) | ((fl & 0x04u) ? TF_V_SYNC_POS : 0u);
    else
        t->flags = TF_H_SYNC_POS | TF_V_SYNC_POS;
    if (wmm && hmm) {
        if (wmm * 3u >= hmm * 4u - hmm / 25u && wmm * 3u <= hmm * 4u + hmm / 25u)
            t->flags |= TF_ASPECT_4_3;
        else if (wmm * 9u >= hmm * 16u - hmm / 25u && wmm * 9u <= hmm * 16u + hmm / 25u)
            t->flags |= TF_ASPECT_16_9;
    }
    return true;
}

/* Minimum vertical rate from the range-limits descriptor, 0 if there is none. */
static uint32_t edid_min_refresh(const uint8_t *e)
{
    for (unsigned off = 72u; off <= 108u; off += 18u) {
        const uint8_t *d = e + off;
        if (d[0] == 0u && d[1] == 0u && d[2] == 0u && d[3] == 0xFDu)
            return (uint32_t)d[5] + (((d[4] & 0x03u) == 0x03u) ? 255u : 0u);
    }
    return 0u;
}

/* The 50 Hz CEA modes a television advertises.  Sent with their video code
   so the set recognises them, and limited-range RGB as CEA modes are. */
static const fw_timing_t cea50[] = {
    { HDMI_DISPLAY_ID, 0, 31, 148500, 1920, 2448, 2492, 2640, 0, 1080, 1084, 1089, 1125, 0, 50, 0,
      TF_H_SYNC_POS | TF_V_SYNC_POS | TF_ASPECT_16_9 | TF_RGB_LIMITED },
    { HDMI_DISPLAY_ID, 0, 19,  74250, 1280, 1720, 1760, 1980, 0,  720,  725,  730,  750, 0, 50, 0,
      TF_H_SYNC_POS | TF_V_SYNC_POS | TF_ASPECT_16_9 | TF_RGB_LIMITED },
    { HDMI_DISPLAY_ID, 0, 17,  27000,  720,  732,  796,  864, 0,  576,  581,  586,  625, 0, 50, 0,
      TF_ASPECT_4_3 | TF_RGB_LIMITED },
};

/* Does the CEA extension block list this video code? */
static bool cea_lists_vic(const uint8_t *x, uint8_t vic)
{
    if (x[0] != 0x02u || x[2] < 4u) return false;
    unsigned i = 4u, end = x[2];
    while (i < end && i < 127u) {
        unsigned tag = x[i] >> 5, len = x[i] & 0x1Fu;
        if (tag == 2u)
            for (unsigned k = 1; k <= len && i + k < 128u; k++)
                if ((x[i + k] & 0x7Fu) == vic) return true;
        i += len + 1u;
    }
    return false;
}

void display_mode_select(void)
{
    static bool done;
    if (done) return;                         /* init_emulator runs again on BREAK */
    done = true;

    const char *v = config_get("Display_refresh");
    uint32_t hz = 50u;
    if (v != NULL) {
        if (strcmp(v, "off") == 0 || atoi(v) == 0) {
            snprintf(report, sizeof report, "off (config.txt decides)");
            return;
        }
        hz = (uint32_t)atoi(v);
        if (hz < 24u || hz > 120u) hz = 50u;
    }

    fw_timing_t now;
    if (!pv_timing_get(&now)) {
        snprintf(report, sizeof report, "no display");
        return;
    }
    uint32_t now_mhz = timing_mhz(&now);
    const char *now_i = (now.flags & TF_INTERLACE) ? "i" : "";

    if (!(now.flags & TF_INTERLACE)
        && now_mhz + 500u >= hz * 1000u && now_mhz <= hz * 1000u + 500u) {
        snprintf(report, sizeof report, "%ux%u already %lu Hz", now.hdisplay, now.vdisplay,
                 (unsigned long)hz);
        return;
    }

    /* Only now the EDID: two blocks over DDC are ~14 ms of boot, so they are
       read when a decision needs them (and by /edid on demand). */
    if (!edid_fetch()) {
        snprintf(report, sizeof report, "no EDID; %ux%u%s @ %lu.%02lu Hz left as set",
                 now.hdisplay, now.vdisplay, now_i, (unsigned long)(now_mhz / 1000u),
                 (unsigned long)((now_mhz % 1000u) / 10u));
        return;
    }

    uint8_t *edid = edid_blocks[0], *ext = edid_blocks[1];

    /* The panel's preferred timing, rescaled - unless it is interlaced or
       otherwise unusable, in which case only a CEA mode below will do. */
    fw_timing_t want;
    bool have = edid_preferred_timing(edid, hz, &want);
    const char *how = "EDID";

    /* A television: its own 50 Hz progressive mode, if it lists one - at the
       preferred size when the preferred timing is usable, else the best it
       has (a set that prefers 1080i50 usually lists 1080p50 or 720p50). */
    if (hz == 50u && edid_bytes == 256u) {
        for (unsigned i = 0; i < sizeof cea50 / sizeof cea50[0]; i++) {
            if (!cea_lists_vic(ext, (uint8_t)cea50[i].video_id_code))
                continue;
            if (have && (cea50[i].hdisplay != want.hdisplay || cea50[i].vdisplay != want.vdisplay))
                continue;
            want = cea50[i];
            how = "CEA";
            have = true;
            break;
        }
    }
    if (!have) {
        snprintf(report, sizeof report, "EDID prefers %s and lists no 50 Hz progressive mode; %ux%u%s left",
                 (edid[54 + 17] & 0x80u) ? "an interlaced mode" : "nothing usable",
                 now.hdisplay, now.vdisplay, now_i);
        return;
    }
    if (how[0] == 'E') {                       /* a rescaled timing: is the panel happy that low? */
        uint32_t min_hz = edid_min_refresh(edid);
        if (min_hz > hz) {
            snprintf(report, sizeof report, "%ux%u: monitor's minimum is %lu Hz, left at %lu.%02lu",
                     want.hdisplay, want.vdisplay, (unsigned long)min_hz,
                     (unsigned long)(now_mhz / 1000u), (unsigned long)((now_mhz % 1000u) / 10u));
            return;
        }
    }

    fw_timing_set(&want);                      /* the caller kicks the watchdog around us */
    fw_timing_t after;
    uint32_t after_mhz = pv_timing_get(&after) ? timing_mhz(&after) : 0u;
    bool ok = after.hdisplay == want.hdisplay && after.vdisplay == want.vdisplay
              && after_mhz + 500u >= hz * 1000u && after_mhz <= hz * 1000u + 500u;
    snprintf(report, sizeof report, "%ux%u %lu Hz from %s: %s, now %ux%u @ %lu.%02lu Hz",
             want.hdisplay, want.vdisplay, (unsigned long)hz, how,
             ok ? "set" : "REFUSED", after.hdisplay, after.vdisplay,
             (unsigned long)(after_mhz / 1000u), (unsigned long)((after_mhz % 1000u) / 10u));
}

const char *display_mode_report(void)
{
    return report;
}

unsigned display_mode_edid(const uint8_t **bytes)
{
    (void)edid_fetch();                  /* on demand: boot reads it only when deciding */
    *bytes = edid_blocks[0];
    return edid_bytes;
}
