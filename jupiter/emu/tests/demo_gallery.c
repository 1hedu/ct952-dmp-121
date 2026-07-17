/*
 * ct952emu -- multi-console gallery, executed on the emulated CT909/CT952.
 *
 * Runs three of the ported JupiterSDK console renderers -- NES (jnes),
 * Game Boy (jgb) and Genesis/Mega Drive (jgen) -- on the emulated SPARC
 * CPU, each drawing a live scene into one shared CT952 8bpp OSD plane,
 * then leaves the framebuffer + palette in DRAM for the host to snapshot.
 * Same machine model that boots the stock ROM; no firmware, no libc.
 *
 * Palette map (OSD indices):
 *   0..15    chrome (black/white/panel/accents)
 *   64..95   Game Boy shades
 *   128..191 Genesis CRAM
 *   192..255 NES master palette
 */
#include "jup_types.h"
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

/* framed sub-window helper */
static void frame(uint8_t *fb, int x, int y, int w, int h, uint8_t c)
{
    jdraw_rect(fb, PITCH, x-3, y-3, w+6, 3, c);
    jdraw_rect(fb, PITCH, x-3, y+h, w+6, 3, c);
    jdraw_rect(fb, PITCH, x-3, y-3, 3, h+6, c);
    jdraw_rect(fb, PITCH, x+w, y-3, 3, h+6, c);
}

static uint32_t g_pal[256];

static void build_palette(void)
{
    int i;
    for (i = 0; i < 256; i++) g_pal[i] = 0xFF000000u;
    g_pal[0]=0xFF0A0E14u; g_pal[1]=0xFFFFFFFFu; g_pal[2]=0xFF141C28u;
    g_pal[3]=0xFF2B3A67u; g_pal[4]=0xFF00AEEFu; g_pal[5]=0xFFF0A030u;
    g_pal[6]=0xFFE45050u;
    /* Game Boy: classic 4-shade green at 64..67 */
    g_pal[64]=0xFF9BBC0Fu; g_pal[65]=0xFF8BAC0Fu;
    g_pal[66]=0xFF306230u; g_pal[67]=0xFF0F380Fu;
    /* Genesis CRAM at 128..191: 3-level-per-channel spread (RGB from index) */
    for (i = 0; i < 64; i++) {
        uint32_t r = ((i >> 4) & 3) * 85, gg = ((i >> 2) & 3) * 85,
                 b = (i & 3) * 85;
        g_pal[128 + i] = 0xFF000000u | (r << 16) | (gg << 8) | b;
    }
    g_pal[128] = 0xFF102848u;   /* Genesis backdrop */
    /* NES master palette at 192..255 */
    for (i = 0; i < 64; i++) g_pal[192 + i] = jup_nes_master_palette[i];
}

/* planar 2bpp tile helper (NES + GB share this format) */
static void chr_set(uint8_t *t, int r, int c, uint8_t ci)
{ uint8_t m=(uint8_t)(0x80>>c); if(ci&1)t[r]|=m; if(ci&2)t[r+8]|=m; }

/* ---- NES scene ---- */
static uint8_t nchr[16*4], nnt[NES_NT_TILES], nattr[NES_NT_ATTRS], npal[32];
static int16_t nscroll[NES_NATIVE_H];
static void build_nes(nes_bg_t *bg)
{
    int r, c, i;
    for (i = 0; i < (int)sizeof(nchr); i++) nchr[i] = 0;
    for (r = 0; r < 8; r++) for (c = 0; c < 8; c++) {
        if (r+c>=3 && r+c<=11 && r-c>=-4 && r-c<=4) chr_set(nchr+16,r,c,3);
        if (r==0||r==7||c==0||c==7) chr_set(nchr+32,r,c,2);
    }
    for (i = 0; i < NES_NT_TILES; i++) nnt[i]=(uint8_t)(((i/32+i)&1)?1:2);
    for (i = 0; i < NES_NT_ATTRS; i++) nattr[i]=(uint8_t)(i & 0x3F);
    npal[0]=0x21; npal[1]=0x0F; npal[2]=0x16; npal[3]=0x30;
    npal[5]=0x0F; npal[6]=0x27; npal[7]=0x30; npal[9]=0x0F; npal[10]=0x1A;
    npal[11]=0x30; npal[13]=0x0F; npal[14]=0x12; npal[15]=0x30;
    for (i = 0; i < NES_NATIVE_H; i++) nscroll[i]=(int16_t)(i/6);
    bg->chr=nchr; bg->nametable=nnt; bg->attribute=nattr; bg->palette_ram=npal;
    bg->line_scroll_x=nscroll; bg->scroll_x=0; bg->scroll_y=0; bg->enabled=1;
}

