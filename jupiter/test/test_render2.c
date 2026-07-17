/*
 * JupiterSDK on CT952 -- SNES + Genesis renderer verification.
 *
 * Same scheme as test_render.c: deterministic scenes, CRC32 per frame
 * (must match between native little-endian and qemu-sparc64 big-endian
 * runs), PPM dumps for eyeballing.
 *
 * Covers: Genesis planes A/B + line-hscroll + window + multi-tile
 * flipped sprites; SNES modes 1 (3 BGs, 4bpp+2bpp), 2 (per-tile-column
 * offset), 3 (8bpp), sprites (multi-tile, flips, clipping), Mode 7.
 */
#include <stdio.h>
#include <string.h>
#include "jgen.h"
#include "jsnes.h"

#define FB_W   616
#define FB_H   440
#define PITCH  FB_W

static uint8_t fb[PITCH * FB_H];

static uint32_t crc32_buf(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static void dump_ppm(const char *path)
{
    /* Simple index -> grayscale-ish false color for eyeballing */
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            uint8_t i = fb[y * PITCH + x];
            fputc((i * 7) & 0xFF, f);
            fputc((i * 13) & 0xFF, f);
            fputc((i * 29) & 0xFF, f);
        }
    fclose(f);
}

/* ---- Genesis scene ---- */

/* 4bpp tile builder: 32 bytes, 2 px/byte, high nibble first */
static void gen_tile_set(uint8_t *tile, int row, int col, uint8_t ci)
{
    uint8_t *b = &tile[row * 4 + (col >> 1)];
    if (col & 1) *b = (uint8_t)((*b & 0xF0) | (ci & 0x0F));
    else         *b = (uint8_t)((*b & 0x0F) | (uint8_t)(ci << 4));
}

static uint8_t gen_tiles[32 * 8];
static uint16_t gen_map_a[32 * 32];
static uint16_t gen_map_b[32 * 32];
static uint16_t gen_win_map[16 * 4];
static uint8_t gen_cram[64];
static int16_t gen_hscroll[GEN_NATIVE_H];

static void build_gen_scene(genesis_plane_t *pa, genesis_plane_t *pb,
                            genesis_window_t *win, genesis_sprite_t *spr)
{
    int r, c, t, i;

    memset(gen_tiles, 0, sizeof(gen_tiles));
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            /* tile 1: solid; 2: checker; 3: diagonal; 4: ring */
            gen_tile_set(&gen_tiles[32 * 1], r, c, 1);
            gen_tile_set(&gen_tiles[32 * 2], r, c,
                         (uint8_t)((((r >> 1) ^ (c >> 1)) & 1) ? 2 : 0));
            gen_tile_set(&gen_tiles[32 * 3], r, c,
                         (uint8_t)((((r + c) & 7) == 0) ? 3 : 1));
            gen_tile_set(&gen_tiles[32 * 4], r, c,
                         (uint8_t)((r == 0 || r == 7 || c == 0 || c == 7)
                                   ? 4 : 0));
        }

    for (r = 0; r < 32; r++)
        for (c = 0; c < 32; c++) {
            gen_map_a[r * 32 + c] = (uint16_t)(((r + c) % 3 == 0)
                ? GEN_ENTRY(4, (r + c) & 3, r & 1, c & 1) : 0);
            gen_map_b[r * 32 + c] =
                (uint16_t)GEN_ENTRY(((r ^ c) & 1) ? 2 : 3,
                                    (r >> 2) & 3, 0, (r + c) & 1);
        }

    for (i = 0; i < 16 * 4; i++)
        gen_win_map[i] = (uint16_t)GEN_ENTRY(1, i & 3, 0, 0);

    for (i = 0; i < 64; i++) gen_cram[i] = (uint8_t)(64 + i);

    for (i = 0; i < GEN_NATIVE_H; i++)
        gen_hscroll[i] = (int16_t)((i & 16) ? 2 : -2);   /* wave */

    pa->tiles = gen_tiles; pa->map = gen_map_a; pa->cram = gen_cram;
    pa->scroll_x = 21; pa->scroll_y = 9; pa->line_hscroll = NULL;
    pa->map_w = 32; pa->map_h = 32; pa->enabled = 1;

    pb->tiles = gen_tiles; pb->map = gen_map_b; pb->cram = gen_cram;
    pb->scroll_x = 7; pb->scroll_y = 3; pb->line_hscroll = gen_hscroll;
    pb->map_w = 32; pb->map_h = 32; pb->enabled = 1;

    win->map = gen_win_map; win->x = 8; win->y = 8;
    win->w = 128; win->h = 32; win->map_w = 16; win->map_h = 4;

    /* 2x2-tile sprite (uses tiles 1..4 column-major), one flipped,
     * one clipped at the left edge */
    for (t = 0; t < 3; t++) {
        spr[t].tile = 1; spr[t].w = 2; spr[t].h = 2;
        spr[t].pal = (uint8_t)t; spr[t].enabled = 1;
        spr[t].fliph = 0; spr[t].flipv = 0;
    }
    spr[0].x = 100; spr[0].y = 120;
    spr[1].x = 140; spr[1].y = 120; spr[1].fliph = 1; spr[1].flipv = 1;
    spr[2].x = -7;  spr[2].y = 200;   /* clips left */
}

