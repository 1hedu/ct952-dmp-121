/*
 * JupiterSDK on CT952 -- Game Boy / Game Boy Color PPU renderer,
 * indexed-8bpp port of Jupiter SDK lib/gb.c.
 * 160x144, 2bpp tiles, 40 sprites (10/line), GBC per-tile palettes.
 */
#include "jgb.h"

/* 1bpp background-opacity bitmap for behind-priority sprites */
static uint8_t bg_opaque_bits[(GB_NATIVE_W / 8) * GB_NATIVE_H];

#define OPAQUE_SET(x, y)  (bg_opaque_bits[(y) * (GB_NATIVE_W / 8) + ((x) >> 3)] |= (uint8_t)(0x80 >> ((x) & 7)))
#define OPAQUE_GET(x, y)  (bg_opaque_bits[(y) * (GB_NATIVE_W / 8) + ((x) >> 3)] &  (uint8_t)(0x80 >> ((x) & 7)))

static uint8_t chr_pixel(const uint8_t *tile, int row, int col)
{
    int shift = 7 - col;
    return (uint8_t)(((tile[row] >> shift) & 1) |
                     (((tile[row + 8] >> shift) & 1) << 1));
}

/* Render background; also rebuilds the opacity bitmap */
static void render_bg(uint8_t *fb, uint32_t pitch,
                      uint32_t x0, uint32_t y0,
                      const gb_bg_t *bg)
{
    uint32_t sy, sx, i;
    uint8_t backdrop;

    for (i = 0; i < sizeof(bg_opaque_bits); i++)
        bg_opaque_bits[i] = 0;

    if (!bg || !bg->enabled || !bg->chr || !bg->map)
        return;

    backdrop = bg->palette[0];

    for (sy = 0; sy < GB_NATIVE_H; sy++) {
        uint32_t wy = ((uint32_t)((int32_t)sy + bg->scroll_y)) & 0xFF;
        uint32_t tr = wy / 8;
        uint32_t fy = wy & 7;
        uint8_t *row = fb + (y0 + sy) * pitch + x0;

        for (sx = 0; sx < GB_NATIVE_W; sx++) {
            uint32_t wx = ((uint32_t)((int32_t)sx + bg->scroll_x)) & 0xFF;
            uint32_t tc = wx / 8;
            uint32_t fx = wx & 7;

            uint32_t mi = (tr & 31) * GB_MAP_W + (tc & 31);
            uint8_t tidx = bg->map[mi];

            /* GBC attributes */
            int pal = 0, hflip = 0, vflip = 0;
            int actual_fy, actual_fx;
            uint8_t ci;

            if (bg->map_attr) {
                uint8_t attr = bg->map_attr[mi];
                pal = attr & 7;
                hflip = (attr >> 5) & 1;
                vflip = (attr >> 6) & 1;
            }

            actual_fy = vflip ? (7 - (int)fy) : (int)fy;
            actual_fx = hflip ? (7 - (int)fx) : (int)fx;
            ci = chr_pixel(bg->chr + tidx * 16, actual_fy, actual_fx);

            if (ci == 0) {
                row[sx] = backdrop;
            } else {
                row[sx] = bg->palette[pal * 4 + ci];
                OPAQUE_SET(sx, sy);
            }
        }
    }
}

static void render_sprites(uint8_t *fb, uint32_t pitch,
                           uint32_t x0, uint32_t y0,
                           const uint8_t *chr, const uint8_t *pal,
                           const gb_oam_entry_t *oam, uint32_t num)
{
    uint8_t line_count[GB_NATIVE_H];
    uint32_t i;
    int s;

    if (!oam || !chr || !pal || num == 0)
        return;

    for (i = 0; i < GB_NATIVE_H; i++)
        line_count[i] = 0;

    /* Reverse OAM order (lower index = higher priority) */
    for (s = (int)num - 1; s >= 0; s--) {
        const gb_oam_entry_t *sp = &oam[s];
        int sy = (int)sp->y - 16;  /* GB OAM Y is offset by 16 */
        int sx = (int)sp->x - 8;   /* GB OAM X is offset by 8 */
        int hflip = (sp->attr >> 5) & 1;
        int vflip = (sp->attr >> 6) & 1;
        int behind = (sp->attr >> 7) & 1;
        int spal = sp->attr & 7;
        int py, px;

        const uint8_t *tile = chr + sp->tile * 16;

        for (py = 0; py < 8; py++) {
            int screen_y = sy + py;
            int tr, drew;

            if (screen_y < 0 || screen_y >= GB_NATIVE_H) continue;
            if (line_count[screen_y] >= GB_SPRITES_PER_LINE) continue;

            tr = vflip ? (7 - py) : py;
            drew = 0;

            for (px = 0; px < 8; px++) {
                int screen_x = sx + px;
                int tc;
                uint8_t ci;

                if (screen_x < 0 || screen_x >= GB_NATIVE_W) continue;

                tc = hflip ? (7 - px) : px;
                ci = chr_pixel(tile, tr, tc);
                if (ci == 0) continue;

                if (behind && OPAQUE_GET(screen_x, screen_y)) continue;

                fb[(y0 + (uint32_t)screen_y) * pitch + x0 + (uint32_t)screen_x] =
                    pal[spal * 4 + ci];
                drew = 1;
            }
            if (drew) line_count[screen_y]++;
        }
    }
}

void gb_render8(uint8_t *fb, uint32_t pitch,
                uint32_t x0, uint32_t y0,
                const gb_bg_t *bg,
                const uint8_t *sprite_chr,
                const uint8_t *sprite_palette,
                const gb_oam_entry_t *oam, uint32_t num_sprites)
{
    render_bg(fb, pitch, x0, y0, bg);
    render_sprites(fb, pitch, x0, y0,
                   sprite_chr, sprite_palette, oam, num_sprites);
}
