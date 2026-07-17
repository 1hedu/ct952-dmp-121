/*
 * JupiterSDK on CT952 -- NES PPU-style renderer, indexed-8bpp port.
 *
 * Ported from Jupiter SDK lib/nes.c. Rendering model is unchanged:
 *   Background: 2bpp tiles, 32x30 nametable, attribute table palette select
 *   Sprites: 64 OAM entries, 8x8, priority/flip, 4 sprite palettes,
 *            8-per-scanline limit
 *   Compositing: sprite behind-BG shows through BG-transparent pixels
 *
 * Output is palette INDICES (pal_base + 6-bit NES color) into a single
 * 8bpp buffer instead of ARGB8888 into two hardware-blended planes.
 * The behind-BG sprite priority uses a 1bpp opacity bitmap (7.5 KB
 * static) instead of the original's byte map, to respect the CT952's
 * tight ~294 KB total RAM window.
 */
#include "jnes.h"

/* NES master palette -- 64 colors as ARGB8888 (same table as the SDK;
 * standard NTSC approximation). Loaded into the OSD palette by the app
 * after jup_argb_to_yuv() conversion. */
const uint32_t jup_nes_master_palette[64] = {
    0xFF545454, 0xFF001E74, 0xFF081090, 0xFF300088,
    0xFF440064, 0xFF5C0030, 0xFF540400, 0xFF3C1800,
    0xFF202A00, 0xFF083A00, 0xFF004000, 0xFF003C00,
    0xFF00323C, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFF989698, 0xFF084CC4, 0xFF3032EC, 0xFF5C1EE4,
    0xFF8800B0, 0xFFA00060, 0xFF981A20, 0xFF783C00,
    0xFF545A00, 0xFF207200, 0xFF007C00, 0xFF007628,
    0xFF006678, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFECEEEC, 0xFF4C9AEC, 0xFF787CEC, 0xFFB062EC,
    0xFFE400DC, 0xFFEC0078, 0xFFEC3C30, 0xFFCC6C00,
    0xFF8C9000, 0xFF48A800, 0xFF10B800, 0xFF00B260,
    0xFF00A2C4, 0xFF4C4C4C, 0xFF000000, 0xFF000000,
    0xFFECEEEC, 0xFFA8CCEC, 0xFFBCBCEC, 0xFFD4B2EC,
    0xFFECAEEC, 0xFFECAED4, 0xFFECB4B0, 0xFFE4C490,
    0xFFCCD278, 0xFFB4DE78, 0xFFA8E290, 0xFF98E2B4,
    0xFFA0D6E4, 0xFFA0A2A0, 0xFF000000, 0xFF000000,
};

/* 1bpp background-opacity bitmap for behind-BG sprite priority */
static uint8_t bg_opaque_bits[(NES_NATIVE_W / 8) * NES_FULL_H];

#define OPAQUE_SET(x, y)  (bg_opaque_bits[(y) * (NES_NATIVE_W / 8) + ((x) >> 3)] |= (uint8_t)(0x80 >> ((x) & 7)))
#define OPAQUE_GET(x, y)  (bg_opaque_bits[(y) * (NES_NATIVE_W / 8) + ((x) >> 3)] &  (uint8_t)(0x80 >> ((x) & 7)))

/* Decode one 2bpp pixel from CHR tile data (col: 0=left, 7=right) */
static uint8_t chr_pixel(const uint8_t *tile, int row, int col)
{
    int shift = 7 - col;
    uint8_t bp0 = (uint8_t)((tile[row] >> shift) & 1);
    uint8_t bp1 = (uint8_t)((tile[row + 8] >> shift) & 1);
    return (uint8_t)((bp1 << 1) | bp0);
}

/* Resolve to OSD palette index: pal_base + 6-bit NES color */
static uint8_t resolve_bg_index(const uint8_t *pal_ram, uint8_t pal_base,
                                int pal_num, int color_idx)
{
    if (color_idx == 0)
        return (uint8_t)(pal_base + (pal_ram[0] & 0x3F));  /* universal BG */
    return (uint8_t)(pal_base + (pal_ram[pal_num * 4 + color_idx] & 0x3F));
}

static uint8_t resolve_spr_index(const uint8_t *pal_ram, uint8_t pal_base,
                                 int pal_num, int color_idx)
{
    /* Sprite palette starts at offset 16 in palette RAM */
    return (uint8_t)(pal_base + (pal_ram[16 + pal_num * 4 + color_idx] & 0x3F));
}

/* Attribute palette for a tile position */
static int get_attr_palette(const uint8_t *attr, int tx, int ty)
{
    int attr_idx = (ty / 4) * 8 + (tx / 4);
    int shift = ((ty & 2) << 1) | (tx & 2);
    return (attr[attr_idx] >> shift) & 3;
}

