/*
 * JupiterSDK on CT952 -- 8bpp drawing helpers
 *
 * Replaces the Jupiter SDK's inline draw_rect/clear_rect (jupiter.h) and
 * the NEON sprite blit rows (sprite_neon.S) with plain-C equivalents
 * operating on the CT952's 8-bit palette-indexed OSD buffer.
 *
 * Color values are OSD palette indices. Index 0 is the OSD's transparent
 * color, so "clear" means "punch through to video" on this hardware.
 */
#ifndef JDRAW_H
#define JDRAW_H

#include "jup_types.h"

/* Fill a rectangle with a palette index. No clipping -- the caller
 * guarantees the rect fits (same contract as the SDK's draw_rect). */
static void jdraw_rect(uint8_t *fb, uint32_t pitch,
                       int x, int y, int w, int h, uint8_t color)
{
    int r, c;
    for (r = y; r < y + h; r++) {
        uint8_t *row = fb + (uint32_t)r * pitch + (uint32_t)x;
        for (c = 0; c < w; c++)
            row[c] = color;
    }
}

/* Fill the whole buffer with one palette index */
static void jdraw_clear(uint8_t *fb, uint32_t pitch,
                        uint32_t w, uint32_t h, uint8_t color)
{
    jdraw_rect(fb, pitch, 0, 0, (int)w, (int)h, color);
}

/* Color-keyed sprite blit (C replacement for sprite_blit +
 * _sprite_row_keyed): copies src except where src == key. Clips against
 * the fb_w x fb_h bounds. src is 8bpp palette-indexed, src_w x src_h. */
static void jdraw_blit_keyed(uint8_t *fb, uint32_t pitch,
                             uint32_t fb_w, uint32_t fb_h,
                             const uint8_t *src, uint32_t src_w, uint32_t src_h,
                             int dx, int dy, uint8_t key)
{
    int x0 = 0, y0 = 0;
    int w = (int)src_w, h = (int)src_h;
    int r, c;

    if (dx < 0) { x0 = -dx; }
    if (dy < 0) { y0 = -dy; }
    if (dx + w > (int)fb_w) w = (int)fb_w - dx;
    if (dy + h > (int)fb_h) h = (int)fb_h - dy;

    for (r = y0; r < h; r++) {
        const uint8_t *s = src + (uint32_t)r * src_w;
        uint8_t *drow = fb + (uint32_t)(dy + r) * pitch;
        for (c = x0; c < w; c++) {
            uint8_t px = s[c];
            if (px != key)
                drow[dx + c] = px;
        }
    }
}

#endif /* JDRAW_H */
