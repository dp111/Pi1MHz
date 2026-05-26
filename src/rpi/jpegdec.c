/*
 * Fast Software Baseline JPEG / MJPEG Decoder
 * Optimised for ARM1176JZF-S (Raspberry Pi 1, 700 MHz, no NEON).
 *
 * Key optimisations:
 *   - 8-bit flat lookup Huffman tables (single table hit for >95% of symbols)
 *   - ARM assembly 2-D IDCT with zero-AC block shortcut (jpegdec_idct.S)
 *   - Merged dequantisation during coefficient decode
 *   - Minimal branching in the hot decode loop
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "rpi.h"
#include "jpegdec.h"

/* ------------------------------------------------------------------ */
/* JPEG markers                                                       */
/* ------------------------------------------------------------------ */
#define M_SOI  0xD8
#define M_EOI  0xD9
#define M_SOF0 0xC0
#define M_DHT  0xC4
#define M_DQT  0xDB
#define M_SOS  0xDA
#define M_DRI  0xDD
#define M_RST0 0xD0

/* ------------------------------------------------------------------ */
/* Limits                                                             */
/* ------------------------------------------------------------------ */
#define MAX_COMP  3
#define HUFF_FAST_BITS 8
#define HUFF_FAST_SIZE (1 << HUFF_FAST_BITS)   /* 256 */

/* ------------------------------------------------------------------ */
/* Zig-zag order                                                      */
/* ------------------------------------------------------------------ */
static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ------------------------------------------------------------------ */
/* Huffman table (fast 8-bit + slow fallback)                         */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Fast path: 8-bit lookup.  fast_val[code] = symbol | (codelen << 8).
       If codelen == 0, the code is longer than 8 bits → use slow path. */
    uint16_t fast[HUFF_FAST_SIZE];

    /* Slow path arrays (codes >8 bits) */
    uint8_t  bits[17];           /* bits[i] = number of codes of length i */
    uint8_t  symbols[256];
    int      maxcode[17];
    int      valptr[17];
} huff_t;

/* ------------------------------------------------------------------ */
/* Component descriptor                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  id;
    uint8_t  hfact, vfact;
    uint8_t  qtab;              /* quant-table index */
    uint8_t  dc_idx, ac_idx;   /* Huffman table indices */
    int      dc_pred;
} comp_t;

/* ------------------------------------------------------------------ */
/* Decoder state (static — no malloc)                                 */
/* ------------------------------------------------------------------ */
static struct {
    const uint8_t *src;
    size_t         len;
    size_t         pos;

    /* Bit reader */
    uint32_t       bbuf;
    int            bbits;

    /* Image */
    uint16_t       w, h;
    uint8_t        ncomp;
    comp_t         comp[MAX_COMP];
    uint8_t        max_h, max_v;

    /* Quantisation tables (pre-scaled for row IDCT: val << 11 or val) */
    int16_t        qtab[4][64];

    /* Huffman tables: 0–1 DC, 2–3 AC */
    huff_t         huff[4];

    uint16_t       restart_interval;

    /* Current 8×8 coefficient block (dequantised, zig-zag ordered).
       128 elements (256 bytes) so the asm IDCT row pass can widen
       int16→int32 in-place without overflowing the buffer. */
    int16_t        block[128] __attribute__((aligned(4)));

    /* Dirty-zero: positions of non-zero AC coefficients from last block.
       After IDCT, we zero only these instead of memset(128). */
    uint8_t        nzpos[64];
    int            nzcount;
} dec;

/* ------------------------------------------------------------------ */
/* ARM assembly IDCT — defined in jpegdec_idct.S                      */
/*   Operates in-place on 64 int16 coefficients, writes 8×8 uint8     */
/*   to output buffer at given stride. Adds +128 bias and clamps.     */
/* ------------------------------------------------------------------ */
extern void jpeg_idct_ifast(int16_t *coef, uint8_t *out, int stride);

/* ------------------------------------------------------------------ */
/* Byte / bit reader                                                  */
/* ------------------------------------------------------------------ */

static inline uint8_t rd8(void)
{
    if (dec.pos < dec.len)
        return dec.src[dec.pos++];
    return 0;
}

static inline uint16_t rd16(void)
{
    uint16_t v = rd8();
    return (uint16_t)((v << 8) | rd8());
}

