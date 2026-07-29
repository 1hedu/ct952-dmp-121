/* ONE-SHOT stride finder: test 8 candidate row pitches simultaneously.
 *
 * Where we are: byte-space fills render as clean solid bands (so the buffer is
 * linear and memory order maps monotonically to raster order), but drawing in
 * (x,y) with pitch 308 produced DIAGONAL edges -- so the hardware's line pitch is
 * NOT 308, even though REG_MCU_VCR23>>16 reads 308. A uniform fill cannot reveal a
 * pitch error, which is why the earlier byte-space probe looked fine. Leading
 * suspect: the OSD window is 720px wide (REG_DISP_OSD_SIZE=0x00f002d0), which at
 * 4bpp is 360 bytes per display line, not the region's 308.
 *
 * Rather than binary-search one candidate per flash, this tests 8 at once. The
 * region is split into 8 equal byte zones (3003 B each). Zone k gets a column of
 * short white marks placed every P[k] BYTES. Because memory order maps
 * monotonically to raster order, marks spaced by exactly the true pitch land on
 * consecutive display lines at the SAME column -> a clean straight vertical line.
 * Any other spacing staggers them into a diagonal or scatter.
 *
 * Each zone is preceded by a thin full-width yellow separator so the zones can be
 * counted from the top. The single question to answer is an ordinal:
 *   "counting zones from the top, which one's white marks form a STRAIGHT
 *    VERTICAL line?"  -> that zone's P[k] is the true stride.
 *
 * Everything is addressed in BYTE space (no assumed pitch anywhere), so the probe
 * itself cannot be distorted by the unknown it is measuring. Writes no display
 * registers.
 */
#include <stdint.h>

#define APBASE   0x40084000u
#define REGION   24024u                 /* 308*78, the AP OSD region in bytes   */
#define NZONE    8u
#define ZONE     (REGION / NZONE)       /* 3003 bytes per zone                  */

#define REG_CACHE   (*(volatile uint32_t *)0x80000014u)
#define REG_SYSCFG1 (*(volatile uint32_t *)0x8000031Cu)

#define IDX_MARK 2      /* white  */
#define IDX_SEP  1      /* yellow */

/* candidate row pitches in bytes, low -> high, one per zone (top -> bottom).
 * 308 = region width at 4bpp; 360 = 720px window at 4bpp; the rest bracket them. */
/* Pass 2. Pass 1 swept 308..384 and NO zone lined up, and the photo's content
 * height (24024 B rendered over ~83 panel lines) implies a pitch near 290 -- i.e.
 * the whole first sweep sat ABOVE the true value. This sweep brackets 290 in
 * 4-byte steps. Marks are also 24 B (48 px) wide now, so a correct candidate
 * renders as a SOLID VERTICAL BAR (consecutive marks abut) while a wrong one
 * breaks into a staircase -- far easier to judge than aligned thin dashes. */
static const uint16_t P[NZONE] = { 276, 280, 284, 288, 292, 296, 300, 304 };

static volatile uint8_t *const FB = (volatile uint8_t *)APBASE;

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}

static void fill_bytes(uint32_t from, uint32_t n, uint8_t idx){
    uint8_t b = (uint8_t)((idx << 4) | (idx & 0x0F));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = from + i;
        if (o >= REGION) return;
        FB[o] = b;
    }
}

int pyapp_main(void){
    REG_SYSCFG1 &= ~0x10000000u;            /* keep the watchdog dead */

    fill_bytes(0, REGION, 0);               /* clear region to transparent */

    for (uint32_t k = 0; k < NZONE; k++) {
        uint32_t z = k * ZONE;
        uint32_t start = z + 300u, rem, delta;
        fill_bytes(z, 300u, IDX_SEP);       /* thin yellow separator, ~1 line */
        /* Phase-align the column to byte 60 of a row *under this candidate*, so
         * that IF P[k] is the true pitch the marks sit at x=120 -- comfortably
         * inside the panel's visible 480px (240 bytes). Without this the column
         * for some candidates lands past the visible edge and the straight line
         * would be invisible even when the candidate is right. */
        rem   = start % P[k];
        delta = (40u + P[k] - rem) % P[k];
        start += delta;
        for (uint32_t off = start; off < z + ZONE; off += P[k])
            fill_bytes(off, 24u, IDX_MARK); /* 24 B = 48 px: abuts into a SOLID
                                            * vertical bar at the true pitch */
    }

    flush();
    for (;;){}
    return 0;
}
