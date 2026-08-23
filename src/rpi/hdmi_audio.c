/* hdmi_audio.c - HDMI MAI audio on VideoCore IV. See hdmi_audio.h. */

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "base.h"
#include "rpi.h"
#include "systimer.h"
#include "hdmi_audio.h"

/* ------------------------------------------------------------------ */
/* Registers (BCM2835..2837)                                          */
/* ------------------------------------------------------------------ */

#define HDMI_BASE   (PERIPHERAL_BASE + 0x902000u)   /* HDMI core */
#define HD_BASE     (PERIPHERAL_BASE + 0x808000u)   /* "HD" (MAI, CSC) */
#define CM_BASE     (PERIPHERAL_BASE + 0x101000u)   /* clock manager */

#define REG(base, off) (*(volatile uint32_t *)((base) + (off)))

#define HDMI_MAI_CHANNEL_MAP     REG(HDMI_BASE, 0x090)
#define HDMI_MAI_CONFIG          REG(HDMI_BASE, 0x094)
#define   MAI_CONFIG_BIT_REVERSE     (1u << 26)
#define   MAI_CONFIG_FORMAT_REVERSE  (1u << 27)
#define HDMI_AUDIO_PACKET_CONFIG REG(HDMI_BASE, 0x09C)
#define   APC_ZERO_ON_INACTIVE       (1u << 24)
#define   APC_ZERO_ON_SAMPLE_FLAT    (1u << 29)
#define   APC_B_FRAME_ID_SHIFT       10
#define HDMI_RAM_PACKET_CONFIG   REG(HDMI_BASE, 0x0A0)
#define   RAM_PACKET_AUDIO_ID        (1u << 4)    /* infoframe 0x84 - 0x80 */
#define   RAM_PACKET_ENABLE          (1u << 16)
#define HDMI_RAM_PACKET_STATUS   REG(HDMI_BASE, 0x0A4)
#define HDMI_CRP_CFG             REG(HDMI_BASE, 0x0A8)
#define   CRP_EXTERNAL_CTS_ENABLE    (1u << 24)
#define HDMI_CTS_0               REG(HDMI_BASE, 0x0AC)
#define HDMI_CTS_1               REG(HDMI_BASE, 0x0B0)
#define HDMI_TX_PHY_CTL0         REG(HDMI_BASE, 0x2C4)
#define   TX_PHY_RNG_PWRDN           (1u << 25)
#define HDMI_RAM_PACKET_AUDIO(n) REG(HDMI_BASE, 0x490u + 4u * (n))   /* 9 words */

#define HD_MAI_CTL               REG(HD_BASE, 0x014)
#define   MAI_CTL_RESET              (1u << 0)
#define   MAI_CTL_ERRORF             (1u << 1)
#define   MAI_CTL_ERRORE             (1u << 2)
#define   MAI_CTL_ENABLE             (1u << 3)
#define   MAI_CTL_CHNUM_SHIFT        4
#define   MAI_CTL_FLUSH              (1u << 9)
#define   MAI_CTL_FULL               (1u << 11)
#define   MAI_CTL_WHOLSMP            (1u << 12)
#define   MAI_CTL_CHALIGN            (1u << 13)
#define   MAI_CTL_DLATE              (1u << 15)
#define HD_MAI_THR               REG(HD_BASE, 0x018)
#define HD_MAI_FMT               REG(HD_BASE, 0x01C)
#define   MAI_FMT_SAMPLE_RATE_SHIFT  8
#define   MAI_FMT_AUDIO_FORMAT_SHIFT 16
#define   MAI_FMT_PCM                2u
#define HD_MAI_DATA_OFF          0x020u
#define HD_MAI_SMP               REG(HD_BASE, 0x02C)

#define CM_HSMDIV                REG(CM_BASE, 0x08C)
#define A2W_PLLH_CTRLR           REG(CM_BASE, 0x1960)
#define A2W_PLLH_FRACR           REG(CM_BASE, 0x1A60)
#define A2W_PLLH_ANA1            REG(CM_BASE, 0x1074)
#define   PLLH_FB_PREDIV             (1u << 11)

#define XOSC_HZ   19200000u
#define PLLD_HZ   500000000u

/* IEC958 consumer channel status, first 40 bits (the rest are zero) */
#define IEC958_FRAMES_PER_BLOCK 192u
#define IEC958_B_PREAMBLE       0x0Fu

static struct {
    bool     running;
    uint32_t rate;
    uint32_t hsm_hz, pixel_hz;
    uint8_t  status[5];
    uint32_t frame;                  /* 0..191 within the channel-status block */
} hd;

/* ------------------------------------------------------------------ */
/* Clocks                                                             */
/* ------------------------------------------------------------------ */

/* HSM = PLLD / (CM_HSMDIV as 4.8 fixed point) - the MAI's reference */
static uint32_t hsm_clock_hz(void)
{
    uint32_t div = CM_HSMDIV;
    div >>= 12 - 8;                  /* the 12.12 field has 4.8 populated */
    div &= (1u << 12) - 1u;
    if (!div)
        return 0;
    return (uint32_t)(((uint64_t)PLLD_HZ << 8) / div);
}

