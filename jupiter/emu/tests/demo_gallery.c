/*
 * ct952emu -- multi-console gallery, executed on the emulated CT909/CT952.
 *
 * Runs three of the ported JupiterSDK console renderers -- NES (jnes),
 * Game Boy (jgb) and Genesis/Mega Drive (jgen) -- on the emulated SPARC
 * CPU, each drawing a composed scene into one shared CT952 8bpp OSD
 * plane, then leaves the framebuffer + palette in DRAM for the host to
 * snapshot. Same machine model that boots the stock ROM; no firmware,
 * no libc.
 *
 * The scenes are authored, not synthetic: the NES draws a platformer
 * vista (clouds, brick platform, coins, ground, two hero sprites), the
 * Genesis a sunset -- gradient sky, mountain silhouettes, shimmering
 * water and a 32x32 radial sun sprite -- and the Game Boy a classic
 * 4-shade DMG landscape with a sun and tile-set lettering.
 *
 * Palette map (OSD indices):
 *   0..15    chrome (black/white/panel/accents)
 *   64..67   Game Boy DMG shades
 *   128..191 Genesis CRAM (crafted gradients)
 *   192..255 NES master palette
 */
#include "jup_types.h"
#include "demo_de.h"
#include "jdraw.h"
#include "jnes.h"
#include "jgb.h"
#include "jgen.h"

#define FB_W    616
#define FB_H    440
#define PITCH   FB_W
#define FB_ADDR    0x40200000u
#define PAL_ADDR   0x402C0000u

/* ---- 5x7 font (space, 0-9, A-Z, '-', ':', '.') ---- */
static const unsigned char FONT[][7] = {
    {0,0,0,0,0,0,0},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},{0x00,0x04,0x00,0x00,0x00,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
};
static int gi(char ch)
{
    if (ch == ' ') return 0;
    if (ch >= '0' && ch <= '9') return 1 + (ch - '0');
    if (ch >= 'A' && ch <= 'Z') return 11 + (ch - 'A');
    if (ch == '-') return 37;
    if (ch == ':') return 38;
    if (ch == '.') return 39;
    return 0;
}
static void text(uint8_t *fb, int x, int y, const char *s, int sc, uint8_t col)
{
    int cx = x, r, c, i, j;
    for (; *s; s++) {
        const unsigned char *g = FONT[gi(*s)];
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c))
                    for (i = 0; i < sc; i++)
                        for (j = 0; j < sc; j++)
                            fb[(uint32_t)(y+r*sc+i)*PITCH + cx+c*sc+j] = col;
        cx += 6 * sc;
    }
}
/* drop-shadowed text */
static void text_sh(uint8_t *fb, int x, int y, const char *s, int sc,
                    uint8_t col, uint8_t shadow)
{
    text(fb, x + sc, y + sc, s, sc, shadow);
    text(fb, x, y, s, sc, col);
}

/* framed sub-window helper */
static void frame(uint8_t *fb, int x, int y, int w, int h, uint8_t c)
{
    jdraw_rect(fb, PITCH, x-3, y-3, w+6, 3, c);
    jdraw_rect(fb, PITCH, x-3, y+h, w+6, 3, c);
    jdraw_rect(fb, PITCH, x-3, y-3, 3, h+6, c);
    jdraw_rect(fb, PITCH, x+w, y-3, 3, h+6, c);
}

static uint32_t g_pal[256];