/*
 * Fill bit buffer to >= 25 bits.  Handles JPEG byte-stuffing (FF 00)
 * and silently absorbs restart markers.
 */
static void fill_bits(void)
{
    while (dec.bbits <= 24) {
        uint8_t b;
        if (dec.pos >= dec.len) {
            dec.bbuf <<= 8;
            dec.bbits += 8;
            continue;
        }
        b = dec.src[dec.pos++];
        if (b == 0xFF) {
            uint8_t m = (dec.pos < dec.len) ? dec.src[dec.pos++] : 0;
            if (m == 0x00)
                b = 0xFF;               /* stuffed FF */
            else if ((m >= 0xD0) && (m <= 0xD7))
                b = 0;                  /* restart marker — pad with zero */
            else {
                dec.pos -= 2;           /* unexpected marker — back off */
                b = 0;
            }
        }
        dec.bbuf = (dec.bbuf << 8) | b;
        dec.bbits += 8;
    }
}

static inline int get_bits(int n)
{
    if (dec.bbits < n)
        fill_bits();
    dec.bbits -= n;
    return (int)((dec.bbuf >> dec.bbits) & ((1u << n) - 1));
}

/* ------------------------------------------------------------------ */
/* Build Huffman table with fast 8-bit lookup                         */
/* ------------------------------------------------------------------ */

static void build_huff(huff_t *h)
{
    int code = 0, si = 0;

    /* Clear fast table */
    memset(h->fast, 0, sizeof(h->fast));

    for (int i = 1; i <= 16; i++) {
        h->valptr[i] = si;
        for (int j = 0; j < h->bits[i]; j++) {
            if (i <= HUFF_FAST_BITS) {
                /* Replicate into all fast-table entries that share this prefix */
                int spread = HUFF_FAST_BITS - i;
                int base = code << spread;
                for (int k = 0; k < (1 << spread); k++)
                    h->fast[base + k] =
                        (uint16_t)(h->symbols[si] | ((unsigned)i << 8));
            }
            code++;
            si++;
        }
        h->maxcode[i] = code - 1;
        code <<= 1;
    }
}

/* ------------------------------------------------------------------ */
/* Huffman decode — fast path / slow path                             */
/* ------------------------------------------------------------------ */

static inline int huff_decode(huff_t *h)
{
    if (dec.bbits < HUFF_FAST_BITS)
        fill_bits();

    /* Peek top 8 bits */
    int peek = (int)((dec.bbuf >> (dec.bbits - HUFF_FAST_BITS))
                     & (HUFF_FAST_SIZE - 1));
    uint16_t fv = h->fast[peek];
    int clen = fv >> 8;

    if (clen) {
        /* Fast hit */
        dec.bbits -= clen;
        return fv & 0xFF;
    }

    /* Slow path: code is > 8 bits */
    int code = peek;
    int bits = HUFF_FAST_BITS;
    while (bits < 16) {
        bits++;
        code = (code << 1) | ((dec.bbuf >> (dec.bbits - bits)) & 1);
        if (code <= h->maxcode[bits]) {
            dec.bbits -= bits;
            return h->symbols[h->valptr[bits] + code - (h->maxcode[bits] - h->bits[bits] + 1)];
        }
    }
    dec.bbits -= 16;
    return 0;
}

/* Receive & extend a VLC value of 'n' bits */
static inline int receive_extend(int n)
{
    int v = get_bits(n);
    if (v < (1 << (n - 1)))
        v += (-1 << n) + 1;
    return v;
}

/* ------------------------------------------------------------------ */
/* Marker parsers                                                     */
/* ------------------------------------------------------------------ */

static int parse_dqt(void)
{
    int len = rd16() - 2;
    while (len > 0) {
        uint8_t info = rd8();
        int prec = info >> 4;
        int id   = info & 0x0F;
        if (id > 3) return -1;
        len--;
        for (int i = 0; i < 64; i++) {
            int v = prec ? (int)rd16() : (int)rd8();
            dec.qtab[id][zigzag[i]] = (int16_t)v;
        }
        len -= prec ? 128 : 64;
    }
    return 0;
}