/* Pixel clock = PLLH / 10, PLLH from its NDIV/FDIV/PDIV registers */
static uint32_t pixel_clock_hz(void)
{
    uint32_t ctrl = A2W_PLLH_CTRLR;
    uint32_t fdiv = A2W_PLLH_FRACR & ((1u << 20) - 1u);
    uint32_t ndiv = ctrl & 0x3FFu;
    uint32_t pdiv = (ctrl & 0x7000u) >> 12;
    if (A2W_PLLH_ANA1 & PLLH_FB_PREDIV) {
        ndiv *= 2;
        fdiv *= 2;
    }
    if (!pdiv)
        return 0;
    uint64_t rate = (uint64_t)XOSC_HZ * ((ndiv << 20) + fdiv);
    rate /= pdiv;
    rate >>= 20;
    return (uint32_t)(rate / 10u);
}

/* Best n/d <= limits for num/den: continued fractions (Linux rational.c
   algorithm, as Circle carries it). */
static void best_rational(uint32_t num, uint32_t den, uint32_t max_n, uint32_t max_d,
                          uint32_t *best_n, uint32_t *best_d)
{
    uint32_t n = num, d = den, n0 = 0, d0 = 1, n1 = 1, d1 = 0, n2, d2;
    for (;;) {
        if (d == 0)
            break;
        uint32_t dp = d, a = n / d;
        d = n % d;
        n = dp;
        n2 = n0 + a * n1;
        d2 = d0 + a * d1;
        if (n2 > max_n || d2 > max_d) {
            uint32_t t1 = (max_n - n0) / n1, t2 = (max_d - d0) / d1;
            uint32_t t = t1 < t2 ? t1 : t2;
            if (2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
                n1 = n0 + t * n1;
                d1 = d0 + t * d1;
            }
            break;
        }
        n0 = n1; n1 = n2; d0 = d1; d1 = d2;
    }
    *best_n = n1;
    *best_d = d1;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                           */
/* ------------------------------------------------------------------ */

static uint32_t mai_rate_code(uint32_t rate)
{
    static const uint32_t rates[] = { 8000, 11025, 12000, 16000, 22050, 24000,
                                      32000, 44100, 48000, 64000, 88200, 96000,
                                      128000, 176400, 192000 };
    for (uint32_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (rates[i] == rate)
            return i + 1u;
    return 0;                        /* not indicated */
}

static uint8_t iec958_fs_code(uint32_t rate)
{
    switch (rate) {
    case 44100: return 0x0;
    case 48000: return 0x2;
    case 32000: return 0x3;
    default:    return 0x1;          /* not indicated: the sink measures it */
    }
}

static bool wait_bit(volatile uint32_t *reg, uint32_t mask, bool set, uint32_t ms)
{
    while (((*reg & mask) != 0) != set) {
        usleep(1000);
        if (--ms == 0)
            return false;
    }
    return true;
}

static bool set_audio_infoframe(void)
{
    HDMI_RAM_PACKET_CONFIG &= ~RAM_PACKET_AUDIO_ID;
    if (!wait_bit(&HDMI_RAM_PACKET_STATUS, RAM_PACKET_AUDIO_ID, false, 100))
        return false;

    /* Audio infoframe: type 0x84, version 1, length 10; then checksum and
       "2 channels, refer to stream header" - everything else zero. The
       checksum makes the header+payload sum to zero mod 256. */
    HDMI_RAM_PACKET_AUDIO(0) = 0x0A0184u;
    HDMI_RAM_PACKET_AUDIO(1) = 0x0170u;
    for (uint32_t i = 2; i <= 8; i++)
        HDMI_RAM_PACKET_AUDIO(i) = 0;

    HDMI_RAM_PACKET_CONFIG |= RAM_PACKET_AUDIO_ID;
    return wait_bit(&HDMI_RAM_PACKET_STATUS, RAM_PACKET_AUDIO_ID, true, 100);
}

bool hdmi_audio_start(uint32_t rate)
{
    if (hd.running && hd.rate == rate)
        return true;
    if (hd.running)
        hdmi_audio_stop();

    /* The firmware only enables packets when it has decided the link is
       HDMI rather than DVI - with an EDID that claims audio, or
       hdmi_drive=2. Without packets there is no audio path at all. */
    if (!(HDMI_RAM_PACKET_CONFIG & RAM_PACKET_ENABLE)) {
        LOG_INFO("hdmi audio: display is DVI / no HDMI packets (set hdmi_drive=2)\r\n");
        return false;
    }

    hd.hsm_hz = hsm_clock_hz();
    hd.pixel_hz = pixel_clock_hz();
    if (!hd.hsm_hz || !hd.pixel_hz) {
        LOG_INFO("hdmi audio: cannot read clocks (hsm %"PRIu32" pixel %"PRIu32")\r\n",
                 hd.hsm_hz, hd.pixel_hz);
        return false;
    }

    uint32_t n, d;
    best_rational(hd.hsm_hz, rate, 0xFFFFFFu, 0x100u, &n, &d);

    uint32_t r128 = rate * 128u;
    uint32_t cts_n = r128 / 1000u;
    uint32_t cts = (uint32_t)(((uint64_t)hd.pixel_hz * cts_n) / r128);

    const struct { volatile uint32_t *reg; uint32_t val; } writes[] = {
        { &HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF },
        { &HD_MAI_SMP, (n << 8) | (d - 1u) },
        { &HD_MAI_FMT, (mai_rate_code(rate) << MAI_FMT_SAMPLE_RATE_SHIFT) |
                       (MAI_FMT_PCM << MAI_FMT_AUDIO_FORMAT_SHIFT) },
        { &HD_MAI_THR, (16u << 24) | (16u << 16) | (16u << 8) | 16u },
        { &HDMI_MAI_CONFIG, MAI_CONFIG_BIT_REVERSE | MAI_CONFIG_FORMAT_REVERSE | 0x3u },
        { &HDMI_MAI_CHANNEL_MAP, 0x08u },
        { &HDMI_AUDIO_PACKET_CONFIG, APC_ZERO_ON_SAMPLE_FLAT | APC_ZERO_ON_INACTIVE |
                                     (IEC958_B_PREAMBLE << APC_B_FRAME_ID_SHIFT) | 0x3u },
        { &HDMI_CRP_CFG, CRP_EXTERNAL_CTS_ENABLE | cts_n },
        { &HDMI_CTS_0, cts },
        { &HDMI_CTS_1, cts },
    };
    for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]); i++)
        *writes[i].reg = writes[i].val;

    if (!set_audio_infoframe())
        LOG_INFO("hdmi audio: audio infoframe not accepted\r\n");

    /* IEC958 channel status: consumer, PCM, copy permitted, no
       pre-emphasis; general category; source 0; rate; 24-bit word */
    hd.status[0] = 0x04;
    hd.status[1] = 0x00;
    hd.status[2] = 0x00;
    hd.status[3] = iec958_fs_code(rate);
    hd.status[4] = 0x0B;
    hd.frame = 0;

    /* power up the audio clock generator, then go */
    HDMI_TX_PHY_CTL0 &= ~TX_PHY_RNG_PWRDN;
    HD_MAI_CTL = (2u << MAI_CTL_CHNUM_SHIFT) | MAI_CTL_WHOLSMP | MAI_CTL_CHALIGN | MAI_CTL_ENABLE;

    hd.rate = rate;
    hd.running = true;
    LOG_INFO("hdmi audio: %"PRIu32" Hz, hsm %"PRIu32" pixel %"PRIu32" smp %"PRIu32"/%"PRIu32" n %"PRIu32" cts %"PRIu32"\r\n",
             rate, hd.hsm_hz, hd.pixel_hz, n, d, cts_n, cts);
    return true;
}