/* linear RGB interpolation between two 0xRRGGBB colours */
static uint32_t lerp_rgb(uint32_t c0, uint32_t c1, int t, int tmax)
{
    uint32_t r = ((c0 >> 16 & 0xFF) * (uint32_t)(tmax - t)
                + (c1 >> 16 & 0xFF) * (uint32_t)t) / (uint32_t)tmax;
    uint32_t g = ((c0 >> 8 & 0xFF) * (uint32_t)(tmax - t)
                + (c1 >> 8 & 0xFF) * (uint32_t)t) / (uint32_t)tmax;
    uint32_t b = ((c0 & 0xFF) * (uint32_t)(tmax - t)
                + (c1 & 0xFF) * (uint32_t)t) / (uint32_t)tmax;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void build_palette(void)
{
    int i;
    for (i = 0; i < 256; i++) g_pal[i] = 0xFF000000u;
    /* chrome */
    g_pal[0]=0xFF0A0E14u; g_pal[1]=0xFFFFFFFFu; g_pal[2]=0xFF141C28u;
    g_pal[3]=0xFF2B3A67u; g_pal[4]=0xFF00AEEFu; g_pal[5]=0xFFF0A030u;
    g_pal[6]=0xFFE45050u; g_pal[7]=0xFF06080Cu;
    /* Game Boy: classic 4-shade DMG green at 64..67 */
    g_pal[64]=0xFF9BBC0Fu; g_pal[65]=0xFF8BAC0Fu;
    g_pal[66]=0xFF306230u; g_pal[67]=0xFF0F380Fu;
    /* Genesis CRAM 128..191, crafted:
     * pal0 [1..12] sky gradient dusk->peach, [13..15] water tones */
    for (i = 1; i <= 12; i++)
        g_pal[128 + i] = lerp_rgb(0x122442, 0xFFBE96, i - 1, 11);
    g_pal[128+13]=0xFF16305Cu; g_pal[128+14]=0xFF1E3C6Eu;
    g_pal[128+15]=0xFFD8E8FFu;
    /* pal1: sun core -> deep orange rim */
    for (i = 1; i <= 14; i++)
        g_pal[144 + i] = lerp_rgb(0xFFFAD2, 0xE64620, i - 1, 13);
    /* pal2: mountain silhouettes (indigo shades + magenta rim light) */
    g_pal[160+1]=0xFF181030u; g_pal[160+2]=0xFF2C1C50u;
    g_pal[160+3]=0xFFFF78B4u;
    /* NES master palette at 192..255 */
    for (i = 0; i < 64; i++) g_pal[192 + i] = jup_nes_master_palette[i];
}

/* ---- string-art tile builders ---- */

/* planar 2bpp (NES + GB share the format): 8 rows of '0'..'3' */
static void tile2bpp(uint8_t *t, const char art[8][9])
{
    int r, c;
    for (r = 0; r < 8; r++) {
        t[r] = 0; t[r + 8] = 0;
        for (c = 0; c < 8; c++) {
            uint8_t ci = (uint8_t)(art[r][c] - '0');
            uint8_t m = (uint8_t)(0x80 >> c);
            if (ci & 1) t[r] |= m;
            if (ci & 2) t[r + 8] |= m;
        }
    }
}

/* Genesis 4bpp: 8 rows of hex chars */
static void tile4bpp(uint8_t *t, const char art[8][9])
{
    int r, c;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            char ch = art[r][c];
            uint8_t ci = (uint8_t)(ch <= '9' ? ch - '0' : ch - 'A' + 10);
            uint8_t *b = &t[r * 4 + (c >> 1)];
            if (c & 1) *b = (uint8_t)((*b & 0xF0) | ci);
            else       *b = (uint8_t)((*b & 0x0F) | (uint8_t)(ci << 4));
        }
}

/* ================= NES: platformer vista ================= */

static const char ART_CLOUD[8][9] = {
    "00022000", "02222200", "22222220", "22222222",
    "22222222", "02222220", "00000000", "00000000" };
static const char ART_BRICK[8][9] = {
    "22212222", "22212222", "11111111", "22221222",
    "22221222", "11111111", "22212222", "22212222" };
static const char ART_GRASS[8][9] = {
    "33333333", "23332333", "11111111", "11211121",
    "11111111", "12111211", "11111111", "11111111" };
static const char ART_DIRT[8][9] = {
    "11111111", "11121111", "11111211", "11111111",
    "12111112", "11111111", "11112111", "21111111" };
static const char ART_COIN[8][9] = {
    "00033000", "00333300", "03322330", "03322330",
    "03322330", "03322330", "00333300", "00033000" };
static const char ART_HERO[8][9] = {
    "00111100", "01111110", "01222210", "01222210",
    "00111100", "01111110", "00333300", "00330330" };
static const char ART_STAR[8][9] = {
    "00020000", "00222000", "02212200", "22111220",
    "02212200", "00222000", "00020000", "00000000" };

static uint8_t nchr[16*6], nspr_chr[16*2];
static uint8_t nnt[NES_NT_TILES], nattr[NES_NT_ATTRS], npal[32];
static nes_oam_entry_t noam[4];

