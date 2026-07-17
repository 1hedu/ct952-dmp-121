/*
 * JupiterSDK on CT952 -- renderer verification.
 *
 * Renders fixed NES and GB test scenes with jnes/jgb into an 8bpp
 * buffer sized like the CT952 OSD region, prints a CRC32 per frame,
 * and dumps PPM images (index -> ARGB via the real master palettes) for
 * eyeballing. Also spot-checks the ARGB->YUV palette converter.
 *
 * Built twice: natively (little-endian x86) and as a static sparc64
 * binary run under qemu (big-endian). Identical CRCs across the two
 * prove the ported code is endian-clean.
 */
#include <stdio.h>
#include <string.h>
#include "jnes.h"
#include "jgb.h"
#include "jdraw.h"
#include "jrgb2yuv.h"

#define FB_W   616
#define FB_H   440
#define PITCH  FB_W

static uint8_t fb[PITCH * FB_H];

/* ---- CRC32 (IEEE 802.3) ---- */
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

/* ---- PPM dump (indices resolved through an ARGB palette) ---- */
static void dump_ppm(const char *path, const uint32_t *pal256)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            uint32_t c = pal256[fb[y * PITCH + x]];
            fputc((int)((c >> 16) & 0xFF), f);
            fputc((int)((c >> 8) & 0xFF), f);
            fputc((int)(c & 0xFF), f);
        }
    fclose(f);
}

/* ---- NES test scene (deterministic, exercises attrs + sprites) ---- */
#define NES_BASE 64

static uint8_t nes_chr[16 * 4];
static uint8_t nes_spr_chr[16 * 2];
static uint8_t nes_nt[NES_NT_TILES];
static uint8_t nes_attr_tab[NES_NT_ATTRS];
static uint8_t nes_pal_ram[32];
static int16_t nes_line_scroll[NES_NATIVE_H];

static void chr_set(uint8_t *tile, int row, int col, uint8_t ci)
{
    uint8_t mask = (uint8_t)(0x80 >> col);
    if (ci & 1) tile[row] |= mask;
    if (ci & 2) tile[row + 8] |= mask;
}

static void build_nes_scene(nes_bg_t *bg, nes_oam_entry_t *oam)
{
    int r, c, i;

    memset(nes_chr, 0, sizeof(nes_chr));
    memset(nes_spr_chr, 0, sizeof(nes_spr_chr));
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            chr_set(&nes_chr[16 * 1], r, c,
                    (uint8_t)((r == 0 || r == 4 || c == (r < 4 ? 0 : 4)) ? 3 : 1));
            chr_set(&nes_chr[16 * 2], r, c,
                    (uint8_t)((((r >> 2) ^ (c >> 2)) & 1) ? 2 : 1));
            chr_set(&nes_chr[16 * 3], r, c,
                    (uint8_t)(((r & 3) == 1 && (c & 3) == 1) ? 3 : 0));
            /* sprite 0: diagonal stripes; sprite 1: solid box w/ hole */
            chr_set(&nes_spr_chr[0], r, c, (uint8_t)((((r + c) & 3) == 0) ? 3 : 1));
            chr_set(&nes_spr_chr[16], r, c,
                    (uint8_t)((r >= 2 && r <= 5 && c >= 2 && c <= 5) ? 0 : 2));
        }

    for (r = 0; r < NES_NT_H; r++)
        for (c = 0; c < NES_NT_W; c++)
            nes_nt[r * NES_NT_W + c] =
                (uint8_t)((r + c) % 4);

    for (i = 0; i < NES_NT_ATTRS; i++)
        nes_attr_tab[i] = NES_ATTR(i & 3, (i + 1) & 3, (i + 2) & 3, (i + 3) & 3);

    for (i = 0; i < 32; i++) nes_pal_ram[i] = (uint8_t)(i * 2);
    nes_pal_ram[0] = 0x0F;

    for (i = 0; i < NES_NATIVE_H; i++)
        nes_line_scroll[i] = (int16_t)((i & 8) ? 3 : -3);  /* split scroll */

    bg->chr = nes_chr;
    bg->nametable = nes_nt;
    bg->attribute = nes_attr_tab;
    bg->palette_ram = nes_pal_ram;
    bg->scroll_x = 13;
    bg->scroll_y = 7;
    bg->line_scroll_x = nes_line_scroll;
    bg->enabled = 1;

    /* Sprites: normal, hflip, vflip, behind-BG, partially off-screen */
    oam[0].y = 50;  oam[0].tile = 0; oam[0].attr = NES_SPR_PAL(0); oam[0].x = 60;
    oam[1].y = 50;  oam[1].tile = 0; oam[1].attr = (uint8_t)(NES_SPR_HFLIP | NES_SPR_PAL(1)); oam[1].x = 80;
    oam[2].y = 100; oam[2].tile = 1; oam[2].attr = (uint8_t)(NES_SPR_VFLIP | NES_SPR_PAL(2)); oam[2].x = 100;
    oam[3].y = 100; oam[3].tile = 1; oam[3].attr = (uint8_t)(NES_SPR_BEHIND | NES_SPR_PAL(3)); oam[3].x = 120;
    oam[4].y = 210; oam[4].tile = 0; oam[4].attr = 0; oam[4].x = 252;  /* clips right */
}