void hdmi_audio_stop(void)
{
    if (!hd.running)
        return;
    HD_MAI_CTL = MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF;
    HDMI_TX_PHY_CTL0 |= TX_PHY_RNG_PWRDN;
    hd.running = false;
}

uint32_t hdmi_audio_fifo_bus(void)
{
    return ((HD_BASE + HD_MAI_DATA_OFF) & 0x00FFFFFFu) | PERIPHERAL_BASE_GPU;
}

/* ------------------------------------------------------------------ */
/* Subframes                                                          */
/* ------------------------------------------------------------------ */

static inline uint32_t parity32(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    return (0x6996u >> (x & 0xFu)) & 1u;
}

static inline uint32_t subframe(int16_t s, uint32_t cbit, bool block_start)
{
    /* 24-bit sample in bits 4..27 (our 16 bits at the top), channel
       status in bit 30, even parity over 4..30 in bit 31, and the
       B preamble code in bits 0..3 on the first frame of a block - the
       MAI recognises it from AUDIO_PACKET_CONFIG's B frame identifier. */
    uint32_t w = ((uint32_t)(uint16_t)s << 12) & 0x0FFFFFF0u;
    w |= cbit << 30;
    w |= parity32(w) << 31;
    if (block_start)
        w |= IEC958_B_PREAMBLE;
    return w;
}

void hdmi_audio_pack(int16_t l, int16_t r, uint32_t *out)
{
    uint32_t f = hd.frame;
    uint32_t cbit = (f < 40u) ? (((uint32_t)hd.status[f >> 3] >> (f & 7u)) & 1u) : 0u;
    out[0] = subframe(l, cbit, f == 0);
    out[1] = subframe(r, cbit, f == 0);
    hd.frame = (f + 1u) % IEC958_FRAMES_PER_BLOCK;
}

/* ------------------------------------------------------------------ */

uint32_t hdmi_audio_hsm_hz(void)   { return hd.hsm_hz; }
uint32_t hdmi_audio_pixel_hz(void) { return hd.pixel_hz; }
uint32_t hdmi_audio_mai_ctl(void)  { return HD_MAI_CTL; }
