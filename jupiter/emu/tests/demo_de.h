/*
 * demo_de.h -- program the CT952 display engine from a demo.
 *
 * A demo draws an 8bpp OSD image into DRAM and an ARGB palette; this
 * turns that into the register state real firmware would set, so the
 * emulator's display-engine scanout (machine_scanout) produces the
 * picture the way the hardware read-channel would:
 *   REG_MCU_VCR20     0x80000D80  OSD read-channel base (DRAM byte addr)
 *   REG_DISP_OSD_POS  0x80001A50  OSD window top-left
 *   REG_DISP_OSD_SIZE 0x80001A54  H/V size, bit28 = OSD enable
 *   GAM_OSD RAM       0x80001C00  256 entries, [23:0] YCbCr (BT.601)
 */
#ifndef DEMO_DE_H
#define DEMO_DE_H
#include "jup_types.h"

#define DE_VCR20     (*(volatile uint32_t *)0x80000D80u)
#define DE_OSD_POS   (*(volatile uint32_t *)0x80001A50u)
#define DE_OSD_SIZE  (*(volatile uint32_t *)0x80001A54u)
#define DE_GAM_OSD   ((volatile uint32_t *)0x80001C00u)
#define DE_OSD_EN    0x10000000u

/* BT.601 studio-swing RGB(0xAARRGGBB) -> packed YCbCr (Y<<16|Cb<<8|Cr),
 * the exact format of a GAM_OSD palette entry (integer approximation,
 * matches jup_argb_to_yuv). */
static uint32_t de_rgb2ycbcr(uint32_t argb)
{
    int r = (int)((argb >> 16) & 0xFF);
    int g = (int)((argb >> 8) & 0xFF);
    int b = (int)(argb & 0xFF);
    int Y  = (( 66*r + 129*g +  25*b + 128) >> 8) + 16;
    int Cb = ((-38*r -  74*g + 112*b + 128) >> 8) + 128;
    int Cr = ((112*r -  94*g -  18*b + 128) >> 8) + 128;
    return ((uint32_t)Y << 16) | ((uint32_t)Cb << 8) | (uint32_t)Cr;
}

/* Program the OSD read-channel to scan out `fb_addr` (w x h) through the
 * 256-entry ARGB palette `pal`. */
static void de_program(uint32_t fb_addr, int w, int h, const uint32_t *pal)
{
    int i;
    for (i = 0; i < 256; i++)
        DE_GAM_OSD[i] = de_rgb2ycbcr(pal[i]);
    DE_VCR20   = fb_addr;
    DE_OSD_POS = 0;
    DE_OSD_SIZE = DE_OSD_EN | ((uint32_t)(h & 0x7FF) << 16)
                | (uint32_t)(w & 0x7FF);
}

#endif /* DEMO_DE_H */
