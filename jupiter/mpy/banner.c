/* Measurement grid -- designed to be MEASURED off a photo, not described.
 *
 * We keep failing the blind loop because a pixel-exact problem (stride + the
 * vertical "2 bands") can't be resolved from words. This draws the simplest
 * high-contrast pattern whose distortion encodes the answer:
 *   - solid yellow field (index 1, the loader's live palette)
 *   - 3 thick HORIZONTAL bars (index 2) at rows 8, 36, 64  -> their spacing and
 *     count on-screen shows the vertical mapping: 3 bars once = 1:1; 6 bars or a
 *     split = the "2 bands" (duplication / interlace / line-repeat), and where
 *     they land quantifies it.
 *   - 3 thick VERTICAL bars (index 3) at cols 100, 300, 500 -> if the row stride
 *     is right they are vertical; any slant is the stride error, measurable from
 *     the top-to-bottom horizontal drift.
 * No text, no display-register writes (those either clobber the loader's config
 * or, via GDI, blanked the panel). Just pixels into the loader's region at the
 * best-known stride, for a photo.
 */
#include <stdint.h>

#define APBASE      0x40084000u
#define OSD_W       616
#define OSD_H       78
#define STRIDE      308
#define REGION_END  (STRIDE*OSD_H)

#define REG_CACHE (*(volatile uint32_t *)0x80000014u)

#define C_BG  1   /* yellow  */
#define C_H   2   /* horizontal bars */
#define C_V   3   /* vertical bars   */

static volatile uint8_t *const FB = (volatile uint8_t *)APBASE;

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}
static void px(int x, int y, uint8_t c){
    if ((unsigned)x >= OSD_W || (unsigned)y >= OSD_H) return;
    uint32_t off = (uint32_t)y * STRIDE + (x >> 1);
    if (off >= REGION_END) return;
    volatile uint8_t *p = FB + off;
    if (x & 1) *p = (uint8_t)((*p & 0xF0) | (c & 0x0F));
    else       *p = (uint8_t)((*p & 0x0F) | ((c & 0x0F) << 4));
}
static void hbar(int y0, int h, uint8_t c){ for (int y=y0;y<y0+h;y++) for (int x=0;x<OSD_W;x++) px(x,y,c); }
static void vbar(int x0, int w, uint8_t c){ for (int x=x0;x<x0+w;x++) for (int y=0;y<OSD_H;y++) px(x,y,c); }

int pyapp_main(void){
    for (int y=0;y<OSD_H;y++) for (int x=0;x<OSD_W;x++) px(x,y,C_BG);  /* yellow field */
    hbar(8, 4, C_H); hbar(36, 4, C_H); hbar(64, 4, C_H);              /* 3 h-bars */
    vbar(100, 6, C_V); vbar(300, 6, C_V); vbar(500, 6, C_V);          /* 3 v-bars */
    flush();
    for (;;){}
    return 0;
}
