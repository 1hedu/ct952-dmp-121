/* Vertical-mapping + stride-verification probe (stride now KNOWN = 308).
 *
 * On-hardware result from the byte-space bucket readout: the bar landed in the
 * QUARTER bucket, so REG_MCU_VCR23 >> 16 == 308 -- the scanout row stride is 308
 * bytes, straight off the chip. That photo also proved the buffer is LINEAR
 * (contiguous byte runs rendered as clean solid bands; a tiled or swizzled layout
 * could not do that), and that the region's content is painted roughly TWICE
 * vertically with black filling the remainder -- i.e. the leftover problem is the
 * VERTICAL mapping, not the pitch. The OSD window is 240 lines
 * (REG_DISP_OSD_SIZE = 0x00f002d0 -> 720x240) while our region is only 78.
 *
 * This probe measures both axes at once, with features big enough to survive a
 * photo:
 *   - LEFT half (x < 240): horizontal bands 6 rows tall, alternating white/yellow
 *     -> counting bands gives the vertical scale and the duplication factor
 *       (13 bands per copy == 1:1; 6-7 == squashed 2x; 26 == doubled).
 *   - RIGHT half (x >= 240): solid yellow.
 *   - The boundary at x=240 is a STRAIGHT VERTICAL EDGE iff the row stride is
 *     right. Any slant/staircase in that edge is the stride error, directly
 *     visible. This is the pixel-space confirmation of stride 308.
 *   - Rows 0..2 are a solid white cap so the top of the region is identifiable.
 *
 * Writes no display registers (reads VCR23 only, falls back to 308).
 */
#include <stdint.h>

#define APBASE      0x40084000u
#define ROWS        78u
#define VIS_W       480         /* the panel's visible width */
#define SPLIT       240         /* left/right boundary -> vertical edge test */
#define BAND        6           /* band height in rows */

#define REG_VCR23   (*(volatile uint32_t *)0x80000D8Cu)
#define REG_CACHE   (*(volatile uint32_t *)0x80000014u)
#define REG_SYSCFG1 (*(volatile uint32_t *)0x8000031Cu)

#define IDX_A  2      /* white  */
#define IDX_B  1      /* yellow */

static volatile uint8_t *const FB = (volatile uint8_t *)APBASE;
static uint32_t g_stride = 308;
static uint32_t g_limit  = 308u * ROWS;

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}

static void px(int x, int y, uint8_t c){
    if (x < 0 || y < 0 || (uint32_t)y >= ROWS) return;
    uint32_t off = (uint32_t)y * g_stride + ((uint32_t)x >> 1);
    if (off >= g_limit) return;
    volatile uint8_t *p = FB + off;
    if (x & 1) *p = (uint8_t)((*p & 0xF0) | (c & 0x0F));
    else       *p = (uint8_t)((*p & 0x0F) | ((c & 0x0F) << 4));
}

int pyapp_main(void){
    uint32_t s;
    REG_SYSCFG1 &= ~0x10000000u;              /* keep the watchdog dead */

    s = REG_VCR23 >> 16;                      /* hardware stride; 308 expected */
    if (s >= 64u && s <= 4096u) g_stride = s;
    g_limit = g_stride * ROWS;
    if (g_limit > 0x5DD8u) g_limit = 0x5DD8u;  /* stay inside the 24024B region */

    for (uint32_t y = 0; y < ROWS; y++) {
        /* left: alternating 6-row bands; right: solid yellow. The x=SPLIT edge
         * is the stride check; the band count is the vertical-mapping check. */
        uint8_t band = ((y / BAND) & 1u) ? IDX_B : IDX_A;
        for (int x = 0; x < SPLIT; x++)      px(x, (int)y, band);
        for (int x = SPLIT; x < VIS_W; x++)  px(x, (int)y, IDX_B);
    }
    for (uint32_t y = 0; y < 3u; y++)         /* white cap = top of region */
        for (int x = 0; x < VIS_W; x++) px(x, (int)y, IDX_A);

    flush();
    for (;;){}
    return 0;
}