/* ---- SNES scenes ---- */

/* SNES 4bpp planar tile builder */
static void s4_tile_set(uint8_t *tile, int row, int col, uint8_t ci)
{
    uint8_t mask = (uint8_t)(0x80 >> col);
    if (ci & 1) tile[row * 2] |= mask;
    if (ci & 2) tile[row * 2 + 1] |= mask;
    if (ci & 4) tile[row * 2 + 16] |= mask;
    if (ci & 8) tile[row * 2 + 17] |= mask;
}

/* SNES 2bpp tile builder (NES-style bitplanes) */
static void s2_tile_set(uint8_t *tile, int row, int col, uint8_t ci)
{
    uint8_t mask = (uint8_t)(0x80 >> col);
    if (ci & 1) tile[row * 2] |= mask;
    if (ci & 2) tile[row * 2 + 1] |= mask;
}

static uint8_t sn_tiles4[32 * 4];
static uint8_t sn_tiles2[16 * 3];
static uint8_t sn_tiles8[64 * 3];
static uint8_t sn_spr_chr[32 * 5];
static uint16_t sn_map1[32 * 32], sn_map2[32 * 32], sn_map3[64 * 32];
static uint8_t sn_pal[256];
static uint8_t sn_spr_pal[128];
static int16_t sn_colofs[32];
static uint8_t m7_map[64 * 64];
static uint8_t m7_pal[256];

