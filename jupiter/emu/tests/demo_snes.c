/*
 * ct952emu -- SNES showcase, executed on the emulated CT909/CT952.
 *
 * Runs the ported jsnes renderer on the emulated SPARC CPU: Mode 7's
 * affine ground projection (a checkered racetrack receding to a gradient
 * horizon, with a swirl of twist) on the left, and a Mode 1 layered
 * tilemap scene (two 4bpp BG planes over a 2bpp BG) on the right -- both
 * drawn into the shared CT952 8bpp OSD plane, snapshotted from DRAM.
 * Same machine model that boots the stock ROM; no firmware, no libc.
 */
#include "jup_types.h"
#include "jdraw.h"
#include "jsnes.h"

#define FB_W    616
#define FB_H    440
#define PITCH   FB_W
#define FB_ADDR    0x40200000u
#define PAL_ADDR   0x402C0000u

/* ---- 5x7 font ---- */
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
static void text_sh(uint8_t *fb, int x, int y, const char *s, int sc,
                    uint8_t col, uint8_t sh)
{ text(fb, x+sc, y+sc, s, sc, sh); text(fb, x, y, s, sc, col); }

static void frame(uint8_t *fb, int x, int y, int w, int h, uint8_t c)
{
    jdraw_rect(fb, PITCH, x-3, y-3, w+6, 3, c);
    jdraw_rect(fb, PITCH, x-3, y+h, w+6, 3, c);
    jdraw_rect(fb, PITCH, x-3, y-3, 3, h+6, c);
    jdraw_rect(fb, PITCH, x+w, y-3, 3, h+6, c);
}

static uint32_t g_pal[256];
static uint32_t lerp_rgb(uint32_t c0, uint32_t c1, int t, int tmax)
{
    uint32_t r = ((c0>>16&0xFF)*(uint32_t)(tmax-t)+(c1>>16&0xFF)*(uint32_t)t)/(uint32_t)tmax;
    uint32_t g = ((c0>>8&0xFF)*(uint32_t)(tmax-t)+(c1>>8&0xFF)*(uint32_t)t)/(uint32_t)tmax;
    uint32_t b = ((c0&0xFF)*(uint32_t)(tmax-t)+(c1&0xFF)*(uint32_t)t)/(uint32_t)tmax;
    return 0xFF000000u | (r<<16) | (g<<8) | b;
}

/* Palette layout:
 *  0..7    chrome
 *  16..47  Mode 7 track ramp (asphalt greys + kerb red/white + grid)
 *  48..79  Mode 1 tile palettes (8 pals x 4 used cols)
 *  96..127 sky gradient (drawn directly, not via SNES)             */
static void build_palette(void)
{
    int i;
    for (i = 0; i < 256; i++) g_pal[i] = 0xFF000000u;
    g_pal[0]=0xFF0A0E14u; g_pal[1]=0xFFFFFFFFu; g_pal[2]=0xFF141C28u;
    g_pal[3]=0xFF2B3A67u; g_pal[4]=0xFF00AEEFu; g_pal[5]=0xFFF0A030u;
    g_pal[6]=0xFFE45050u; g_pal[7]=0xFF06080Cu;
    /* Mode 7 track colours */
    g_pal[16]=0xFF3A4048u;   /* asphalt A */
    g_pal[17]=0xFF2C3238u;   /* asphalt B (checker) */
    g_pal[18]=0xFFB03030u;   /* kerb red */
    g_pal[19]=0xFFE8E8E8u;   /* kerb white / centre line */
    g_pal[20]=0xFF20A040u;   /* infield grass */
    g_pal[21]=0xFF186030u;   /* grass B */
    /* Mode 1 palettes: 8 palettes, colours 1..3 each, at 48.. */
    for (i = 0; i < 8; i++) {
        uint32_t base = lerp_rgb(0x2040A0, 0xF06020, i, 7);
        g_pal[48 + i*4 + 1] = base;
        g_pal[48 + i*4 + 2] = lerp_rgb(base & 0xFFFFFF, 0xFFFFFF, 1, 3);
        g_pal[48 + i*4 + 3] = lerp_rgb(base & 0xFFFFFF, 0x000000, 1, 3);
    }
    /* sky gradient for the Mode 7 horizon, 96..127 */
    for (i = 0; i < 32; i++)
        g_pal[96 + i] = lerp_rgb(0x1A2A55, 0xF0C0A0, i, 31);
}

/* ---- Mode 7 racetrack texture (128x128, tile-indexed into palette) ---- */
#define M7_W 128
static uint8_t m7_map[M7_W * M7_W];
static uint8_t m7_pal[256];
static void build_mode7(snes_mode7_t *m7)
{
    int x, y, i;
    for (y = 0; y < M7_W; y++)
        for (x = 0; x < M7_W; x++) {
            uint8_t v;
            int cx = x - 64, cy = y - 64;
            int rr = cx*cx + cy*cy;
            if (rr > 60*60) {                     /* infield / outfield grass */
                v = (uint8_t)(((x>>3) ^ (y>>3)) & 1 ? 20 : 21);
            } else if (rr > 54*54) {              /* kerb ring */
                v = (uint8_t)(((x>>2) + (y>>2)) & 1 ? 18 : 19);
            } else {                              /* asphalt checker + lines */
                v = (uint8_t)(((x>>4) ^ (y>>4)) & 1 ? 16 : 17);
                if (((x & 31) == 0) || ((y & 31) == 0)) v = 19;   /* grid */
            }
            m7_map[y*M7_W + x] = v;
        }
    for (i = 0; i < 256; i++) m7_pal[i] = (uint8_t)i;   /* identity -> OSD */

    m7->cam_x = 200; m7->cam_y = 130;
    m7->angle = 24; m7->twist = 0;
    m7->horizon = 74; m7->space_z = 7200;
    m7->map = m7_map; m7->palette = m7_pal;
    m7->map_w_bits = 7; m7->map_mask = M7_W - 1;
}

