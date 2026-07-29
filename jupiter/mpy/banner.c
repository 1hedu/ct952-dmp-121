/* Geometry-PROOF readout of the real OSD stride.
 *
 * Every pixel-addressed probe so far has been unreadable on the panel, because
 * the row-mapping is the very thing that is broken -- so any pattern drawn in
 * (x,y) is garbled by definition, and asking for fine detail from a photo was a
 * bad design. What the user CAN report reliably is colour and rough proportion.
 *
 * So this writes purely in BYTE space: contiguous byte ranges of the OSD buffer.
 * However the hardware maps memory to the screen, a contiguous run of buffer
 * bytes lands as a contiguous run of the raster, so a "fraction of the buffer"
 * reads back as "fraction of the visible area" no matter what the stride is.
 *
 * Encoding: bar length = (VCR23 >> 16) * 32 bytes, filled with index 2, rest of
 * the region index 1. So the fraction of the screen covered by colour-2 reports
 * the stride directly (region = 24024 bytes):
 *      stride   0 (unprogrammed) ->   0%  (no bar at all)
 *      stride 308               ->  41%
 *      stride 360               ->  48%
 *      stride 616               ->  82%
 * Distinguishing none / ~40% / ~half / ~80% by eye is easy, and it needs no
 * legible pixels. Reads VCR23 only; writes no display registers.
 */
#include <stdint.h>

#define APBASE      0x40084000u
#define REGION      (308u * 78u)          /* 24024 bytes, the AP OSD region     */
#define REG_VCR23   (*(volatile uint32_t *)0x80000D8Cu)
#define REG_VCR22   (*(volatile uint32_t *)0x80000D88u)
#define REG_CACHE   (*(volatile uint32_t *)0x80000014u)
#define REG_SYSCFG1 (*(volatile uint32_t *)0x8000031Cu)

#define IDX_BAR  2      /* the "measure me" colour */
#define IDX_BG   1      /* yellow, per the loader's live palette */

static volatile uint8_t *const FB = (volatile uint8_t *)APBASE;

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}

/* fill a byte range with an index duplicated into both 4bpp nibbles, so the run
 * is one flat colour regardless of pixel phase */
static void fill_bytes(uint32_t from, uint32_t to, uint8_t idx){
    uint8_t b = (uint8_t)((idx << 4) | (idx & 0x0F));
    if (to > REGION) to = REGION;
    for (uint32_t i = from; i < to; i++) FB[i] = b;
}

int pyapp_main(void){
    uint32_t stride, bar;

    REG_SYSCFG1 &= ~0x10000000u;           /* keep the watchdog dead */

    stride = REG_VCR23 >> 16;              /* the hardware's own row increment */

    /* Discrete buckets, not a proportional bar: 41% vs 48% is not separable by
     * eye, but none/quarter/half/three-quarters/full is. Each candidate stride
     * gets its own unmistakable bar length. */
    if (stride == 0u)        bar = 0u;                  /* UNPROGRAMMED         */
    else if (stride == 308u) bar = REGION / 4u;         /* quarter              */
    else if (stride == 360u) bar = REGION / 2u;         /* half                 */
    else if (stride == 616u) bar = (REGION / 4u) * 3u;  /* three quarters       */
    else                     bar = REGION;              /* some OTHER value     */

    fill_bytes(0, bar, IDX_BAR);           /* proportional bar */
    fill_bytes(bar, REGION, IDX_BG);       /* remainder */

    flush();
    for (;;){}
    return 0;
}