static void build_snes_scenes(snes_bg_t *b1, snes_bg_t *b2, snes_bg_t *b3,
                              snes_bg_t *b8, snes_tile_offset_t *ofs,
                              snes_mode7_t *m7, snes_sprite_t *spr)
{
    int r, c, i, t;

    memset(sn_tiles4, 0, sizeof(sn_tiles4));
    memset(sn_tiles2, 0, sizeof(sn_tiles2));
    memset(sn_spr_chr, 0, sizeof(sn_spr_chr));
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            s4_tile_set(&sn_tiles4[32 * 1], r, c,
                        (uint8_t)(((r ^ c) & 1) ? 5 : 11));
            s4_tile_set(&sn_tiles4[32 * 2], r, c,
                        (uint8_t)((r < 4) ? ((c & 3) + 1) : 0));
            s4_tile_set(&sn_tiles4[32 * 3], r, c,
                        (uint8_t)((r == c) ? 15 : ((r > c) ? 3 : 0)));
            s2_tile_set(&sn_tiles2[16 * 1], r, c,
                        (uint8_t)((((r >> 2) ^ (c >> 2)) & 1) ? 1 : 2));
            s2_tile_set(&sn_tiles2[16 * 2], r, c,
                        (uint8_t)(((r + c) & 3) ? 0 : 3));
            /* sprite tiles 1..4 (2x2 col-major) + tile 0 solid */
            for (t = 0; t < 5; t++)
                s4_tile_set(&sn_spr_chr[32 * t], r, c,
                            (uint8_t)(((r + c + t) % 5 == 0) ? 0
                                      : ((c + t) & 15)));
        }

    /* 8bpp tiles: direct byte ramps */
    for (i = 0; i < 64; i++) {
        sn_tiles8[64 * 1 + i] = (uint8_t)(i & 0x3F);
        sn_tiles8[64 * 2 + i] = (uint8_t)(63 - (i & 0x3F));
    }

    for (r = 0; r < 32; r++)
        for (c = 0; c < 32; c++) {
            sn_map1[r * 32 + c] = (uint16_t)(((r + c) & 3)
                ? SNES_ENTRY(2, (r + c) & 7, 0, r & 1, c & 1) : 0);
            sn_map2[r * 32 + c] =
                (uint16_t)SNES_ENTRY(((r ^ c) & 1) + 1, (r >> 1) & 7,
                                     0, 0, 0);
        }
    for (r = 0; r < 32; r++)
        for (c = 0; c < 64; c++)
            sn_map3[r * 64 + c] =
                (uint16_t)SNES_ENTRY((c & 1) + 1, 0, 0, c & 1, r & 1);

    for (i = 0; i < 256; i++) sn_pal[i] = (uint8_t)i;
    for (i = 0; i < 128; i++) sn_spr_pal[i] = (uint8_t)(128 + i);
    for (i = 0; i < 32; i++)
        sn_colofs[i] = (int16_t)((i & 1) ? (i * 2) : -(i * 2));

    b1->tiles = sn_tiles4; b1->map = sn_map1; b1->palette = sn_pal;
    b1->scroll_x = 11; b1->scroll_y = 3; b1->map_w = 32; b1->map_h = 32;
    b1->bpp = 4; b1->enabled = 1;

    b2->tiles = sn_tiles4; b2->map = sn_map2; b2->palette = sn_pal;
    b2->scroll_x = 5; b2->scroll_y = 17; b2->map_w = 32; b2->map_h = 32;
    b2->bpp = 4; b2->enabled = 1;

    b3->tiles = sn_tiles2; b3->map = sn_map2; b3->palette = sn_pal;
    b3->scroll_x = 2; b3->scroll_y = 1; b3->map_w = 32; b3->map_h = 32;
    b3->bpp = 2; b3->enabled = 1;

    b8->tiles = sn_tiles8; b8->map = sn_map3; b8->palette = sn_pal;
    b8->scroll_x = 30; b8->scroll_y = 12; b8->map_w = 64; b8->map_h = 32;
    b8->bpp = 8; b8->enabled = 1;

    ofs->col_offset = sn_colofs;
    ofs->vertical = 1;

    /* Mode 7: rings texture */
    for (r = 0; r < 64; r++)
        for (c = 0; c < 64; c++) {
            int dx = c - 32, dy = r - 32;
            m7_map[r * 64 + c] = (uint8_t)(((dx * dx + dy * dy) >> 5) & 15);
        }
    for (i = 0; i < 256; i++) m7_pal[i] = (uint8_t)(64 + (i & 15) * 4);

    m7->cam_x = 123; m7->cam_y = -45;
    m7->angle = 37; m7->twist = 1;
    m7->horizon = 60; m7->space_z = 8000;
    m7->map = m7_map; m7->palette = m7_pal;
    m7->map_w_bits = 6; m7->map_mask = 63;

    /* Sprites: 2x2 col-major, flips, clipped bottom-right */
    for (t = 0; t < 3; t++) {
        spr[t].tile = 1; spr[t].w = 2; spr[t].h = 2;
        spr[t].pal = (uint8_t)(t * 2); spr[t].priority = 0;
        spr[t].fliph = 0; spr[t].flipv = 0; spr[t].enabled = 1;
    }
    spr[0].x = 60;  spr[0].y = 80;
    spr[1].x = 90;  spr[1].y = 80; spr[1].fliph = 1;
    spr[2].x = 245; spr[2].y = 215;   /* clips right+bottom */
}