/* ---- Mode 1: two 4bpp planes over a 2bpp plane ---- */
static uint8_t s4[32*8], s2[16*8];
static uint16_t map_a[32*32], map_b[32*32], map_c[32*32];
static uint8_t s_pal[256];
static void s4set(uint8_t *t, int r, int c, uint8_t ci)
{ uint8_t *b=&t[r*4+(c>>1)];
  if(c&1)*b=(uint8_t)((*b&0xF0)|(ci&15)); else *b=(uint8_t)((*b&0x0F)|(uint8_t)(ci<<4)); }
static void s2set(uint8_t *t, int r, int c, uint8_t ci)
{ uint8_t m=(uint8_t)(0x80>>c); if(ci&1)t[r]|=m; if(ci&2)t[r+8]|=m; }
static void build_mode1(snes_bg_t *b1, snes_bg_t *b2, snes_bg_t *b3)
{
    int r, c, i;
    for (i = 0; i < (int)sizeof(s4); i++) s4[i] = 0;
    for (i = 0; i < (int)sizeof(s2); i++) s2[i] = 0;
    for (r = 0; r < 8; r++) for (c = 0; c < 8; c++) {
        /* tile1: gem (diamond), tile2: block frame */
        if (r+c>=3 && r+c<=11 && r-c>=-4 && r-c<=4)
            s4set(s4+32*1, r, c, (uint8_t)(2 + ((r+c) & 1)));
        if (r==0||r==7||c==0||c==7) s4set(s4+32*2, r, c, 3);
        else s4set(s4+32*2, r, c, 1);
        /* 2bpp starfield tile */
        if (((r*3 + c*5) % 11) == 0) s2set(s2+16*1, r, c, 3);
        else if (((r+c) & 3) == 0)   s2set(s2+16*1, r, c, 1);
    }
    for (i = 0; i < 32*32; i++) {
        int tx=i&31, ty=i>>5;
        map_a[i] = (uint16_t)(((tx^ty)&1) ? SNES_ENTRY(1,(tx>>2)&7,0,tx&1,ty&1) : 0);
        map_b[i] = (uint16_t)SNES_ENTRY(2,(ty>>2)&7,0,0,0);
        map_c[i] = (uint16_t)SNES_ENTRY(1,0,0,0,0);
    }
    for (i = 0; i < 256; i++) s_pal[i] = (uint8_t)(48 + (i & 31));

    b1->tiles=s4; b1->map=map_a; b1->palette=s_pal;
    b1->scroll_x=6; b1->scroll_y=2; b1->map_w=32; b1->map_h=32;
    b1->bpp=4; b1->enabled=1;
    b2->tiles=s4; b2->map=map_b; b2->palette=s_pal;
    b2->scroll_x=3; b2->scroll_y=9; b2->map_w=32; b2->map_h=32;
    b2->bpp=4; b2->enabled=1;
    b3->tiles=s2; b3->map=map_c; b3->palette=s_pal;
    b3->scroll_x=1; b3->scroll_y=1; b3->map_w=32; b3->map_h=32;
    b3->bpp=2; b3->enabled=1;
}

unsigned testmain(void)
{
    uint8_t *fb = (uint8_t *)FB_ADDR;
    uint32_t *pal = (uint32_t *)PAL_ADDR;
    snes_mode7_t m7;
    snes_bg_t b1, b2, b3;
    int x, y, i;
    int ox, oy;

    build_palette();
    jdraw_clear(fb, PITCH, FB_W, FB_H, 0);

    jdraw_rect(fb, PITCH, 0, 0, FB_W, 52, 2);
    text_sh(fb, 14, 8, "SNES ON EMULATED CT952", 3, 1, 7);
    text(fb, 14, 34, "JSNES  MODE 7 AFFINE + MODE 1 LAYERS", 2, 4);

    /* left: Mode 7 -- paint the sky above the horizon, then the ground */
    ox = 24; oy = 74;
    frame(fb, ox, oy, SNES_NATIVE_W, SNES_NATIVE_H, 5);
    build_mode7(&m7);
    for (y = 0; y < (int)m7.horizon; y++) {
        uint8_t sky = (uint8_t)(96 + (y * 31 / (m7.horizon - 1)));
        jdraw_rect(fb, PITCH, ox, oy + y, SNES_NATIVE_W, 1, sky);
    }
    snes_mode7_render8(fb, PITCH, (uint32_t)ox, (uint32_t)oy, &m7);
    text(fb, ox, oy + SNES_NATIVE_H + 10, "MODE 7 AFFINE GROUND", 2, 5);

    /* right: Mode 1 */
    ox = 24 + SNES_NATIVE_W + 40; oy = 74;
    frame(fb, ox, oy, SNES_NATIVE_W, SNES_NATIVE_H, 6);
    build_mode1(&b1, &b2, &b3);
    snes_mode1_render8(fb, PITCH, (uint32_t)ox, (uint32_t)oy, 3,
                       &b1, &b2, &b3);
    text(fb, ox, oy + SNES_NATIVE_H + 10, "MODE 1 BG1+BG2 OVER BG3", 2, 6);

    for (i = 0; i < 256; i++) pal[i] = g_pal[i];
    (void)x;
    return FB_ADDR;
}