static void build_nes(nes_bg_t *bg)
{
    int i, tx, ty;
    tile2bpp(nchr + 16*1, ART_CLOUD);
    tile2bpp(nchr + 16*2, ART_BRICK);
    tile2bpp(nchr + 16*3, ART_GRASS);
    tile2bpp(nchr + 16*4, ART_DIRT);
    tile2bpp(nchr + 16*5, ART_COIN);
    tile2bpp(nspr_chr + 16*0, ART_HERO);
    tile2bpp(nspr_chr + 16*1, ART_STAR);

    for (i = 0; i < NES_NT_TILES; i++) nnt[i] = 0;          /* sky */
    /* clouds */
    nnt[3*32 + 4] = 1;  nnt[3*32 + 5] = 1;
    nnt[5*32 + 15] = 1; nnt[5*32 + 16] = 1;
    nnt[2*32 + 24] = 1; nnt[2*32 + 25] = 1;
    nnt[8*32 + 9] = 1;
    /* brick platform + coins above it */
    for (tx = 10; tx <= 17; tx++) nnt[14*32 + tx] = 2;
    nnt[12*32 + 11] = 5; nnt[12*32 + 13] = 5; nnt[12*32 + 15] = 5;
    /* ground: grass cap + dirt */
    for (tx = 0; tx < 32; tx++) {
        nnt[24*32 + tx] = 3;
        for (ty = 25; ty < 30; ty++) nnt[ty*32 + tx] = 4;
    }
    /* attributes: sky pal0, platform band pal1, ground pal2 */
    for (i = 0; i < NES_NT_ATTRS; i++) {
        int ax = i & 7, ay = i >> 3;
        uint8_t p = 0;
        if (ay >= 6) p = 2;
        else if (ay == 3 && ax >= 2 && ax <= 4) p = 1;
        nattr[i] = (uint8_t)(p | (p << 2) | (p << 4) | (p << 6));
    }
    /* BG palettes: 0 sky/cloud, 1 brick/coin, 2 ground */
    npal[0]=0x21;
    npal[1]=0x31; npal[2]=0x30; npal[3]=0x3C;          /* cloud whites  */
    npal[5]=0x07; npal[6]=0x16; npal[7]=0x27;          /* brick + coin  */
    npal[9]=0x17; npal[10]=0x07; npal[11]=0x29;        /* dirt + grass  */
    /* sprite palettes: 0 hero, 1 star */
    npal[17]=0x16; npal[18]=0x36; npal[19]=0x0F;
    npal[21]=0x28; npal[22]=0x30; npal[23]=0x30;

    noam[0].y=183; noam[0].x=60;  noam[0].tile=0; noam[0].attr=0;
    noam[1].y=103; noam[1].x=110; noam[1].tile=0; noam[1].attr=0;
    noam[2].y=54;  noam[2].x=196; noam[2].tile=1; noam[2].attr=1;
    noam[3].y=76;  noam[3].x=48;  noam[3].tile=1; noam[3].attr=1;

    bg->chr=nchr; bg->nametable=nnt; bg->attribute=nattr; bg->palette_ram=npal;
    bg->line_scroll_x=0; bg->scroll_x=0; bg->scroll_y=0; bg->enabled=1;
}

/* ================= Game Boy: DMG landscape ================= */

static const char ART_GB_SUN[8][9] = {
    "00033000", "00322300", "03200230", "32000023",
    "32000023", "03200230", "00322300", "00033000" };
static const char ART_GB_HILL[8][9] = {
    "12121212", "21212121", "12121212", "21212121",
    "12121212", "21212121", "12121212", "21212121" };
static const char ART_GB_DARK[8][9] = {
    "33333333", "33333333", "33333333", "33333333",
    "33333333", "33333333", "33333333", "33333333" };
static const char ART_GB_SPARK[8][9] = {
    "33333333", "33233333", "33333333", "33333323",
    "32333333", "33333333", "33332333", "33333333" };

static uint8_t gchr[16*16], gmap[32*32], gpalette[32];

