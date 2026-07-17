/*
 * JupiterSDK on CT952 -- video-plane tiled-YUV canvas implementation.
 * Swizzle math mirrors the active CT909S path of GDI_FBDrawDot
 * (gdi.c:3548-3572) exactly; see jfb.h for the layout.
 */
#include "jfb.h"

uint32_t jfb_y_offset(const jfb_t *fb, uint32_t x, uint32_t y)
{
    return (y >> 4) * fb->strip + (x >> 2) * 64 + (y & 15) * 4 + (x & 3);
}

uint32_t jfb_uv_offset(const jfb_t *fb, uint32_t cx, uint32_t cy)
{
    return (cy >> 4) * fb->strip + (cx >> 3) * 256 +
           ((cx & 7) >> 2) * 64 + (cy & 15) * 4 + (cx & 3);
}

void jfb_set_y(const jfb_t *fb, uint32_t x, uint32_t y, uint8_t Y)
{
    fb->y_base[jfb_y_offset(fb, x, y)] = Y;
}

void jfb_set_uv(const jfb_t *fb, uint32_t x, uint32_t y,
                uint8_t U, uint8_t V)
{
    uint32_t off = jfb_uv_offset(fb, x >> 1, y >> 1);
    fb->c_base[off] = U;
    fb->c_base[off + 128] = V;
}

uint8_t jfb_get_y(const jfb_t *fb, uint32_t x, uint32_t y)
{
    return fb->y_base[jfb_y_offset(fb, x, y)];
}

void jfb_get_uv(const jfb_t *fb, uint32_t x, uint32_t y,
                uint8_t *U, uint8_t *V)
{
    uint32_t off = jfb_uv_offset(fb, x >> 1, y >> 1);
    if (U) *U = fb->c_base[off];
    if (V) *V = fb->c_base[off + 128];
}

void jfb_dot(const jfb_t *fb, uint32_t x, uint32_t y, uint32_t yuv)
{
    uint8_t Y = (uint8_t)(yuv >> 16);
    x &= ~1u;
    y &= ~1u;
    jfb_set_y(fb, x, y, Y);
    jfb_set_y(fb, x + 1, y, Y);
    jfb_set_y(fb, x, y + 1, Y);
    jfb_set_y(fb, x + 1, y + 1, Y);
    jfb_set_uv(fb, x, y, (uint8_t)(yuv >> 8), (uint8_t)yuv);
}

void jfb_fill(const jfb_t *fb, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h, uint32_t yuv)
{
    uint32_t r, c;
    for (r = 0; r < h; r += 2)
        for (c = 0; c < w; c += 2)
            jfb_dot(fb, x + c, y + r, yuv);
}

void jfb_blit_indexed(const jfb_t *fb, uint32_t dx, uint32_t dy,
                      const uint8_t *src, uint32_t sw, uint32_t sh,
                      uint32_t src_pitch, const uint32_t *pal_yuv)
{
    uint32_t r, c;
    for (r = 0; r < sh; r++) {
        const uint8_t *row = src + r * src_pitch;
        uint32_t fy = dy + r;
        for (c = 0; c < sw; c++) {
            uint32_t col = pal_yuv[row[c]];
            jfb_set_y(fb, dx + c, fy, (uint8_t)(col >> 16));
            /* one chroma sample per 2x2 quad, from its top-left pixel */
            if (((r & 1) == 0) && ((c & 1) == 0))
                jfb_set_uv(fb, dx + c, fy,
                           (uint8_t)(col >> 8), (uint8_t)col);
        }
    }
}