/* ---- GB test scene (GBC attribute map path) ---- */
#define GB_BASE 192

static uint8_t gbt_chr[16 * 3];
static uint8_t gbt_spr_chr[16];
static uint8_t gbt_map[GB_MAP_SIZE];
static uint8_t gbt_map_attr[GB_MAP_SIZE];
static uint8_t gbt_pal[32];
static uint8_t gbt_spr_pal[32];

static void build_gb_scene(gb_bg_t *bg, gb_oam_entry_t *oam)
{
    int r, c, i;

    memset(gbt_chr, 0, sizeof(gbt_chr));
    memset(gbt_spr_chr, 0, sizeof(gbt_spr_chr));
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            int dx = (c < 4) ? (3 - c) : (c - 4);
            int dy = (r < 4) ? (3 - r) : (r - 4);
            chr_set(&gbt_chr[16 * 1], r, c, (uint8_t)((dx + dy <= 3) ? 2 : 1));
            chr_set(&gbt_chr[16 * 2], r, c,
                    (uint8_t)((r == 0 || r == 7 || c == 0 || c == 7) ? 3 : 0));
            chr_set(gbt_spr_chr, r, c, (uint8_t)(((r ^ c) & 1) ? 3 : 1));
        }

    for (r = 0; r < GB_MAP_H; r++)
        for (c = 0; c < GB_MAP_W; c++) {
            gbt_map[r * GB_MAP_W + c] = (uint8_t)((r + c) % 3);
            /* alternate palettes + flips via GBC attribute map */
            gbt_map_attr[r * GB_MAP_W + c] =
                (uint8_t)(((r + c) & 7) | (((r & 1) != 0) ? GB_BG_HFLIP : 0)
                          | (((c & 1) != 0) ? GB_BG_VFLIP : 0));
        }

    for (i = 0; i < 32; i++) {
        gbt_pal[i] = (uint8_t)(GB_BASE + (i & 31));
        gbt_spr_pal[i] = (uint8_t)(GB_BASE + 32 + (i & 31));
    }

    bg->chr = gbt_chr;
    bg->map = gbt_map;
    bg->map_attr = gbt_map_attr;
    bg->palette = gbt_pal;
    bg->scroll_x = 5;
    bg->scroll_y = 250;   /* wraps */
    bg->enabled = 1;

    oam[0].y = 16 + 30; oam[0].x = 8 + 40;  oam[0].tile = 0; oam[0].attr = GB_SPR_PAL(2);
    oam[1].y = 16 + 30; oam[1].x = 8 + 60;  oam[1].tile = 0;
    oam[1].attr = (uint8_t)(GB_SPR_PRIORITY | GB_SPR_VFLIP | GB_SPR_PAL(5));
}

