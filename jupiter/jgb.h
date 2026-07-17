/*
 * JupiterSDK on CT952 -- Game Boy / Game Boy Color PPU renderer
 * (indexed-8bpp port)
 *
 * Ported from Jupiter SDK lib/gb.c + include/gb.h.
 * 160x144, 2bpp tiles, 40 sprites (10/line), GBC per-tile palettes.
 *
 * Differences from the original:
 *   - Output is an 8-bit palette-indexed framebuffer (CT952 OSD region
 *     format) instead of two ARGB8888 planes; sprites are composited in
 *     software into the same buffer (behind-priority via a 1bpp
 *     opacity bitmap).
 *   - Palettes are arrays of OSD palette INDICES (uint8_t), not
 *     ARGB8888 colors. The app converts its ARGB colors with
 *     jup_argb_to_yuv(), loads them into the OSD hardware palette, and
 *     passes the corresponding indices here.
 *
 * Tile/map/OAM formats are identical to the original (byte-wise, so
 * endian-safe on big-endian SPARC).
 */
#ifndef JGB_H
#define JGB_H

#include "jup_types.h"

#define GB_NATIVE_W  160
#define GB_NATIVE_H  144

#define GB_MAP_W     32
#define GB_MAP_H     32
#define GB_MAP_SIZE  (GB_MAP_W * GB_MAP_H)

#define GB_MAX_SPRITES      40
#define GB_SPRITES_PER_LINE 10

/* OAM entry (matches GB byte order) */
typedef struct {
    uint8_t y;       /* Y + 16 (sprite visible when scanline in [y-16, y-9]) */
    uint8_t x;       /* X + 8 */
    uint8_t tile;    /* tile index */
    uint8_t attr;    /* attributes */
} gb_oam_entry_t;

/* OAM attribute bits (GBC) */
#define GB_SPR_PRIORITY   (1 << 7)  /* 1 = behind BG colors 1-3 */
#define GB_SPR_VFLIP      (1 << 6)
#define GB_SPR_HFLIP      (1 << 5)
#define GB_SPR_PAL_DMG    (1 << 4)  /* DMG: OBP1 select */
#define GB_SPR_BANK       (1 << 3)  /* GBC: tile VRAM bank */
#define GB_SPR_PAL(n)     ((n) & 7) /* GBC: palette 0-7 */

/* BG map attribute (GBC only, one per tile in a separate attribute map) */
#define GB_BG_PRIORITY    (1 << 7)
#define GB_BG_VFLIP       (1 << 6)
#define GB_BG_HFLIP       (1 << 5)
#define GB_BG_BANK        (1 << 3)
#define GB_BG_PAL(n)      ((n) & 7)

/* Background descriptor.
 * palette: 8 palettes x 4 colors = 32 OSD palette indices. */
typedef struct {
    const uint8_t  *chr;          /* 2bpp tile data (16 bytes/tile) */
    const uint8_t  *map;          /* 32x32 tile indices */
    const uint8_t  *map_attr;     /* 32x32 GBC tile attributes (NULL for DMG) */
    const uint8_t  *palette;      /* 32 OSD palette indices (8 pal x 4) */
    int16_t scroll_x, scroll_y;
    uint8_t enabled;
} gb_bg_t;

/*
 * Render one GB/GBC frame into an 8-bit palette-indexed framebuffer.
 *
 *   fb              byte framebuffer; image rendered at (x0,y0), 160x144
 *   pitch           framebuffer pitch in bytes
 *   sprite_palette  32 OSD palette indices (8 palettes x 4 colors)
 *
 * The caller must guarantee the 160x144 window fits inside the buffer.
 */
void gb_render8(uint8_t *fb, uint32_t pitch,
                uint32_t x0, uint32_t y0,
                const gb_bg_t *bg,
                const uint8_t *sprite_chr,
                const uint8_t *sprite_palette,
                const gb_oam_entry_t *oam, uint32_t num_sprites);

#endif /* JGB_H */