static int parse_sof0(void)
{
    rd16();                    /* length */
    if (rd8() != 8) return -1; /* 8-bit only */
    dec.h = rd16();
    dec.w = rd16();
    dec.ncomp = rd8();
    if (dec.ncomp > MAX_COMP) return -1;
    dec.max_h = 0;
    dec.max_v = 0;
    for (int i = 0; i < dec.ncomp; i++) {
        dec.comp[i].id = rd8();
        uint8_t sf = rd8();
        dec.comp[i].hfact = sf >> 4;
        dec.comp[i].vfact = sf & 0x0F;
        dec.comp[i].qtab  = rd8();
        if (dec.comp[i].hfact > dec.max_h) dec.max_h = dec.comp[i].hfact;
        if (dec.comp[i].vfact > dec.max_v) dec.max_v = dec.comp[i].vfact;
    }
    return 0;
}

static int parse_dht(void)
{
    int len = rd16() - 2;
    while (len > 0) {
        uint8_t info = rd8();
        int cls = info >> 4;        /* 0 = DC, 1 = AC */
        int id  = info & 0x0F;
        if (id > 1) return -1;
        len--;
        int idx = cls * 2 + id;
        huff_t *h = &dec.huff[idx];
        int total = 0;
        for (int i = 1; i <= 16; i++) {
            h->bits[i] = rd8();
            total += h->bits[i];
        }
        len -= 16;
        for (int i = 0; i < total && i < 256; i++)
            h->symbols[i] = rd8();
        len -= total;
        build_huff(h);
    }
    return 0;
}

static int parse_sos(void)
{
    rd16();
    int n = rd8();
    for (int i = 0; i < n; i++) {
        uint8_t cid = rd8();
        uint8_t td_ta = rd8();
        for (int c = 0; c < dec.ncomp; c++) {
            if (dec.comp[c].id == cid) {
                dec.comp[c].dc_idx = td_ta >> 4;
                dec.comp[c].ac_idx = td_ta & 0x0F;
                break;
            }
        }
    }
    rd8(); rd8(); rd8();  /* Ss, Se, Ah|Al */
    return 0;
}

static void parse_dri(void)
{
    rd16();
    dec.restart_interval = rd16();
}

/* ------------------------------------------------------------------ */
/* Decode one 8×8 block of DCT coefficients                          */
/* Returns number of non-zero AC coefficients (0 = DC-only block)     */
/* ------------------------------------------------------------------ */

static int decode_block(comp_t *c)
{
    huff_t *dc_h = &dec.huff[c->dc_idx];
    huff_t *ac_h = &dec.huff[2 + c->ac_idx];
    int16_t *qt  = dec.qtab[c->qtab];
    int16_t *blk = dec.block;

    /* Dirty-zero: clear only the coefficients set by the previous block.
       blk[0] (DC) is always written, so always clear it. */
    blk[0] = 0;
    for (int i = 0; i < dec.nzcount; i++)
        blk[dec.nzpos[i]] = 0;
    dec.nzcount = 0;

    /* DC */
    int dc_len = huff_decode(dc_h);
    if (dc_len) {
        c->dc_pred += receive_extend(dc_len);
    }
    blk[0] = (int16_t)(c->dc_pred * qt[0]);

    /* AC — track positions of non-zero coefficients for dirty-zero */
    int nzcount = 0;
    int k = 1;
    while (k < 64) {
        int sym = huff_decode(ac_h);
        int run  = sym >> 4;
        int size = sym & 0x0F;
        if (size == 0) {
            if (run == 0)  break;       /* EOB */
            if (run == 15) { k += 16; continue; }  /* ZRL */
            break;
        }
        k += run;
        if (k >= 64) break;
        int pos = zigzag[k];
        int val = receive_extend(size);
        blk[pos] = (int16_t)(val * qt[pos]);
        dec.nzpos[nzcount] = (uint8_t)pos;
        nzcount++;
        k++;
    }
    dec.nzcount = nzcount;
    return nzcount;
}

/* ------------------------------------------------------------------ */
/* Decode scan data (entropy-coded segment)                           */
/* ------------------------------------------------------------------ */