static void build_gb(gb_bg_t *bg)
{
    static const char word[] = "JUPITER";
    int i, tx, ty, r, c;
    tile2bpp(gchr + 16*1, ART_GB_SUN);
    tile2bpp(gchr + 16*2, ART_GB_HILL);
    tile2bpp(gchr + 16*3, ART_GB_DARK);
    tile2bpp(gchr + 16*4, ART_GB_SPARK);
    /* letter tiles 8..14 built from the 5x7 chrome font */
    for (i = 0; word[i]; i++) {
        uint8_t *t = gchr + 16 * (8 + i);
        const unsigned char *g = FONT[gi(word[i])];
        char art[8][9];
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++)
                art[r][c] = (char)((r < 7 && c >= 1 && c <= 5 &&
                                    (g[r] & (0x10 >> (c - 1)))) ? '3' : '0');
        tile2bpp(t, art);
    }
    for (i = 0; i < 32*32; i++) gmap[i] = 0;               /* sky */
    gmap[2*32 + 2] = 1;                                    /* sun */
    for (i = 0; word[i]; i++) gmap[5*32 + 6 + i] = (uint8_t)(8 + i);
    for (ty = 10; ty <= 13; ty++)                          /* dither hills */
        for (tx = 0; tx < 32; tx++) gmap[ty*32 + tx] = 2;
    for (ty = 14; ty < 32; ty++)                           /* dark ground */
        for (tx = 0; tx < 32; tx++)
            gmap[ty*32 + tx] = (uint8_t)(((tx*5 + ty) % 7) < 1 ? 4 : 3);

    gpalette[0]=64; gpalette[1]=65; gpalette[2]=66; gpalette[3]=67;
    bg->chr=gchr; bg->map=gmap; bg->map_attr=0; bg->palette=gpalette;
    bg->scroll_x=0; bg->scroll_y=0; bg->enabled=1;
}

/* ================= Genesis: sunset ================= */

/* mountain slope tiles: '/' up-slope (fill below the diagonal) */
static const char ART_GEN_SLOPE[8][9] = {
    "00000003", "00000032", "00000321", "00003211",
    "00032111", "00321111", "03211111", "32111111" };
static const char ART_GEN_SOLID[8][9] = {
    "11111111", "11111111", "11111111", "11111111",
    "11111111", "11111111", "11111111", "11111111" };
static const char ART_GEN_RIDGE[8][9] = {
    "11111111", "21111112", "11111111", "11111111",
    "11121111", "11111111", "11111121", "11111111" };
static const char ART_GEN_WATER[8][9] = {
    "DDDDDDDD", "DEDDDDFD", "EEEEEEEE", "DDDDDDDD",
    "EDDFDDDE", "EEEEEEEE", "DDDDDDDD", "DDFDDDDD" };

#define GEN_NTILES 26          /* 0..7 scenery, 8..23 sun orb, 24 sparkle */
static uint8_t gt_tiles[32 * GEN_NTILES];
static uint16_t ga_map[32*32], gb_map[32*32];
static uint8_t gcram[64];
static genesis_sprite_t gspr[3];

static void gtile_px(uint8_t *tile, int r, int c, uint8_t ci)
{ uint8_t *b=&tile[r*4 + (c>>1)];
  if (c & 1) *b = (uint8_t)((*b & 0xF0) | (ci & 0x0F));
  else       *b = (uint8_t)((*b & 0x0F) | (uint8_t)(ci << 4)); }

