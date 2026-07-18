/*
 * ct952emu -- SDK splash demo, executed on the emulated CT909/CT952.
 *
 * This is real JupiterSDK code compiled for SPARC V8 and run inside the
 * ct952emu CPU/machine model (the same interpreter that boots the stock
 * ROM). It draws an intentional boot screen -- a colour-bar test card, a
 * title band with a built-in 5x7 font, a palette ramp, and the ported
 * NES background renderer (jnes) drawing a live scene -- into the CT952
 * OSD's native 8bpp indexed framebuffer, using the real BT.601 palette
 * built by jrgb2yuv. The framebuffer + a 256-entry ARGB palette are left
 * in DRAM for the host driver to snapshot.
 *
 * There is no firmware, no libc: start.S sets up the trap table and
 * stack and calls testmain(). Everything below runs on the emulated
 * SPARC exactly as it would on the metal.
 */
#include "jup_types.h"
#include "jdraw.h"
#include "jnes.h"
#include "jrgb2yuv.h"

#define FB_W    616
#define FB_H    440
#define PITCH   FB_W

/* Fixed DRAM staging (inside the emulator's 8 MB DRAM; the stack lives
 * at 0x40100000 and grows down, well clear of these). */
#define FB_ADDR    0x40200000u   /* 616*440 8bpp indices          */
#define PAL_ADDR   0x402C0000u   /* 256 * 4 bytes ARGB            */

/* ---- a compact 5x7 font: space, 0-9, A-Z, '-', ':', '.' ---- */
/* Each glyph is 7 rows of a 5-bit mask (bit4 = leftmost column). */
static const unsigned char FONT[][7] = {
    {0,0,0,0,0,0,0},                                  /* ' ' */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},             /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},             /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},             /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},             /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},             /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},             /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},             /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},             /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},             /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},             /* 9 */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},             /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},             /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},             /* C */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},             /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},             /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},             /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},             /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},             /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},             /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},             /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},             /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},             /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},             /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},             /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},             /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},             /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},             /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},             /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},             /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},             /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},             /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},             /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},             /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},             /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},             /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},             /* Z */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},             /* - */
    {0x00,0x04,0x00,0x00,0x00,0x04,0x00},             /* : */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},             /* . */
};

static int glyph_index(char ch)
{
    if (ch == ' ') return 0;
    if (ch >= '0' && ch <= '9') return 1 + (ch - '0');
    if (ch >= 'A' && ch <= 'Z') return 11 + (ch - 'A');
    if (ch == '-') return 37;
    if (ch == ':') return 38;
    if (ch == '.') return 39;
    return 0;
}

/* Draw a string at (x,y), scaled sx/sy, in palette index `color`. */
static void text(uint8_t *fb, int x, int y, const char *s,
                 int sx, int sy, uint8_t color)
{
    int col = x, r, c, i;
    for (; *s; s++) {
        const unsigned char *g = FONT[glyph_index(*s)];
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c)) {
                    for (i = 0; i < sy; i++) {
                        int py = y + r * sy + i, px;
                        for (px = 0; px < sx; px++)
                            fb[(uint32_t)py * PITCH + col + c * sx + px] = color;
                    }
                }
        col += (5 * sx) + sx;   /* one-column gap */
    }
}

/* ---- palette layout (indices we draw with) ----
 * 0   transparent/black    8..15  SMPTE-ish bars
 * 16..79  64-step grey ramp   80..87 accents
 * 128..255 NES master palette (jnes uses its own indices; we map them
 *          into this range so the NES sub-window composites cleanly). */
static uint32_t g_pal[256];

static void build_palette(void)
{
    int i;
    for (i = 0; i < 256; i++) g_pal[i] = 0xFF000000u;
    g_pal[0]  = 0xFF000000u;                 /* black   */
    g_pal[1]  = 0xFFFFFFFFu;                 /* white   */
    g_pal[2]  = 0xFF101820u;                 /* panel   */
    g_pal[3]  = 0xFF2B3A67u;                 /* deep blue */
    g_pal[4]  = 0xFF00AEEFu;                 /* cyan accent */
    /* SMPTE-style colour bars (8..14) */
    g_pal[8]  = 0xFFC0C0C0u;                 /* grey  */
    g_pal[9]  = 0xFFC0C000u;                 /* yellow*/
    g_pal[10] = 0xFF00C0C0u;                 /* cyan  */
    g_pal[11] = 0xFF00C000u;                 /* green */
    g_pal[12] = 0xFFC000C0u;                 /* magenta */
    g_pal[13] = 0xFFC00000u;                 /* red   */
    g_pal[14] = 0xFF0000C0u;                 /* blue  */
    /* 64-step grey ramp at 16..79 */
    for (i = 0; i < 64; i++) {
        uint32_t v = (uint32_t)(i * 255 / 63);
        g_pal[16 + i] = 0xFF000000u | (v << 16) | (v << 8) | v;
    }
    /* a warm gradient at 80..143 for the title band */
    for (i = 0; i < 64; i++) {
        uint32_t r = (uint32_t)(40 + i * 3), b = (uint32_t)(120 - i);
        if (r > 255) r = 255;
        g_pal[80 + i] = 0xFF000000u | (r << 16) | ((uint32_t)(30 + i) << 8) | b;
    }
}

/* jnes writes OSD indices [192..255]; load the true NES master palette
 * (ARGB8888) there so the sub-window shows real NES colours. */