static int decode_scan(uint8_t *yuv)
{
    uint8_t *y_plane  = yuv;
    uint8_t *cr_plane = yuv + JPEG_WIDTH * JPEG_HEIGHT;
    uint8_t *cb_plane = cr_plane + (JPEG_WIDTH / 2) * JPEG_HEIGHT;

    int mcu_w = dec.max_h * 8;
    int mcu_h = dec.max_v * 8;
    int mcus_x = (dec.w + mcu_w - 1) / mcu_w;
    int mcus_y = (dec.h + mcu_h - 1) / mcu_h;
    int y_stride = JPEG_WIDTH;
    int c_stride = JPEG_WIDTH / 2;

    dec.bbuf  = 0;
    dec.bbits = 0;
    int rst_cnt = 0;

    for (int my = 0; my < mcus_y; my++) {
        for (int mx = 0; mx < mcus_x; mx++) {

            /* Restart handling */
            if (dec.restart_interval && rst_cnt == dec.restart_interval) {
                rst_cnt = 0;
                dec.bbits = 0;
                dec.bbuf  = 0;
                /* Consume restart marker */
                if (dec.pos < dec.len && dec.src[dec.pos] == 0xFF)
                    dec.pos += 2;
                for (int c = 0; c < dec.ncomp; c++)
                    dec.comp[c].dc_pred = 0;
            }

            for (int c = 0; c < dec.ncomp; c++) {
                comp_t *cp = &dec.comp[c];
                for (int v = 0; v < cp->vfact; v++) {
                    for (int h = 0; h < cp->hfact; h++) {
                        decode_block(cp);

                        if (c == 0) {
                            /* Luma */
                            int bx = mx * dec.max_h + h;
                            int by = my * dec.max_v + v;
                            if (bx * 8 >= JPEG_WIDTH || by * 8 >= JPEG_HEIGHT)
                                continue;
                            uint8_t *dst = y_plane + by * 8 * y_stride + bx * 8;
                            jpeg_idct_ifast(dec.block, dst, y_stride);
                        } else {
                            /* Chroma — 4:2:2 output, no downsampling */
                            uint8_t *cplane = (c == 1) ? cr_plane : cb_plane;
                            int bx = mx * cp->hfact + h;
                            int by = my * cp->vfact + v;
                            if (bx * 8 >= JPEG_WIDTH / 2 || by * 8 >= JPEG_HEIGHT)
                                continue;
                            uint8_t *dst = cplane + by * 8 * c_stride + bx * 8;
                            jpeg_idct_ifast(dec.block, dst, c_stride);
                        }
                    }
                }
            }
            rst_cnt++;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* In-place int16 IDCT (C fallback — currently unused, kept for       */
/* reference / future chroma downsample path if needed)               */
/* ------------------------------------------------------------------ */
#if 0

#define CW1 2841
#define CW2 2676
#define CW3 2408
#define CW5 1609
#define CW6 1108
#define CW7  565

static void idct_row(int *row)
{
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;

    if (!((x1 = row[4] << 11) | (x2 = row[6]) | (x3 = row[2]) |
          (x4 = row[1]) | (x5 = row[7]) | (x6 = row[5]) | (x7 = row[3]))) {
        int v = row[0] << 3;
        row[0]=row[1]=row[2]=row[3]=row[4]=row[5]=row[6]=row[7]=v;
        return;
    }

    x0 = (row[0] << 11) + 128;
    x8 = CW7*(x4+x5);     x4 = x8 + (CW1-CW7)*x4; x5 = x8 - (CW1+CW7)*x5;
    x8 = CW3*(x6+x7);     x6 = x8 - (CW3-CW5)*x6; x7 = x8 - (CW3+CW5)*x7;
    x8=x0+x1; x0-=x1;
    x1 = CW6*(x3+x2);     x2 = x1 - (CW2+CW6)*x2; x3 = x1 + (CW2-CW6)*x3;
    x1=x4+x6; x4-=x6; x6=x5+x7; x5-=x7;
    x7=x8+x3; x8-=x3; x3=x0+x2; x0-=x2;
    x2 = (181*(x4+x5)+128)>>8;
    x4 = (181*(x4-x5)+128)>>8;
    row[0]=(x7+x1)>>8; row[1]=(x3+x2)>>8; row[2]=(x0+x4)>>8; row[3]=(x8+x6)>>8;
    row[4]=(x8-x6)>>8; row[5]=(x0-x4)>>8; row[6]=(x3-x2)>>8; row[7]=(x7-x1)>>8;
}

static void idct_col(int *blk, int col)
{
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;

    if (!((x1 = blk[32+col] << 8) | (x2 = blk[48+col]) | (x3 = blk[16+col]) |
          (x4 = blk[8+col]) | (x5 = blk[56+col]) | (x6 = blk[40+col]) |
          (x7 = blk[24+col]))) {
        int v = (blk[col] + 32) >> 6;
        blk[col]=blk[8+col]=blk[16+col]=blk[24+col]=v;
        blk[32+col]=blk[40+col]=blk[48+col]=blk[56+col]=v;
        return;
    }
    x0 = (blk[col] << 8) + 8192;
    x8 = CW7*(x4+x5)+4; x4=(x8+(CW1-CW7)*x4)>>3; x5=(x8-(CW1+CW7)*x5)>>3;
    x8 = CW3*(x6+x7)+4; x6=(x8-(CW3-CW5)*x6)>>3; x7=(x8-(CW3+CW5)*x7)>>3;
    x8=x0+x1; x0-=x1;
    x1 = CW6*(x3+x2)+4; x2=(x1-(CW2+CW6)*x2)>>3; x3=(x1+(CW2-CW6)*x3)>>3;
    x1=x4+x6; x4-=x6; x6=x5+x7; x5-=x7;
    x7=x8+x3; x8-=x3; x3=x0+x2; x0-=x2;
    x2=(181*(x4+x5)+128)>>8; x4=(181*(x4-x5)+128)>>8;
    blk[col]    =(x7+x1)>>14; blk[8+col] =(x3+x2)>>14;
    blk[16+col]=(x0+x4)>>14; blk[24+col]=(x8+x6)>>14;
    blk[32+col]=(x8-x6)>>14; blk[40+col]=(x0-x4)>>14;
    blk[48+col]=(x3-x2)>>14; blk[56+col]=(x7-x1)>>14;
}

void idct_block_inplace(int16_t *block)
{
    int ws[64];
    for (int i = 0; i < 64; i++) ws[i] = block[i];
    for (int i = 0; i < 8; i++) idct_row(ws + i * 8);
    for (int i = 0; i < 8; i++) idct_col(ws, i);
    for (int i = 0; i < 64; i++) block[i] = (int16_t)ws[i];
}

#endif /* C IDCT fallback */

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void jpegdec_init(void)
{
    LOG_DEBUG("jpegdec: software JPEG decoder (ARM1176 optimised)\r\n");
}

int jpegdec_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                   uint8_t *yuv_output)
{
    if (!jpeg_data || jpeg_size < 2 || !yuv_output)
        return -1;

    memset(&dec, 0, sizeof(dec));
    dec.src = jpeg_data;
    dec.len = jpeg_size;

    /* Verify SOI */
    if (rd8() != 0xFF || rd8() != M_SOI)
        return -1;

    int sos_found = 0;
    while (dec.pos < dec.len && !sos_found) {
        if (rd8() != 0xFF) continue;
        uint8_t marker;
        do { marker = rd8(); } while (marker == 0xFF);
        if (marker == 0 || marker == M_EOI) break;

        switch (marker) {
        case M_SOF0: if (parse_sof0() < 0) return -1; break;
        case M_DHT:  if (parse_dht()  < 0) return -1; break;
        case M_DQT:  if (parse_dqt()  < 0) return -1; break;
        case M_DRI:  parse_dri(); break;
        case M_SOS:  if (parse_sos()  < 0) return -1; sos_found = 1; break;
        default:
            /* Skip APP / COM / unknown */
            if (marker >= 0xC0) {
                uint16_t len = rd16();
                if (len > 2) dec.pos += len - 2;
            }
            break;
        }
    }

    if (!sos_found || dec.w == 0 || dec.h == 0)
        return -1;

    /* Clear output to black (Y=0, Cb=Cr=128) — 4:2:2 planar */
    memset(yuv_output, 0, JPEG_WIDTH * JPEG_HEIGHT);
    memset(yuv_output + JPEG_WIDTH * JPEG_HEIGHT, 0x80,
           (JPEG_WIDTH / 2) * JPEG_HEIGHT * 2);

    return decode_scan(yuv_output);
}
