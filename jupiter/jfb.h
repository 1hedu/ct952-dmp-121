/*
 * JupiterSDK on CT952 -- video-plane framebuffer canvas (jfb)
 *
 * The CT952 analogue of the Jupiter SDK's Cedar pixel-format helpers
 * (cedar_argb_to_nv12 / cedar_nv12_to_argb): CPU access to the video
 * plane's tiled YUV 4:2:0 framebuffers, so renderers can target the
 * full-color 704x480 video layer instead of (or underneath) the
 * 256-color OSD.
 *
 * Format (the active CT909S/909P path of GDI_FBDrawDot, gdi.c:3548-3572):
 *   - Separate Y and C (chroma) buffers, block-tiled, NOT linear.
 *   - Y: 8bpp, tiled in 4-px-wide x 16-line blocks of 64 bytes:
 *       off = (y>>4)*strip + (x>>2)*64 + (y&15)*4 + (x&3)
 *   - C: 4:2:0 (one U,V pair per 2x2 pixel quad), chroma coords
 *     cx=x/2, cy=y/2, tiled in 8-px-wide x 16-line blocks of 256
 *     bytes, U and V split 128 bytes apart:
 *       off = (cy>>4)*strip + (cx>>3)*256 + ((cx&7)>>2)*64
 *             + (cy&15)*4 + (cx&3)
 *       U at off+0, V at off+128
 *   - strip = bytes per 16-line block row. On the CT909P the firmware
 *     derives it as ((REG_DISP_STRIPE & 0xFF) << 8) / 4 (gdi.c:3553-3557).
 *
 * Colors here are 0x00YYUUVV words (same as the OSD palette after
 * jup_argb_to_yuv), so one palette conversion serves both planes.
 *
 * This module is pure C over a caller-supplied descriptor -- portable
 * and host-testable. The firmware side fills the descriptor from
 * __DISPFrameInfo[] + REG_DISP_STRIPE (see jcodec.h).
 */
#ifndef JFB_H
#define JFB_H

#include "jup_types.h"

/* Canvas descriptor: one video framebuffer */
typedef struct {
    uint8_t *y_base;     /* luma buffer base */
    uint8_t *c_base;     /* chroma buffer base */
    uint32_t strip;      /* bytes per 16-line block row (Y and C) */
    uint32_t w, h;       /* usable canvas size (<= 704x480) */
} jfb_t;

/* Byte offset of pixel (x,y) in the Y buffer */
uint32_t jfb_y_offset(const jfb_t *fb, uint32_t x, uint32_t y);

/* Byte offset of the U sample for chroma coords (cx,cy) = (x/2, y/2);
 * the V sample lives at the returned offset + 128. */
uint32_t jfb_uv_offset(const jfb_t *fb, uint32_t cx, uint32_t cy);

/* Write one pixel's luma (1-px granularity) */
void jfb_set_y(const jfb_t *fb, uint32_t x, uint32_t y, uint8_t Y);

/* Write the chroma of the 2x2 quad containing (x,y) */
void jfb_set_uv(const jfb_t *fb, uint32_t x, uint32_t y,
                uint8_t U, uint8_t V);

/* Read back (for tests and effects) */
uint8_t jfb_get_y(const jfb_t *fb, uint32_t x, uint32_t y);
void jfb_get_uv(const jfb_t *fb, uint32_t x, uint32_t y,
                uint8_t *U, uint8_t *V);

/* 2x2 dot in one color (the GDI_FBDrawDot equivalent; x,y even) */
void jfb_dot(const jfb_t *fb, uint32_t x, uint32_t y, uint32_t yuv);

/* Fill a rect with one 0x00YYUUVV color (x,y,w,h even) */
void jfb_fill(const jfb_t *fb, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h, uint32_t yuv);

/* Blit an 8-bit indexed image through a 0x00YYUUVV palette:
 * per-pixel luma, per-quad chroma (from each quad's top-left pixel).
 * dx,dy must be even; sw,sh even; caller guarantees fit. This is the
 * bridge from the jnes/jgb/jsnes/jgen renderers to the video plane. */
void jfb_blit_indexed(const jfb_t *fb, uint32_t dx, uint32_t dy,
                      const uint8_t *src, uint32_t sw, uint32_t sh,
                      uint32_t src_pitch, const uint32_t *pal_yuv);

#endif /* JFB_H */