static void install_nes_palette(void)
{
    int i;
    for (i = 0; i < 64; i++)
        g_pal[192 + i] = jup_nes_master_palette[i];
}

/* ---- NES scene (a small deterministic tiled background + one sprite) */
static uint8_t nes_chr[16 * 4];
static uint8_t nes_nt[NES_NT_TILES];
static uint8_t nes_attr[NES_NT_ATTRS];
static uint8_t nes_palram[32];
static int16_t nes_scroll[NES_NATIVE_H];

static void chr_set(uint8_t *t, int row, int col, uint8_t ci)
{
    uint8_t m = (uint8_t)(0x80 >> col);
    if (ci & 1) t[row] |= m;
    if (ci & 2) t[row + 8] |= m;
}

static void build_nes(nes_bg_t *bg)
{
    int r, c, i;
    for (i = 0; i < (int)sizeof(nes_chr); i++) nes_chr[i] = 0;
    /* tile 1: filled diamond, tile 2: frame */
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            if (r + c >= 3 && r + c <= 11 && r - c >= -4 && r - c <= 4)
                chr_set(nes_chr + 16, r, c, 3);
            if (r == 0 || r == 7 || c == 0 || c == 7)
                chr_set(nes_chr + 32, r, c, 2);
        }
    for (i = 0; i < NES_NT_TILES; i++)
        nes_nt[i] = (uint8_t)(((i / 32 + i) & 1) ? 1 : 2);
    for (i = 0; i < NES_NT_ATTRS; i++) nes_attr[i] = (uint8_t)(i & 0x3F);
    /* a pleasant NES sub-palette */
    nes_palram[0] = 0x21;                       /* sky blue bg */
    nes_palram[1] = 0x0F; nes_palram[2] = 0x16; nes_palram[3] = 0x30;
    nes_palram[5] = 0x0F; nes_palram[6] = 0x27; nes_palram[7] = 0x30;
    nes_palram[9] = 0x0F; nes_palram[10]= 0x1A; nes_palram[11]= 0x30;
    nes_palram[13]= 0x0F; nes_palram[14]= 0x12; nes_palram[15]= 0x30;
    for (i = 0; i < NES_NATIVE_H; i++) nes_scroll[i] = (int16_t)(i / 6);

    bg->chr = nes_chr; bg->nametable = nes_nt; bg->attribute = nes_attr;
    bg->palette_ram = nes_palram; bg->line_scroll_x = nes_scroll;
    bg->scroll_x = 0; bg->scroll_y = 0; bg->enabled = 1;
}

unsigned testmain(void)
{
    uint8_t *fb = (uint8_t *)FB_ADDR;
    uint32_t *pal = (uint32_t *)PAL_ADDR;
    nes_bg_t bg;
    int x, y, i;

    build_palette();
    install_nes_palette();

    /* background: dark panel */
    jdraw_clear(fb, PITCH, FB_W, FB_H, 2);

    /* top title band: warm gradient + text */
    for (y = 0; y < 64; y++)
        jdraw_rect(fb, PITCH, 0, y, FB_W, 1, (uint8_t)(80 + (y * 63 / 63) % 64));
    text(fb, 20, 12, "JUPITER SDK", 4, 4, 1);
    text(fb, 20, 44, "RUNNING ON EMULATED CT952 SPARC", 2, 2, 4);

    /* SMPTE-style colour bars, mid strip */
    {
        static const uint8_t bar[7] = {8,9,10,11,12,13,14};
        int bw = FB_W / 7;
        for (i = 0; i < 7; i++)
            jdraw_rect(fb, PITCH, i * bw, 80, bw, 70, bar[i]);
    }

    /* 64-step grey ramp below the bars */
    for (x = 0; x < FB_W; x++)
        jdraw_rect(fb, PITCH, x, 152, 1, 24, (uint8_t)(16 + (x * 63 / (FB_W - 1))));

    /* NES renderer sub-window: real jnes output, rendered natively
     * (256x224) straight into the OSD plane at indices [192..255]. */
    build_nes(&bg);
    {
        int ox = 24, oy = 200;
        jdraw_rect(fb, PITCH, ox - 4, oy - 4,
                   NES_NATIVE_W + 8, NES_NATIVE_H + 8, 4);
        nes_render8(fb, PITCH, (uint32_t)ox, (uint32_t)oy, 192,
                    &bg, NULL, NULL, 0);
        text(fb, ox + NES_NATIVE_W + 16, oy + 8,  "JNES BG", 3, 3, 1);
        text(fb, ox + NES_NATIVE_W + 16, oy + 40, "RENDERER", 3, 3, 4);
        text(fb, ox + NES_NATIVE_W + 16, oy + 84, "8BPP OSD PLANE", 2, 2, 8);
        text(fb, ox + NES_NATIVE_W + 16, oy + 108,"BT.601 PALETTE", 2, 2, 10);
        text(fb, ox + NES_NATIVE_W + 16, oy + 132,"256X224 NATIVE", 2, 2, 11);
    }

    /* footer */
    text(fb, 20, FB_H - 20, "PORTED FROM LICHEE ZERO V3S", 2, 2, 8);

    /* publish the palette for the host snapshot */
    for (i = 0; i < 256; i++) pal[i] = g_pal[i];

    return FB_ADDR;
}
