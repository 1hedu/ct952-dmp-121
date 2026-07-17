/*
 * JupiterSDK on CT952 -- hardware layer manager (the DE2-mixer analogue)
 *
 * On the V3s the SDK composits VI0 (game) + VI1 (sprite) + UI0
 * (overlay) in the DE2 hardware. The CT952's display engine composits
 * up to FOUR planes with zero CPU cost, and this module hands the SDK
 * the two that were still idle:
 *
 *   [4] SP2  subpicture plane, 2bpp bitmap mode  <- jlayer
 *   [3] SP1  subpicture plane, 2bpp bitmap mode  <- jlayer
 *   [2] OSD  8bpp indexed                        <- jshim (jvid_*)
 *   [1] video plane, tiled YUV canvas            <- jfb/jcodec
 *
 * Each SP plane: 4 colors, each with its own YUV value AND 16-level
 * contrast (= alpha) -- hardware-blended translucency per color, the
 * thing neither the OSD's global mix nor the blitter can do. The
 * firmware already runs the SPU in raw-bitmap ("BMP") mode for DivX
 * subtitles (char_subpict.c:1716-1729); jlayer parameterizes exactly
 * that recipe (SPU_BMP_Init / SetDisplayArea / SetColorContrast /
 * SetDisplay).
 *
 * Buffers are linear 2bpp (see jdraw2.h). Two idle DRAM areas serve
 * as defaults (the SP-OSD region and the subpicture bitstream buffer,
 * both unused while the Jupiter app owns the screen).
 *
 * HARDWARE-UNVERIFIED: same status as the rest of the platform layer.
 */
#ifndef JLAYER_H
#define JLAYER_H

#include "jup_types.h"

#define JLAYER_SP1  0
#define JLAYER_SP2  1

/* An open SP layer: draw into bmp with jdraw2_* then it just shows
 * (the SPU scans the buffer directly; no present call). */
typedef struct {
    uint8_t *bmp;      /* 2bpp bitmap, 4 px/byte */
    uint32_t pitch;    /* bytes per row (w/4) */
    uint32_t w, h;     /* pixels; w must be a multiple of 4 */
} jlayer_sp_t;

/* Default DRAM bitmap buffer for an SP plane; capacity in bytes is
 * written to *size. (Firmware: SP1 = the SP-OSD region buffer, SP2 =
 * the idle subpicture bitstream buffer.) */
uint8_t *jlayer_sp_buffer(uint8_t sp, uint32_t *size);

/* Open SP plane `sp` as a w x h bitmap at screen position (x,y),
 * scanning the 2bpp buffer at `bmp`. Fills in `layer`. Returns 0. */
int jlayer_sp_open(jlayer_sp_t *layer, uint8_t sp,
                   uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint8_t *bmp);

/* Set one of the plane's 4 colors: ARGB (converted to YUV here) plus
 * alpha 0 (invisible) .. 15 (opaque). */
void jlayer_sp_color(uint8_t sp, uint8_t idx, uint32_t argb,
                     uint8_t alpha_0_15);

/* Show/hide the plane */
void jlayer_sp_show(uint8_t sp, int on);

/* Move an open plane to a new screen position -- hardware scroll of
 * the whole layer, no redraw (re-programs the display area with the
 * stored geometry/buffer). */
void jlayer_sp_move(uint8_t sp, uint16_t x, uint16_t y);

/* Hide and forget the plane */
void jlayer_sp_close(uint8_t sp);

/* OSD plane global alpha over the video plane, 0..63
 * (DISP mix ratio -- the whole-plane knob; per-color translucency is
 * the palette mix bit, see jvid_load_palette/GDI_ChangePALEntry). */
void jlayer_osd_mix(uint8_t ratio_0_63);

#endif /* JLAYER_H */