int main(void)
{
    nes_bg_t nbg;
    nes_oam_entry_t noam[5];
    gb_bg_t gbg;
    gb_oam_entry_t goam[2];
    uint32_t pal256[256];
    int i;

    /* ---- ARGB->YUV converter spot checks ---- */
    {
        uint32_t black = jup_argb_to_yuv(0xFF000000);
        uint32_t white = jup_argb_to_yuv(0xFFFFFFFF);
        uint32_t red   = jup_argb_to_yuv(0xFFFF0000);
        printf("yuv black=%08x white=%08x red=%08x\n",
               (unsigned)black, (unsigned)white, (unsigned)red);
        if (black != 0x00108080) { printf("FAIL: black YUV\n"); return 1; }
        if (((white >> 16) & 0xFF) != 235) { printf("FAIL: white Y\n"); return 1; }
        if (((red >> 8) & 0xFF) >= 0x80 || (red & 0xFF) <= 0x80) {
            printf("FAIL: red chroma signs\n"); return 1;
        }
    }

    /* Display palette for the PPM dumps */
    for (i = 0; i < 256; i++) pal256[i] = 0xFF000000;
    for (i = 0; i < 64; i++) pal256[NES_BASE + i] = jup_nes_master_palette[i];
    for (i = 0; i < 64; i++)   /* GB test range: green ramp */
        pal256[GB_BASE + i] = 0xFF000000u | (uint32_t)((64 + i * 3) << 8);

    /* ---- NES frame ---- */
    memset(fb, 0, sizeof(fb));
    build_nes_scene(&nbg, noam);
    nes_render8(fb, PITCH, (FB_W - NES_NATIVE_W) / 2, (FB_H - NES_NATIVE_H) / 2,
                NES_BASE, &nbg, nes_spr_chr, noam, 5);
    printf("CRC nes_frame=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));
    dump_ppm("out_nes.ppm", pal256);

    /* Second frame with different scroll must differ */
    {
        uint32_t crc1 = crc32_buf(fb, sizeof(fb));
        nbg.scroll_x = 14;
        nes_render8(fb, PITCH, (FB_W - NES_NATIVE_W) / 2,
                    (FB_H - NES_NATIVE_H) / 2,
                    NES_BASE, &nbg, nes_spr_chr, noam, 5);
        if (crc32_buf(fb, sizeof(fb)) == crc1) {
            printf("FAIL: scroll did not change frame\n");
            return 1;
        }
    }

    /* ---- GB frame ---- */
    memset(fb, 0, sizeof(fb));
    build_gb_scene(&gbg, goam);
    gb_render8(fb, PITCH, (FB_W - GB_NATIVE_W) / 2, (FB_H - GB_NATIVE_H) / 2,
               &gbg, gbt_spr_chr, gbt_spr_pal, goam, 2);
    printf("CRC gb_frame=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));
    dump_ppm("out_gb.ppm", pal256);

    /* ---- jdraw helpers ---- */
    memset(fb, 0, sizeof(fb));
    jdraw_clear(fb, PITCH, FB_W, FB_H, 7);
    jdraw_rect(fb, PITCH, 10, 10, 100, 50, 42);
    {
        static uint8_t spr[16 * 16];
        int x, y;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                spr[y * 16 + x] = (uint8_t)(((x + y) & 1) ? 99 : 0);
        /* blit with clipping on all four edges */
        jdraw_blit_keyed(fb, PITCH, FB_W, FB_H, spr, 16, 16, -8, -8, 0);
        jdraw_blit_keyed(fb, PITCH, FB_W, FB_H, spr, 16, 16, FB_W - 8, FB_H - 8, 0);
        jdraw_blit_keyed(fb, PITCH, FB_W, FB_H, spr, 16, 16, 300, 200, 0);
    }
    printf("CRC draw=%08x\n", (unsigned)crc32_buf(fb, sizeof(fb)));

    printf("render tests OK\n");
    return 0;
}
