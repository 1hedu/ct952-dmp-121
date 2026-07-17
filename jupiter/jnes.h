/*
 * JupiterSDK on CT952 -- NES PPU-style renderer (indexed-8bpp port)
 *
 * Ported from Jupiter SDK lib/nes.c + include/nes.h (Allwinner V3s).
 * Differences from the original:
 *
 *   - Output is an 8-bit palette-indexed framebuffer (the CT952 OSD
 *     region format: 1 byte per pixel, pitch = region width), not two
 *     ARGB8888 planes. The renderer writes OSD palette indices
 *     (pal_base + 6-bit NES master color), so the 64-color NES master
 *     palette must be loaded at [pal_base .. pal_base+63] in the OSD
 *     hardware palette (converted to YUV -- see jrgb2yuv.h and
 *     jup_nes_master_palette[]).
 *
 *   - The V3s DE2 hardware blender composited background (VI0) and
 *     sprites (UI0); the CT952 has a single OSD plane, so sprites are
 *     composited in software into the same buffer, after the background,
 *     using a 1bpp background-opacity bitmap for behind-BG priority.
 *
 * Tile/nametable/attribute/OAM formats are IDENTICAL to the original
 * (see the format notes below) -- all assets carry over unchanged. The
 * formats are byte-wise, so they are endian-safe on the big-endian SPARC.
 */
#ifndef JNES_H
#define JNES_H

#include "jup_types.h"

/* Native NES resolution */
#define NES_NATIVE_W  256
#define NES_NATIVE_H  224   /* safe visible area (full is 240) */
#define NES_FULL_H    240

/* ================================================================== */
/* Tile format: 2bpp, 16 bytes per 8x8 tile                           */
/*                                                                      */
/* Bytes [0..7]  = bitplane 0 (bit 0 of color index)                   */
/* Bytes [8..15] = bitplane 1 (bit 1 of color index)                   */
/*                                                                      */
/* For row r, pixel p (0=left, 7=right):                               */
/*   bit0 = (tile[r]   >> (7-p)) & 1                                   */
/*   bit1 = (tile[r+8] >> (7-p)) & 1                                   */
/*   color_index = (bit1 << 1) | bit0    (0=transparent/bg, 1-3=opaque) */
/* ================================================================== */

/* Nametable: 32x30 tile indices (960 bytes) + 64-byte attribute table */
#define NES_NT_W   32
#define NES_NT_H   30
#define NES_NT_TILES  (NES_NT_W * NES_NT_H)  /* 960 */
#define NES_NT_ATTRS  64

/* Build an attribute byte from four 2-bit palette selections
 * (quadrant order: top-left, top-right, bottom-left, bottom-right) */
#define NES_ATTR(tl, tr, bl, br) \
    (((tl)&3) | (((tr)&3)<<2) | (((bl)&3)<<4) | (((br)&3)<<6))

/* NES master palette: 64 colors as ARGB8888. Convert with
 * jup_argb_to_yuv() and load at [pal_base..pal_base+63] in the OSD
 * palette before rendering. */
extern const uint32_t jup_nes_master_palette[64];

/* Palette RAM layout (mirrors NES $3F00-$3F1F):
 *   [0]      = universal background color (6-bit NES color index)
 *   [1..3]   = BG palette 0, colors 1-3
 *   [4..7]   = BG palette 1 (index 4 mirrors universal BG)
 *   [8..11]  = BG palette 2
 *   [12..15] = BG palette 3
 *   [16]     = mirrors universal BG
 *   [17..19] = sprite palette 0, colors 1-3
 *   [20..23] = sprite palette 1
 *   [24..27] = sprite palette 2
 *   [28..31] = sprite palette 3
 *
 * Each entry is a 6-bit NES color index (0-63). */

/* OAM entry -- matches NES byte order */
typedef struct {
    uint8_t y;         /* Y position (sprite visible at y+1) */
    uint8_t tile;      /* tile index (8x8: index into sprite CHR table) */
    uint8_t attr;      /* attributes (see below) */
    uint8_t x;         /* X position */
} nes_oam_entry_t;

/* OAM attribute bits */
#define NES_SPR_VFLIP     (1 << 7)
#define NES_SPR_HFLIP     (1 << 6)
#define NES_SPR_BEHIND    (1 << 5)  /* 1 = behind background */
#define NES_SPR_PAL(p)    ((p) & 3) /* sprite palette 0-3 */

#define NES_SPR_GET_PAL(a)    ((a) & 3)
#define NES_SPR_GET_VFLIP(a)  ((a) >> 7)
#define NES_SPR_GET_HFLIP(a)  (((a) >> 6) & 1)
#define NES_SPR_GET_BEHIND(a) (((a) >> 5) & 1)

#define NES_MAX_SPRITES  64
#define NES_OAM_SIZE     (NES_MAX_SPRITES * sizeof(nes_oam_entry_t))

/* Background descriptor -- identical to the Jupiter SDK original */
typedef struct {
    const uint8_t  *chr;          /* 2bpp tile data (16 bytes per tile) */
    const uint8_t  *nametable;    /* 960 tile indices */
    const uint8_t  *attribute;    /* 64-byte attribute table */
    const uint8_t  *palette_ram;  /* 32 bytes: BG [0..15], sprites [16..31] */
    int16_t scroll_x, scroll_y;   /* pixel scroll offset */
    /* Optional per-scanline X scroll (split-scroll effects).
     * Array of NES_NATIVE_H entries. NULL to disable. Added to scroll_x. */
    const int16_t  *line_scroll_x;
    uint8_t enabled;
} nes_bg_t;

/*
 * Render one NES frame into an 8-bit palette-indexed framebuffer.
 *
 *   fb          byte framebuffer; the NES image is rendered at (x0,y0)
 *   pitch       framebuffer pitch in bytes (CT952 OSD: region width)
 *   x0, y0      top-left corner of the 256x224 output window
 *   pal_base    OSD palette index of NES master color 0 (image uses
 *               indices [pal_base .. pal_base+63])
 *   bg          background descriptor (NULL/disabled to skip)
 *   sprite_chr  sprite CHR data (2bpp; may differ from BG CHR)
 *   oam         OAM array (NULL to skip sprites)
 *   num_sprites number of active sprites (0-64)
 *
 * The caller must guarantee the 256x224 window fits inside the buffer.
 */
void nes_render8(uint8_t *fb, uint32_t pitch,
                 uint32_t x0, uint32_t y0, uint8_t pal_base,
                 const nes_bg_t *bg,
                 const uint8_t *sprite_chr,
                 const nes_oam_entry_t *oam, uint32_t num_sprites);

#endif /* JNES_H */
