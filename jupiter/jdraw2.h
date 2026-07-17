/*
 * JupiterSDK on CT952 -- 2bpp drawing helpers for subpicture layers.
 *
 * SP-plane bitmaps are linear 2bpp, 4 pixels per byte, leftmost pixel
 * in the high bits (the GDI 2-bit region convention: byte offset =
 * (y*width + x) >> 2, matching gdi.c's color-mode shift; in-byte order
 * follows the 4bpp high-nibble-left convention -- INFERRED, flagged in
 * the README). Color indices are 0..3, mapped to YUV + 16-level alpha
 * by jlayer_sp_color(). Index 0 is conventionally the transparent one
 * (alpha 0), like DVD subpictures.
 *
 * Pure C over caller buffers -- host-testable.
 */
#ifndef JDRAW2_H
#define JDRAW2_H

#include "jup_types.h"

/* Set one pixel (ci 0..3) */
static void jdraw2_set(uint8_t *bmp, uint32_t pitch,
                       uint32_t x, uint32_t y, uint8_t ci)
{
    uint8_t *b = bmp + y * pitch + (x >> 2);
    uint32_t sh = (3 - (x & 3)) * 2;
    *b = (uint8_t)((*b & ~(3u << sh)) | ((ci & 3u) << sh));
}

static uint8_t jdraw2_get(const uint8_t *bmp, uint32_t pitch,
                          uint32_t x, uint32_t y)
{
    uint32_t sh = (3 - (x & 3)) * 2;
    return (uint8_t)((bmp[y * pitch + (x >> 2)] >> sh) & 3);
}

/* Fill a rect with one index */
static void jdraw2_fill(uint8_t *bmp, uint32_t pitch,
                        uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint8_t ci)
{
    uint32_t r, c;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            jdraw2_set(bmp, pitch, x + c, y + r, ci);
}

/* Clear a whole buffer to one index fast (byte-replicated) */
static void jdraw2_clear(uint8_t *bmp, uint32_t pitch, uint32_t h,
                         uint8_t ci)
{
    uint32_t n = pitch * h, i;
    uint8_t v = (uint8_t)((ci & 3) * 0x55);   /* 4 pixels per byte */
    for (i = 0; i < n; i++)
        bmp[i] = v;
}

/* Rectangle outline, thickness t */
static void jdraw2_frame(uint8_t *bmp, uint32_t pitch,
                         uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t t, uint8_t ci)
{
    jdraw2_fill(bmp, pitch, x, y, w, t, ci);
    jdraw2_fill(bmp, pitch, x, y + h - t, w, t, ci);
    jdraw2_fill(bmp, pitch, x, y, t, h, ci);
    jdraw2_fill(bmp, pitch, x + w - t, y, t, h, ci);
}

#endif /* JDRAW2_H */
