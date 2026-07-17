/*
 * JupiterSDK on CT952 -- Sega Genesis VDP-style plane renderer
 * (indexed-8bpp port)
 *
 * Ported from Jupiter SDK lib/genesis.c + include/genesis.h.
 * Two scroll planes + one window + sprites, all 4bpp tiles,
 * 4 palettes x 16 colors in CRAM.
 *
 * Differences from the original:
 *   - Output is one 8-bit palette-indexed framebuffer (CT952 OSD
 *     region format) instead of two ARGB8888 planes. Layers composite
 *     in software, back to front: Plane B (opaque with backdrop),
 *     Plane A (transparent where empty, and EXCLUDED from the window
 *     rectangle so the window truly replaces it there, matching the
 *     original's overlay semantics), Window, then sprites.
 *   - CRAM is 64 OSD palette INDICES (uint8_t), not ARGB8888 colors.
 *     The app converts its 64 ARGB colors with jup_argb_to_yuv(),
 *     loads them into the OSD hardware palette, and passes the
 *     corresponding indices here.
 *   - Renders a fixed GEN_NATIVE_W x GEN_NATIVE_H rect at (x0,y0)
 *     (same convention as jnes/jgb; the caller pre-clears the
 *     pillarbox).
 *
 * Tile/nametable/sprite formats are identical to the original
 * (byte/uint16 access on in-memory arrays -- endian-safe).
 */
#ifndef JGEN_H
#define JGEN_H

#include "jup_types.h"

/* Native Genesis resolution (H40 mode) */
#define GEN_NATIVE_W 320
#define GEN_NATIVE_H 224

/* Nametable entry (16-bit), matching Genesis VDP bit layout:
 *   [10:0]  tile index (0-2047, 0 = transparent/empty)
 *   [12:11] palette number (0-3)
 *   [13]    horizontal flip
 *   [14]    vertical flip
 *   [15]    priority (not modeled; Plane A is always in front of B) */
#define GEN_TILE(idx)       ((idx) & 0x7FF)
#define GEN_PAL(p)          (((p) & 3) << 11)
#define GEN_FLIPH           (1 << 13)
#define GEN_FLIPV           (1 << 14)
#define GEN_PRIO            (1 << 15)

#define GEN_ENTRY(idx, pal, fh, fv) \
    (GEN_TILE(idx) | GEN_PAL(pal) | ((fh) ? GEN_FLIPH : 0) | \
     ((fv) ? GEN_FLIPV : 0))

#define GEN_GET_TILE(e)   ((e) & 0x7FF)
#define GEN_GET_PAL(e)    (((e) >> 11) & 3)
#define GEN_GET_FLIPH(e)  (((e) >> 13) & 1)
#define GEN_GET_FLIPV(e)  (((e) >> 14) & 1)

/* Scroll plane descriptor.
 * cram: 64 OSD palette indices (4 palettes x 16; entry 0 of each
 * palette is transparent and never emitted). */
typedef struct {
    const uint8_t  *tiles;    /* 4bpp tile data, 32 bytes per tile */
    const uint16_t *map;      /* nametable: map_w x map_h entries */
    const uint8_t  *cram;     /* 64 OSD palette indices */
    int32_t scroll_x, scroll_y;
    /* Optional per-scanline horizontal scroll offset, added to scroll_x.
     * Length >= GEN_NATIVE_H. NULL to disable. */
    const int16_t *line_hscroll;
    uint16_t map_w, map_h;    /* must be power of 2 */
    uint8_t enabled;
} genesis_plane_t;

/* Sprite descriptor. Size in tiles (1x1 .. 4x4 = 8x8 .. 32x32 px).
 * Tile data column-major (Genesis VDP convention): tile at sprite
 * coords (tx,ty) lives at index tile + tx*h + ty. */
typedef struct {
    int16_t  x, y;            /* top-left, relative to the render rect */
    uint16_t tile;
    uint8_t  w, h;            /* size in tiles (1-4) */
    uint8_t  pal;             /* palette 0-3 */
    uint8_t  fliph, flipv;
    uint8_t  enabled;
} genesis_sprite_t;

/* Window plane -- replaces Plane A inside a screen-aligned rectangle.
 * Does not scroll. Leave map NULL to disable. */
typedef struct {
    const uint16_t *map;      /* NULL = window disabled */
    uint16_t x, y;            /* top-left in pixels (rect-relative) */
    uint16_t w, h;            /* size in pixels (multiple of 8) */
    uint16_t map_w, map_h;    /* nametable dimensions */
} genesis_window_t;

/*
 * Render one frame into an 8-bit palette-indexed framebuffer at
 * (x0,y0), size GEN_NATIVE_W x GEN_NATIVE_H (caller guarantees fit).
 *
 *   backdrop  OSD palette index written where every layer is empty
 *   plane_a   foreground plane; sprites share its tiles and CRAM
 *   plane_b   background plane
 *   window    optional window (NULL or map==NULL to disable)
 */
void genesis_render8(uint8_t *fb, uint32_t pitch,
                     uint32_t x0, uint32_t y0,
                     uint8_t backdrop,
                     const genesis_plane_t *plane_a,
                     const genesis_plane_t *plane_b,
                     const genesis_window_t *window,
                     const genesis_sprite_t *sprites, uint32_t num_sprites);

#endif /* JGEN_H */
