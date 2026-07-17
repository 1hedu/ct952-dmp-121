/*
 * JupiterSDK on CT952 -- ARGB8888 -> CT952 OSD palette color conversion
 *
 * The CT952 OSD palette holds 32-bit entries in 0x00YYUUVV layout
 * (BT.601: Y in bits[23:16], Cb/U in [15:8], Cr/V in [7:0], chroma
 * centered at 0x80). The Jupiter SDK's palettes are ARGB8888, so every
 * color the SDK loads passes through this converter once at init time.
 */
#ifndef JRGB2YUV_H
#define JRGB2YUV_H

#include "jup_types.h"

/* Convert one ARGB8888 (0xAARRGGBB) color to a CT952 OSD palette word
 * (0x00YYUUVV, BT.601 studio range: Y 16..235, U/V 16..240).
 * Alpha is ignored -- OSD transparency is palette-index 0, not per-pixel. */
uint32_t jup_argb_to_yuv(uint32_t argb);

/* Convert a whole ARGB palette into YUV palette words. */
void jup_palette_to_yuv(const uint32_t *argb, uint32_t *yuv, int count);

#endif /* JRGB2YUV_H */
