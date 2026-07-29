/* SELF-IDENTIFYING pitch confirmation: draw the candidate pitch, as large digits,
 * USING that candidate as the row pitch -- in three stacked zones.
 *
 * Whichever number is LEGIBLE is the true pitch: the digits only come out
 * readable when the pitch used to address them matches the hardware's line pitch;
 * a wrong pitch shears them into diagonal hash. So the readout names its own
 * answer, with no measuring, counting, or ordinal-reporting required.
 *
 * How we got here (all on-hardware):
 *   - sweep 308..384: no zone lined up  -> true pitch is BELOW 308, killing both
 *     the 308 (region-width) and 360 (720px-window) hypotheses.
 *   - region height measurement: 24024 B over ~83 panel lines -> pitch ~= 290.
 *   - sweep 276..304 with fat 48px marks: the SOLID vertical bar landed in zone 5
 *     -> pitch = 292. Consistent with ~290, and a multiple of 4 as expected since
 *     VCR23's low half (the X increment) is 4.
 * So 292 is the answer unless this test says otherwise; 288 and 296 are its
 * neighbours and are included as the control.
 *
 * Digits are drawn at 3x scale (24 px tall) so they survive a phone photo.
 * Each zone's base is rounded UP to a multiple of its own candidate pitch, so its
 * rows start at a real line boundary. Writes no display registers.
 */
#include <stdint.h>

#define APBASE   0x40084000u
#define REGION   24024u
#define NZONE    3u
#define ZONE     (REGION / NZONE)        /* 8008 bytes per zone */
#define SCALE    3

#define REG_CACHE   (*(volatile uint32_t *)0x80000014u)
#define REG_SYSCFG1 (*(volatile uint32_t *)0x8000031Cu)

#define IDX_TXT  2      /* white  */
#define IDX_SEP  1      /* yellow */

static const uint16_t CAND[NZONE] = { 288, 292, 296 };

/* 8x8 digits 0-9, bit 0 = leftmost pixel */
static const uint8_t D[10][8] = {
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
};

static volatile uint8_t *const FB = (volatile uint8_t *)APBASE;

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00400000u; REG_CACHE |= 0x00040000u;
}

static void put(uint32_t off, uint8_t idx, int hi){
    if (off >= REGION) return;
    if (hi) FB[off] = (uint8_t)((FB[off] & 0x0F) | (idx << 4));
    else    FB[off] = (uint8_t)((FB[off] & 0xF0) | (idx & 0x0F));
}

/* pixel within a zone, addressed with that zone's candidate pitch */
static void zpx(uint32_t base, uint32_t pitch, int x, int y, uint8_t idx, uint32_t cap){
    uint32_t off;
    if (x < 0 || y < 0) return;
    off = base + (uint32_t)y * pitch + ((uint32_t)x >> 1);
    if (off >= cap) return;
    put(off, idx, !(x & 1));
}

static void digit(uint32_t base, uint32_t pitch, int x0, int y0, int d, uint32_t cap){
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (D[d][r] & (1u << c))
                for (int sy = 0; sy < SCALE; sy++)
                    for (int sx = 0; sx < SCALE; sx++)
                        zpx(base, pitch, x0 + c*SCALE + sx, y0 + r*SCALE + sy, IDX_TXT, cap);
}

int pyapp_main(void){
    REG_SYSCFG1 &= ~0x10000000u;              /* keep the watchdog dead */

    for (uint32_t i = 0; i < REGION; i++) FB[i] = 0;   /* transparent */

    for (uint32_t k = 0; k < NZONE; k++) {
        uint32_t p    = CAND[k];
        uint32_t z    = k * ZONE;
        uint32_t base = ((z + p - 1u) / p) * p;         /* align to a line start */
        uint32_t cap  = (k + 1u) * ZONE;
        uint32_t v    = p;
        int dg[3], n = 0, x;
        if (cap > REGION) cap = REGION;

        /* thin separator so the three zones are visually distinct */
        for (uint32_t i = z; i < z + 240u && i < REGION; i++)
            FB[i] = (uint8_t)((IDX_SEP << 4) | IDX_SEP);

        while (v && n < 3) { dg[n++] = (int)(v % 10u); v /= 10u; }
        x = 40;
        for (int j = n - 1; j >= 0; j--) {            /* digits, MSD first */
            digit(base, p, x, 4, dg[j], cap);
            x += 8*SCALE + 6;
        }
    }

    flush();
    for (;;){}
    return 0;
}
