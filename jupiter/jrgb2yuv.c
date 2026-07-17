/*
 * JupiterSDK on CT952 -- ARGB8888 -> 0x00YYUUVV (BT.601) conversion.
 * Integer-only (the SPARC target has no FPU worth using: -msoft-float).
 */
#include "jrgb2yuv.h"

/* BT.601 studio-range RGB->YCbCr, Q8 fixed point:
 *   Y  =  16 + ( 65.738*R + 129.057*G +  25.064*B) / 256
 *   Cb = 128 + (-37.945*R -  74.494*G + 112.439*B) / 256
 *   Cr = 128 + (112.439*R -  94.154*G -  18.285*B) / 256
 */
uint32_t jup_argb_to_yuv(uint32_t argb)
{
    int32_t r = (int32_t)((argb >> 16) & 0xFF);
    int32_t g = (int32_t)((argb >> 8) & 0xFF);
    int32_t b = (int32_t)(argb & 0xFF);

    int32_t y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    int32_t u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    int32_t v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);

    if (y < 16) y = 16;
    if (y > 235) y = 235;
    if (u < 16) u = 16;
    if (u > 240) u = 240;
    if (v < 16) v = 16;
    if (v > 240) v = 240;

    return ((uint32_t)y << 16) | ((uint32_t)u << 8) | (uint32_t)v;
}

void jup_palette_to_yuv(const uint32_t *argb, uint32_t *yuv, int count)
{
    int i;
    for (i = 0; i < count; i++)
        yuv[i] = jup_argb_to_yuv(argb[i]);
}