/* Render background; also rebuilds the opacity bitmap */
static void render_bg(uint8_t *fb, uint32_t pitch,
                      uint32_t x0, uint32_t y0, uint8_t pal_base,
                      const nes_bg_t *bg)
{
    uint32_t sy, sx, i;
    uint8_t backdrop;

    for (i = 0; i < sizeof(bg_opaque_bits); i++)
        bg_opaque_bits[i] = 0;

    if (!bg || !bg->enabled || !bg->chr || !bg->nametable)
        return;

    backdrop = (uint8_t)(pal_base + (bg->palette_ram[0] & 0x3F));

    for (sy = 0; sy < NES_NATIVE_H; sy++) {
        int16_t scr_x = bg->scroll_x;
        uint32_t world_y, tile_row, fine_y;
        uint8_t *row;

        if (bg->line_scroll_x)
            scr_x = (int16_t)(scr_x + bg->line_scroll_x[sy]);

        world_y = (uint32_t)((int32_t)sy + bg->scroll_y) % (NES_NT_H * 8);
        tile_row = world_y / 8;
        fine_y   = world_y & 7;

        row = fb + (y0 + sy) * pitch + x0;

        for (sx = 0; sx < NES_NATIVE_W; sx++) {
            uint32_t world_x = (uint32_t)((int32_t)sx + scr_x) % (NES_NT_W * 8);
            uint32_t tile_col = world_x / 8;
            uint32_t fine_x   = world_x & 7;

            uint8_t tile_idx = bg->nametable[tile_row * NES_NT_W + tile_col];
            const uint8_t *tile = bg->chr + tile_idx * 16;

            uint8_t ci = chr_pixel(tile, (int)fine_y, (int)fine_x);

            if (ci == 0) {
                row[sx] = backdrop;
            } else {
                int pal_num = get_attr_palette(bg->attribute,
                                               (int)tile_col, (int)tile_row);
                row[sx] = resolve_bg_index(bg->palette_ram, pal_base,
                                           pal_num, ci);
                OPAQUE_SET(sx, sy);
            }
        }
    }
}

/* Composite sprites into the same 8bpp buffer */
static void render_sprites(uint8_t *fb, uint32_t pitch,
                           uint32_t x0, uint32_t y0, uint8_t pal_base,
                           const nes_bg_t *bg,
                           const uint8_t *sprite_chr,
                           const nes_oam_entry_t *oam, uint32_t num_sprites)
{
    uint8_t scanline_count[NES_FULL_H];
    uint32_t i;
    int s;

    if (!oam || !sprite_chr || num_sprites == 0 || !bg)
        return;

    for (i = 0; i < NES_FULL_H; i++)
        scanline_count[i] = 0;

    /* Reverse OAM order so lower indices have priority */
    for (s = (int)num_sprites - 1; s >= 0; s--) {
        const nes_oam_entry_t *spr = &oam[s];
        int spr_y = (int)spr->y + 1;  /* NES Y is one less than actual */
        int spr_x = (int)spr->x;
        int hflip = NES_SPR_GET_HFLIP(spr->attr);
        int vflip = NES_SPR_GET_VFLIP(spr->attr);
        int behind = NES_SPR_GET_BEHIND(spr->attr);
        int spr_pal = NES_SPR_GET_PAL(spr->attr);
        int py, px;

        const uint8_t *tile = sprite_chr + spr->tile * 16;

        for (py = 0; py < 8; py++) {
            int screen_y = spr_y + py;
            int tile_row, drew_pixel;

            if (screen_y < 0 || screen_y >= NES_NATIVE_H) continue;
            if (scanline_count[screen_y] >= 8) continue;  /* 8/line limit */

            tile_row = vflip ? (7 - py) : py;
            drew_pixel = 0;

            for (px = 0; px < 8; px++) {
                int screen_x = spr_x + px;
                int tile_col;
                uint8_t ci;

                if (screen_x < 0 || screen_x >= NES_NATIVE_W) continue;

                tile_col = hflip ? (7 - px) : px;
                ci = chr_pixel(tile, tile_row, tile_col);
                if (ci == 0) continue;  /* transparent */

                /* Behind-BG priority: only show through transparent BG */
                if (behind && OPAQUE_GET(screen_x, screen_y))
                    continue;

                fb[(y0 + (uint32_t)screen_y) * pitch + x0 + (uint32_t)screen_x] =
                    resolve_spr_index(bg->palette_ram, pal_base, spr_pal, ci);
                drew_pixel = 1;
            }

            if (drew_pixel)
                scanline_count[screen_y]++;
        }
    }
}

void nes_render8(uint8_t *fb, uint32_t pitch,
                 uint32_t x0, uint32_t y0, uint8_t pal_base,
                 const nes_bg_t *bg,
                 const uint8_t *sprite_chr,
                 const nes_oam_entry_t *oam, uint32_t num_sprites)
{
    render_bg(fb, pitch, x0, y0, pal_base, bg);
    render_sprites(fb, pitch, x0, y0, pal_base,
                   bg, sprite_chr, oam, num_sprites);
}
