/* Self-calibrating banner: READ the true OSD stride from the hardware instead of
 * guessing it, and report the raw registers as a photo-readable bit pattern.
 *
 * RE result (dp700wd.bin, real DISP code at file offset ~0xa5760, located by
 * register footprint -- DVD909.sym is misaligned with this binary and must not be
 * used): the OSD read channel is REG_MCU_VCR20..23 @ 0x80000D80..D8C, and
 *
 *   VCR22 = (height << 16) | (width_in_X_increments)
 *   VCR23 = (Y_increment << 16) | X_increment
 *
 * where the Y increment IS the scanout row stride in bytes. The AP loader's
 * GDI_InitialRegion -> DISP_OSDSet path programs these before jumping to us, so
 * we can simply READ the stride rather than guess it (308 vs 360 vs 616 was the
 * whole problem). Fallback 308 only if the channel reads back unprogrammed.
 *
 * We touch NO display registers (writing VCR20/OSD_SIZE clobbers the loader's
 * working config -- established earlier). Read-only + pixels.
 *
 * Layout: a solid top bar (proof of render), then three 32-bit readouts drawn as
 * chunky blocks (MSB left, filled = 1) for VCR20, VCR22, VCR23. Blocks survive a
 * wrong stride far better than 8px text, so one photo yields the real register
 * values even in the failure case. 448px wide so it fits the panel's visible 480.
 */
#include <stdint.h>

#define APBASE      0x40084000u
#define REG_VCR20   (*(volatile uint32_t *)0x80000D80u)
#define REG_VCR22   (*(volatile uint32_t *)0x80000D88u)
#define REG_VCR23   (*(volatile uint32_t *)0x80000D8Cu)
#define REG_CACHE   (*(volatile uint32_t *)0x80000014u)
#define REG_SYSCFG1 (*(volatile uint32_t *)0x8000031Cu)

/* loader's live palette (empirical): 1 = yellow, 2/3 = black/white */
#define C_BG  1
#define C_ON  2
#define C_OFF 3

static volatile uint8_t *const FB = (volatile uint8_t *)APBASE;

static uint32_t g_stride = 308;   /* set from VCR23 at runtime */
static uint32_t g_rows   = 78;
static uint32_t g_limit  = 308u * 78u;   /* hard write cap, recomputed */

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}

static void px(int x, int y, uint8_t c){
    if (x < 0 || y < 0 || (uint32_t)y >= g_rows) return;
    uint32_t off = (uint32_t)y * g_stride + ((uint32_t)x >> 1);
    if (off >= g_limit) return;                     /* never leave the region */
    volatile uint8_t *p = FB + off;
    if (x & 1) *p = (uint8_t)((*p & 0xF0) | (c & 0x0F));
    else       *p = (uint8_t)((*p & 0x0F) | ((c & 0x0F) << 4));
}

static void box(int x0, int y0, int w, int h, uint8_t c){
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) px(x, y, c);
}

/* 32 bits as blocks, MSB at the left: filled = 1, hollow = 0 */
static void bits32(int y0, uint32_t v){
    const int cw = 14, ch = 12;
    for (int b = 0; b < 32; b++) {
        int x0 = b * cw;
        uint32_t bit = (v >> (31 - b)) & 1u;
        if (bit) {
            box(x0 + 1, y0, cw - 2, ch, C_ON);          /* solid = 1 */
        } else {
            box(x0 + 1, y0,          cw - 2, 2, C_OFF); /* hollow = 0 */
            box(x0 + 1, y0 + ch - 2, cw - 2, 2, C_OFF);
        }
    }
}

int pyapp_main(void){
    uint32_t vcr20, vcr22, vcr23, s, r;

    REG_SYSCFG1 &= ~0x10000000u;          /* keep the watchdog dead */

#ifdef BANNER_SELFPROG
    /* EMULATOR-TEST ONLY (never compiled into the hardware AP): stand in for the
     * firmware's DISP setup by programming the OSD channel exactly as the real
     * code at ~0xa5760 does, for W=616 H=78 CM=1 SEL=0, so the emulator's
     * register-derived scanout and this reader can be validated end to end.
     *   VCR22 = (H<<16) | (W >> (CM+2)) ; VCR23 = ((W << (16-CM)) & 0xFFFF0000)+4 */
    {
        const uint32_t W = 616u, H = 78u, CM = 1u;
        REG_VCR20 = APBASE;
        REG_VCR22 = (H << 16) | (W >> (CM + 2));
        REG_VCR23 = ((W << (16 - CM)) & 0xFFFF0000u) + 4u;
    }
#endif
    vcr20 = REG_VCR20; vcr22 = REG_VCR22; vcr23 = REG_VCR23;

    /* the whole point: take the stride from the hardware */
    s = vcr23 >> 16;
    if (s >= 64u && s <= 4096u) g_stride = s;         /* sane -> trust it */
    r = vcr22 >> 16;
    if (r >= 16u && r <= 1024u) g_rows = r;
    g_limit = g_stride * g_rows;
    if (g_limit > 0x18000u) g_limit = 0x18000u;       /* cap at ~96KB of DRAM */

    /* background + a solid top bar: unmistakable "we rendered" marker */
    for (uint32_t y = 0; y < g_rows; y++)
        for (int x = 0; x < 448; x++) px(x, (int)y, C_BG);
    box(0, 0, 448, 6, C_ON);

    bits32(10, vcr20);        /* OSD base            */
    bits32(28, vcr22);        /* (height<<16)|width  */
    bits32(46, vcr23);        /* (stride<<16)|xinc   */

    flush();
    for (;;){}
    return 0;
}
