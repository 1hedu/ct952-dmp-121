/*
 * JupiterSDK on CT952 -- SNES-style background + sprite renderer
 * (indexed-8bpp port)
 *
 * Ported from Jupiter SDK lib/snes.c + include/snes.h.
 * All 8 SNES BG modes (0-7), 2/4/8bpp tiles, per-tile-column offset
 * (modes 2/4/6), 128 sprites (32/line), Mode 7 affine ground.
 *
 * Differences from the original:
 *   - Output is one 8-bit palette-indexed framebuffer (CT952 OSD
 *     region format). The bottom BG renders opaque (backdrop where
 *     empty); upper BGs composite front-to-back with transparent-skip
 *     instead of writing to a separate hardware-blended overlay.
 *   - Palettes are arrays of OSD palette INDICES (uint8_t), not
 *     ARGB8888 (convert colors with jup_argb_to_yuv() and load them
 *     into the OSD palette first).
 *   - Mode 7's NEON scanline (mode7_neon.S) and NEON line-double
 *     memcpy are replaced with plain C.
 *   - Renders a fixed SNES_NATIVE_W x SNES_NATIVE_H rect at (x0,y0)
 *     (jnes/jgb convention; caller pre-clears the pillarbox).
 *
 * Tile/tilemap/sprite formats are identical to the original:
 *
 * Tile pixel formats:
 *   2bpp: 4px/byte  -- 16 bytes/tile  -- 4 colors/palette
 *   4bpp: 2px/byte  -- 32 bytes/tile  -- 16 colors/palette
 *   8bpp: 1px/byte  -- 64 bytes/tile  -- 256 colors/palette
 *
 * Tilemap entry (16-bit):
 *   [9:0]   tile index (0-1023, 0 = transparent/empty)
 *   [12:10] palette number (0-7)
 *   [13]    priority (not modeled)
 *   [14]    flip X
 *   [15]    flip Y
 */
#ifndef JSNES_H
#define JSNES_H

#include "jup_types.h"

#define SNES_NATIVE_W 256
#define SNES_NATIVE_H 224

/* Tilemap entry macros */
#define SNES_TILE(idx)        ((idx) & 0x3FF)
#define SNES_PAL(p)           (((p) & 7) << 10)
#define SNES_PRIO             (1 << 13)
#define SNES_FLIPX            (1 << 14)
#define SNES_FLIPY            (1 << 15)

#define SNES_ENTRY(idx, pal, pri, fx, fy) \
    (SNES_TILE(idx) | SNES_PAL(pal) | ((pri) ? SNES_PRIO : 0) | \
     ((fx) ? SNES_FLIPX : 0) | ((fy) ? SNES_FLIPY : 0))

#define SNES_GET_TILE(e)   ((e) & 0x3FF)
#define SNES_GET_PAL(e)    (((e) >> 10) & 7)
#define SNES_GET_PRIO(e)   (((e) >> 13) & 1)
#define SNES_GET_FLIPX(e)  (((e) >> 14) & 1)
#define SNES_GET_FLIPY(e)  (((e) >> 15) & 1)

/* Background layer descriptor.
 * palette: OSD palette indices; the tile's palette number selects a
 * block of 4/16/256 entries depending on bpp (same indexing as the
 * SDK's ARGB palette). */
typedef struct {
    const uint8_t  *tiles;       /* tile pixel data (packed) */
    const uint16_t *map;         /* tilemap: map_w x map_h entries */
    const uint8_t  *palette;     /* OSD palette indices */
    int32_t scroll_x, scroll_y;
    uint16_t map_w, map_h;       /* must be power of 2 */
    uint8_t bpp;                 /* 2, 4, or 8 */
    uint8_t enabled;
} snes_bg_t;

/* Per-tile-column offset table (Modes 2, 4, 6). One entry per screen
 * tile column (SNES_NATIVE_W/8 = 32 entries). */
typedef struct {
    const int16_t *col_offset;
    uint8_t vertical;            /* 0 = applies to scroll_x, 1 = scroll_y */
} snes_tile_offset_t;

/* === Mode compositors ===
 * All render a SNES_NATIVE_W x SNES_NATIVE_H rect at (x0,y0) into the
 * 8bpp buffer. backdrop is an OSD palette index. Layering per mode
 * matches the SDK (BG1 front ... BGn back, first opaque pixel wins). */

/* Mode 0: BG1+BG2+BG3 over BG4, all 2bpp */
void snes_mode0_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_bg_t *bg3, const snes_bg_t *bg4);

/* Mode 1: BG1(4bpp)+BG2(4bpp) over BG3(2bpp) */
void snes_mode1_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_bg_t *bg3);

/* Mode 2: BG1(4bpp, per-tile-column offset) over BG2(4bpp) */
void snes_mode2_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_tile_offset_t *ofs);

/* Mode 3: BG1(8bpp) over BG2(4bpp) */
void snes_mode3_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2);

/* Mode 4: BG1(8bpp, offset) over BG2(2bpp) */
void snes_mode4_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_tile_offset_t *ofs);

/* Mode 5: BG1(4bpp) over BG2(2bpp) */
void snes_mode5_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2);

/* Mode 6: BG1(4bpp, offset) alone */
void snes_mode6_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1,
                        const snes_tile_offset_t *ofs);

/* === Mode 7: affine ground projection ===
 * Textured ground plane with perspective and optional per-scanline
 * twist. The sky region above `horizon` is left untouched. Rendered
 * at half vertical resolution with line doubling (plain C here). */
typedef struct {
    int32_t  cam_x, cam_y;   /* camera in world coords */
    uint8_t  angle;          /* yaw, 0..255 = 0..360 degrees */
    uint8_t  twist;          /* 0 = rigid plane; >0 = vortex swirl */
    uint16_t horizon;        /* y in the rect where the ground starts */
    uint16_t space_z;        /* depth scale; 8000 is a good default */
    const uint8_t *map;      /* texture, tile-indexed into palette */
    const uint8_t *palette;  /* 256 OSD palette indices */
    uint8_t  map_w_bits;     /* log2(map width) */
    uint8_t  map_mask;       /* map width - 1 */
} snes_mode7_t;

void snes_mode7_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0,
                        const snes_mode7_t *m7);

/* Shared sin/cos LUTs in Q12, indexed 0..255. */
const int32_t *snes_sin_lut(void);
const int32_t *snes_cos_lut(void);

/* === Sprites ===
 * 4bpp tiles (32 bytes/tile, planar pairs: rows 0-7 planes 0-1 then
 * planes 2-3), column-major multi-tile layout (tile + tx*h + ty),
 * 8 palettes x 16 colors, 32 sprites/scanline limit. */
typedef struct {
    int16_t  x, y;           /* rect-relative; may be partially off-screen */
    uint16_t tile;
    uint8_t  w, h;           /* size in tiles (1-8) */
    uint8_t  pal;            /* palette 0-7 */
    uint8_t  priority;       /* 0-3 (not modeled) */
    uint8_t  fliph, flipv;
    uint8_t  enabled;
} snes_sprite_t;

#define SNES_MAX_SPRITES      128
#define SNES_SPRITES_PER_LINE 32

/* sprite_pal: 8 palettes x 16 = 128 OSD palette indices. */
void snes_render_sprites8(uint8_t *fb, uint32_t pitch,
                          uint32_t x0, uint32_t y0,
                          const uint8_t *sprite_chr,
                          const uint8_t *sprite_pal,
                          const snes_sprite_t *sprites,
                          uint32_t num_sprites);

#endif /* JSNES_H */
