/*
 * ct952emu display-engine test: drive the DISP OSD plane through the
 * real hardware register interface, so the emulator's modelled scan-out
 * (machine_disp_scanout) renders it -- proving the display model works
 * via registers, not a host-side DRAM snapshot.
 *
 * Steps, exactly what a MicroPython HAL or the booted firmware would do
 * to put pixels on the panel:
 *   1. load the OSD colour palette into the DISP GAM_OSD RAM
 *      (0x80001C00, one 0x00YYUUVV BT.601 word per entry);
 *   2. enable the OSD (DISP_OSD_EN in REG_DISP_OSD_SIZE, 0x80001A54);
 *   3. paint an 8bpp test card into the firmware OSD plane at
 *      DS_OSDFRAME_ST (0x4005F000): 8 colour bars over a 64-step grey
 *      ramp.
 *
 * Freestanding: no .data (link.ld forbids it) -- palette and pixels are
 * computed at runtime and written through volatile MMIO/DRAM pointers.
 */
#include "testapi.h"

#define OSD_BASE      0x4005F000u   /* DS_OSDFRAME_ST */
#define OSD_W         616u
#define OSD_H         440u
#define GAM_OSD       0x80001C00u   /* REG_DISP_GAM_OSD(n) = +n*4 */
#define REG_OSD_POS   0x80001A50u   /* REG_DISP_OSD_POS */
#define REG_OSD_SIZE  0x80001A54u   /* REG_DISP_OSD_SIZE, bit28 = EN */
#define DISP_OSD_EN   0x10000000u

/* BT.601 studio-range ARGB -> 0x00YYUUVV, matching jup_argb_to_yuv. */
static unsigned argb2yuv(unsigned argb)
{
    int r = (int)((argb >> 16) & 0xFF);
    int g = (int)((argb >> 8) & 0xFF);
    int b = (int)(argb & 0xFF);
    int y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    int u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    int v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
    if (y < 16) y = 16; if (y > 235) y = 235;
    if (u < 16) u = 16; if (u > 240) u = 240;
    if (v < 16) v = 16; if (v > 240) v = 240;
    return ((unsigned)y << 16) | ((unsigned)u << 8) | (unsigned)v;
}

unsigned testmain(void)
{
    volatile unsigned *gam = (volatile unsigned *)GAM_OSD;
    volatile unsigned char *fb = (volatile unsigned char *)OSD_BASE;
    unsigned bars[8];
    unsigned i, x, y;

    /* palette entries 0..7: 8 colour bars */
    bars[0] = 0xFFFFFF; bars[1] = 0xFFFF00; bars[2] = 0x00FFFF;
    bars[3] = 0x00FF00; bars[4] = 0xFF00FF; bars[5] = 0xFF0000;
    bars[6] = 0x0000FF; bars[7] = 0x000000;
    for (i = 0; i < 8; i++)
        gam[i] = argb2yuv(bars[i]);

    /* palette entries 16..79: 64-step grey ramp */
    for (i = 0; i < 64; i++) {
        unsigned s = (i * 255) / 63;
        gam[16 + i] = argb2yuv((s << 16) | (s << 8) | s);
    }

    /* turn the OSD plane on and set its window geometry */
    *(volatile unsigned *)REG_OSD_POS = 0;
    *(volatile unsigned *)REG_OSD_SIZE = DISP_OSD_EN | (OSD_H << 16) | OSD_W;

    /* paint: top 300 rows = colour bars, bottom = grey ramp */
    for (y = 0; y < OSD_H; y++)
        for (x = 0; x < OSD_W; x++) {
            unsigned idx = (y < 300) ? (x * 8) / OSD_W
                                     : 16 + (x * 64) / OSD_W;
            fb[y * OSD_W + x] = (unsigned char)idx;
        }

    return 0;
}