static void build_gen(genesis_plane_t *pa, genesis_plane_t *pb)
{
    int r, c, tx, ty, i;
    /* sky gradient bands: tiles 1..3, each 4 gradient steps deep */
    for (i = 0; i < 3; i++)
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++)
                gtile_px(gt_tiles + 32*(1+i), r, c,
                         (uint8_t)(1 + i*4 + (r >> 1)));
    tile4bpp(gt_tiles + 32*4, ART_GEN_SLOPE);
    tile4bpp(gt_tiles + 32*5, ART_GEN_SOLID);
    tile4bpp(gt_tiles + 32*6, ART_GEN_RIDGE);
    tile4bpp(gt_tiles + 32*7, ART_GEN_WATER);
    /* sun orb: 32x32 radial in tiles 8..23 (column-major 4x4) */
    for (r = 0; r < 32; r++)
        for (c = 0; c < 32; c++) {
            int dr = r - 16, dc = c - 16, d2 = dr*dr + dc*dc;
            if (d2 <= 225) {
                uint8_t ci = (uint8_t)(1 + (d2 * 13) / 226);
                uint8_t *t = gt_tiles + 32 * (8 + (c >> 3) * 4 + (r >> 3));
                gtile_px(t, r & 7, c & 7, ci);
            }
        }
    /* water sparkle */
    tile4bpp(gt_tiles + 32*24, ART_GEN_WATER);

    /* plane B: sky bands then water */
    for (ty = 0; ty < 32; ty++)
        for (tx = 0; tx < 32; tx++) {
            uint16_t t;
            if      (ty <= 6)  t = 1;
            else if (ty <= 13) t = 2;
            else if (ty <= 20) t = 3;
            else               t = 7;                       /* water */
            gb_map[ty*32 + tx] = (uint16_t)GEN_ENTRY(t, 0, 0, 0);
        }
    /* plane A: sawtooth mountain silhouettes above the waterline */
    for (ty = 0; ty < 32; ty++)
        for (tx = 0; tx < 32; tx++) ga_map[ty*32 + tx] = 0;
    for (tx = 0; tx < 32; tx++) {
        static const int prof[6] = {0, 1, 2, 3, 2, 1};
        int h = prof[tx % 6];
        int top = 18 - h;
        for (ty = top; ty <= 20; ty++)
            ga_map[ty*32 + tx] = (uint16_t)
                GEN_ENTRY(ty == top ? 4 : (ty == top+1 ? 6 : 5), 2,
                          (tx % 6) > 3, 0);
    }
    for (i = 0; i < 64; i++) gcram[i] = (uint8_t)(128 + i);

    pb->tiles=gt_tiles; pb->map=gb_map; pb->cram=gcram;
    pb->scroll_x=0; pb->scroll_y=0; pb->line_hscroll=0;
    pb->map_w=32; pb->map_h=32; pb->enabled=1;
    pa->tiles=gt_tiles; pa->map=ga_map; pa->cram=gcram;
    pa->scroll_x=0; pa->scroll_y=0; pa->line_hscroll=0;
    pa->map_w=32; pa->map_h=32; pa->enabled=1;

    /* sun over the water, twin sparkles on it */
    gspr[0].x=212; gspr[0].y=52;  gspr[0].tile=8;  gspr[0].w=4; gspr[0].h=4;
    gspr[0].pal=1; gspr[0].fliph=0; gspr[0].flipv=0; gspr[0].enabled=1;
    gspr[1].x=100; gspr[1].y=178; gspr[1].tile=24; gspr[1].w=1; gspr[1].h=1;
    gspr[1].pal=0; gspr[1].fliph=0; gspr[1].flipv=0; gspr[1].enabled=1;
    gspr[2].x=240; gspr[2].y=190; gspr[2].tile=24; gspr[2].w=1; gspr[2].h=1;
    gspr[2].pal=0; gspr[2].fliph=1; gspr[2].flipv=0; gspr[2].enabled=1;
}

unsigned testmain(void)
{
    uint8_t *fb = (uint8_t *)FB_ADDR;
    uint32_t *pal = (uint32_t *)PAL_ADDR;
    nes_bg_t nbg;
    gb_bg_t  gbg;
    genesis_plane_t gpa, gpb;
    int i;

    build_palette();
    jdraw_clear(fb, PITCH, FB_W, FB_H, 0);

    /* title band */
    jdraw_rect(fb, PITCH, 0, 0, FB_W, 52, 2);
    text_sh(fb, 14, 8,  "CT952 CONSOLE RENDERERS", 3, 1, 7);
    text(fb, 14, 34, "JNES  JGB  JGEN  ON EMULATED SPARC", 2, 4);

    /* NES 256x224 top-left */
    build_nes(&nbg);
    frame(fb, 12, 66, NES_NATIVE_W, NES_NATIVE_H, 6);
    nes_render8(fb, PITCH, 12, 66, 192, &nbg, nspr_chr, noam, 4);
    text(fb, 12, 66 + NES_NATIVE_H + 8, "NES 256X224", 2, 6);

    /* Genesis 320x224 top-right */
    build_gen(&gpa, &gpb);
    frame(fb, 284, 66, GEN_NATIVE_W, GEN_NATIVE_H, 4);
    genesis_render8(fb, PITCH, 284, 66, 128 + 13, &gpa, &gpb, NULL, gspr, 3);
    text(fb, 284, 66 + GEN_NATIVE_H + 8, "GENESIS 320X224", 2, 4);

    /* Game Boy 160x144 bottom-left */
    build_gb(&gbg);
    frame(fb, 12, 320, 160, 144, 5);
    gb_render8(fb, PITCH, 12, 320, &gbg, NULL, NULL, NULL, 0);
    text(fb, 12, 320 + 144 + 4, "GAME BOY 160X144", 2, 5);

    /* caption block */
    text(fb, 196, 328, "THREE PORTED", 2, 1);
    text(fb, 196, 350, "RENDERERS,", 2, 1);
    text(fb, 196, 372, "ONE 8BPP OSD", 2, 5);
    text(fb, 196, 394, "PLANE, DRAWN", 2, 1);
    text(fb, 196, 416, "ON THE EMU CPU", 2, 4);

    for (i = 0; i < 256; i++) pal[i] = g_pal[i];
    /* program the real display engine so the emulator scans it out */
    de_program(FB_ADDR, FB_W, FB_H, g_pal);
    return FB_ADDR;
}