int main(void)
{
    genesis_plane_t pa, pb;
    genesis_window_t win;
    genesis_sprite_t gspr[3];
    snes_bg_t b1, b2, b3, b8;
    snes_tile_offset_t ofs;
    snes_mode7_t m7;
    snes_sprite_t sspr[3];
    uint32_t gx0 = (FB_W - GEN_NATIVE_W) / 2;
    uint32_t gy0 = (FB_H - GEN_NATIVE_H) / 2;
    uint32_t sx0 = (FB_W - SNES_NATIVE_W) / 2;
    uint32_t sy0 = (FB_H - SNES_NATIVE_H) / 2;

    build_gen_scene(&pa, &pb, &win, gspr);
    build_snes_scenes(&b1, &b2, &b3, &b8, &ofs, &m7, sspr);

    /* ---- Genesis: planes + window + sprites ---- */
    memset(fb, 0, sizeof(fb));
    genesis_render8(fb, PITCH, gx0, gy0, 0x0F, &pa, &pb, &win, gspr, 3);
    printf("CRC gen_frame=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));
    dump_ppm("out_gen.ppm");

    /* Window must differ from no-window */
    {
        uint32_t crc1 = crc32_buf(fb, sizeof(fb));
        memset(fb, 0, sizeof(fb));
        genesis_render8(fb, PITCH, gx0, gy0, 0x0F, &pa, &pb, NULL, gspr, 3);
        if (crc32_buf(fb, sizeof(fb)) == crc1) {
            printf("FAIL: window had no effect\n");
            return 1;
        }
    }

    /* ---- SNES mode 1 + sprites ---- */
    memset(fb, 0, sizeof(fb));
    snes_mode1_render8(fb, PITCH, sx0, sy0, 0x0F, &b1, &b2, &b3);
    snes_render_sprites8(fb, PITCH, sx0, sy0, sn_spr_chr, sn_spr_pal,
                         sspr, 3);
    printf("CRC snes_m1=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));
    dump_ppm("out_snes_m1.ppm");

    /* ---- SNES mode 2 (per-tile-column offset, vertical) ---- */
    memset(fb, 0, sizeof(fb));
    snes_mode2_render8(fb, PITCH, sx0, sy0, 0x0F, &b1, &b2, &ofs);
    printf("CRC snes_m2=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));

    /* Offsets must matter */
    {
        uint32_t crc1 = crc32_buf(fb, sizeof(fb));
        snes_tile_offset_t noofs;
        noofs.col_offset = NULL;
        noofs.vertical = 0;
        memset(fb, 0, sizeof(fb));
        snes_mode2_render8(fb, PITCH, sx0, sy0, 0x0F, &b1, &b2, &noofs);
        if (crc32_buf(fb, sizeof(fb)) == crc1) {
            printf("FAIL: tile-column offsets had no effect\n");
            return 1;
        }
    }

    /* ---- SNES mode 3 (8bpp BG1) ---- */
    memset(fb, 0, sizeof(fb));
    snes_mode3_render8(fb, PITCH, sx0, sy0, 0x0F, &b8, &b2);
    printf("CRC snes_m3=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));

    /* ---- SNES mode 0 (4 BGs 2bpp) ---- */
    {
        snes_bg_t c1 = b3, c2 = b3, c3 = b3, c4 = b3;
        c1.scroll_x = 1; c2.scroll_x = 9; c3.scroll_y = 21;
        memset(fb, 0, sizeof(fb));
        snes_mode0_render8(fb, PITCH, sx0, sy0, 0x0F, &c1, &c2, &c3, &c4);
        printf("CRC snes_m0=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));
    }

    /* ---- SNES mode 7 ---- */
    memset(fb, 0, sizeof(fb));
    snes_mode7_render8(fb, PITCH, sx0, sy0, &m7);
    printf("CRC snes_m7=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));
    dump_ppm("out_snes_m7.ppm");

    /* Rotating the camera must change the frame */
    {
        uint32_t crc1 = crc32_buf(fb, sizeof(fb));
        m7.angle = (uint8_t)(m7.angle + 3);
        memset(fb, 0, sizeof(fb));
        snes_mode7_render8(fb, PITCH, sx0, sy0, &m7);
        if (crc32_buf(fb, sizeof(fb)) == crc1) {
            printf("FAIL: mode7 rotation had no effect\n");
            return 1;
        }
    }

    printf("render2 tests OK\n");
    return 0;
}