/* ---- Game Boy scene ---- */
static uint8_t gchr[16*4], gmap[32*32], gpalette[32];
static void build_gb(gb_bg_t *bg)
{
    int r, c, i;
    for (i = 0; i < (int)sizeof(gchr); i++) gchr[i] = 0;
    /* tile 1: solid ring, tile 2: diagonal stripes, tile 3: filled */
    for (r = 0; r < 8; r++) for (c = 0; c < 8; c++) {
        if (r==0||r==7||c==0||c==7) chr_set(gchr+16, r, c, 3);
        else if (r==1||r==6||c==1||c==6) chr_set(gchr+16, r, c, 2);
        if (((r+c) & 3) < 2) chr_set(gchr+32, r, c, 1);
        chr_set(gchr+48, r, c, 2);
    }
    for (i = 0; i < 32*32; i++) {
        int tx=i&31, ty=i>>5;
        gmap[i] = (uint8_t)(((tx^ty)&1) ? 1 : (((tx+ty)&3)==0 ? 3 : 2));
    }
    /* palette entries are OSD indices; DMG uses palette[0..3] */
    gpalette[0]=64; gpalette[1]=65; gpalette[2]=66; gpalette[3]=67;
    bg->chr=gchr; bg->map=gmap; bg->map_attr=0; bg->palette=gpalette;
    bg->scroll_x=0; bg->scroll_y=0; bg->enabled=1;
}

/* ---- Genesis scene ---- */
static uint8_t gt_tiles[32*8];          /* 8 tiles, 32 bytes each (4bpp) */
static uint16_t ga_map[32*32], gb_map[32*32];
static uint8_t gcram[64];
static void gtile_px(uint8_t *tile, int r, int c, uint8_t ci)
{ uint8_t *b=&tile[r*4 + (c>>1)];
  if (c & 1) *b = (uint8_t)((*b & 0xF0) | (ci & 0x0F));
  else       *b = (uint8_t)((*b & 0x0F) | (ci << 4)); }
static void build_gen(genesis_plane_t *pa, genesis_plane_t *pb)
{
    int r, c, i;
    for (i = 0; i < (int)sizeof(gt_tiles); i++) gt_tiles[i] = 0;
    /* tile 1: radial gradient block; tile 2: frame; tile 3: h-gradient */
    for (r = 0; r < 8; r++) for (c = 0; c < 8; c++) {
        int d = (r-4)*(r-4) + (c-4)*(c-4);
        gtile_px(gt_tiles + 32*1, r, c, (uint8_t)(1 + (d & 0x0E)));
        if (r==0||r==7||c==0||c==7) gtile_px(gt_tiles + 32*2, r, c, 15);
        else gtile_px(gt_tiles + 32*2, r, c, (uint8_t)(4 + ((r+c)&7)));
        gtile_px(gt_tiles + 32*3, r, c, (uint8_t)(1 + (c*2 & 0x0E)));
    }
    for (i = 0; i < 32*32; i++) {
        int tx=i&31, ty=i>>5;
        ga_map[i] = (uint16_t)GEN_ENTRY(((tx^ty)&1)?2:0, (tx>>3)&3, 0, 0);
        gb_map[i] = (uint16_t)GEN_ENTRY(((tx+ty)&1)?1:3, (ty>>3)&3, 0, 0);
    }
    /* CRAM: 4 palettes x 16 colours, all mapped into OSD 128..191 */
    for (i = 0; i < 64; i++) gcram[i] = (uint8_t)(128 + i);
    gcram[0] = 128;   /* transparent-ish backdrop tone */
    pb->tiles=gt_tiles; pb->map=gb_map; pb->cram=gcram;
    pb->scroll_x=0; pb->scroll_y=0; pb->line_hscroll=0;
    pb->map_w=32; pb->map_h=32; pb->enabled=1;
    pa->tiles=gt_tiles; pa->map=ga_map; pa->cram=gcram;
    pa->scroll_x=0; pa->scroll_y=0; pa->line_hscroll=0;
    pa->map_w=32; pa->map_h=32; pa->enabled=1;
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
    text(fb, 14, 8,  "CT952 CONSOLE RENDERERS", 3, 1);
    text(fb, 14, 34, "JNES  JGB  JGEN  ON EMULATED SPARC", 2, 4);

    /* NES 256x224 top-left */
    build_nes(&nbg);
    frame(fb, 12, 66, NES_NATIVE_W, NES_NATIVE_H, 6);
    nes_render8(fb, PITCH, 12, 66, 192, &nbg, NULL, NULL, 0);
    text(fb, 12, 66 + NES_NATIVE_H + 8, "NES 256X224", 2, 6);

    /* Genesis 320x224 top-right */
    build_gen(&gpa, &gpb);
    frame(fb, 284, 66, GEN_NATIVE_W, GEN_NATIVE_H, 4);
    genesis_render8(fb, PITCH, 284, 66, 128, &gpa, &gpb, NULL, NULL, 0);
    text(fb, 284, 66 + GEN_NATIVE_H + 8, "GENESIS 320X224", 2, 4);

    /* Game Boy 160x144 bottom-left */
    build_gb(&gbg);
    frame(fb, 12, 320, 160, 144, 5);
    gb_render8(fb, PITCH, 12, 320, &gbg, NULL, NULL, NULL, 0);
    text(fb, 12, 320 + 144 + 4, "GAME BOY 160X144", 2, 5);

    /* caption block, bottom-right of the GB panel */
    text(fb, 196, 328, "THREE PORTED", 2, 1);
    text(fb, 196, 350, "RENDERERS,", 2, 1);
    text(fb, 196, 372, "ONE 8BPP OSD", 2, 5);
    text(fb, 196, 394, "PLANE, DRAWN", 2, 1);
    text(fb, 196, 416, "ON THE EMU CPU", 2, 4);

    for (i = 0; i < 256; i++) pal[i] = g_pal[i];
    return FB_ADDR;
}
