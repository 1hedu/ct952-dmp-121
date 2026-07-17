/*
 * JupiterSDK on CT952 -- demo application.
 *
 * Five scenes, switched with LEFT/RIGHT:
 *   0  color bars    -- palette / region bring-up check (static image)
 *   1  NES demo      -- jnes renderer: scrolling attributed background +
 *                      bouncing OAM sprites, APU jingle on entry
 *   2  Game Boy demo -- jgb renderer: DMG-green scrolling background +
 *                      sprites
 *   3  Genesis demo  -- jgen renderer: two-plane parallax + window HUD +
 *                      sprites
 *   4  SNES Mode 7   -- jsnes renderer: affine ground flight over a
 *                      rings texture
 *   5  YUV canvas    -- the NES scene rendered FULL-COLOR on the video
 *                      plane via the jfb tiled-YUV canvas (the
 *                      cedar_nes-analogue pipeline; OSD is closed while
 *                      this scene owns the video layer)
 *
 * All assets are generated procedurally at entry -- no bitmap resources,
 * no ROMs. The scenes exercise every ported subsystem: jnes, jgb, jgen,
 * jsnes, jdraw, jaudio, jrgb2yuv, jfb/jcodec and the jshim layer.
 */
#include "Winav.h"
#include "gdi.h"
#include "disp.h"
#include "input.h"

#include "jshim.h"
#include "jrgb2yuv.h"
#include "jdraw.h"
#include "jnes.h"
#include "jgb.h"
#include "jgen.h"
#include "jsnes.h"
#include "jaudio.h"
#include "jfb.h"
#include "jcodec.h"
#include "jgpu.h"
#include "japp.h"

/* Use the 2D engine for OSD fills (color bars, scene clears). The
 * blitter path mirrors gdi.c's own programming; set to 0 to fall back
 * to CPU drawing if hardware bring-up misbehaves. */
#define JAPP_USE_GPU 1

extern BYTE __bKey;

/* ---- OSD palette layout while the Jupiter app owns the screen ----
 * [JPAL_NES_BASE .. +63]  NES master palette (color bars + NES scene;
 *                         the Mode 7 scene reuses this range too)
 * [JPAL_GEN_BASE .. +63]  Genesis CRAM (4 palettes x 16)
 * [JPAL_GB_BASE  .. +7]   GB demo colors (4 BG + 4 sprite)
 * Chosen above the firmware's reserved UI entries (0..154 are the GDI
 * UI ranges; the app owns the screen, but staying high keeps the UI
 * palette intact for instant restore on exit). */
#define JPAL_NES_BASE  64
#define JPAL_GEN_BASE  128
#define JPAL_GB_BASE   192

#define JAPP_SCENES    6
#define JAPP_FRAME_MS  33   /* ~30 fps target; degrades gracefully */
#define JAPP_SCENE_CANVAS 5 /* video-plane scene: OSD closed while active */

static BYTE _bJupActive = 0;
static BYTE _bScene = 0;
static BYTE _bSceneDirty = 0;
static uint32_t _dwLastFrameMs = 0;

/* ---- NES demo state ---- */
static uint8_t nes_chr[16 * 4];        /* 4 BG tiles, 2bpp */
static uint8_t nes_spr_chr[16];        /* 1 sprite tile (ball) */
static uint8_t nes_nt[NES_NT_TILES];
static uint8_t nes_attr[NES_NT_ATTRS];
static uint8_t nes_pal_ram[32];
static nes_bg_t nes_bg;
static nes_oam_entry_t nes_oam[4];
static int16_t nes_ball_x[4], nes_ball_y[4], nes_ball_dx[4], nes_ball_dy[4];

/* ---- GB demo state ---- */
static uint8_t gb_chr[16 * 3];         /* 3 BG tiles */
static uint8_t gb_spr_chr[16];         /* 1 sprite tile */
static uint8_t gb_map[GB_MAP_SIZE];
static uint8_t gb_pal_idx[32];         /* BG palette indices */
static uint8_t gb_spr_pal_idx[32];     /* sprite palette indices */
static gb_bg_t gb_bg;
static gb_oam_entry_t gb_oam[2];

/* ---- Genesis demo state ---- */
static uint8_t gen_tiles[32 * 5];      /* 4bpp tiles */
static uint16_t gen_map_a[32 * 32];
static uint16_t gen_map_b[32 * 32];
static uint16_t gen_win_map[16 * 4];
static uint8_t gen_cram_idx[64];
static genesis_plane_t gen_pa, gen_pb;
static genesis_window_t gen_win;
static genesis_sprite_t gen_spr[2];

/* ---- SNES Mode 7 demo state ---- */
static uint8_t m7_map[64 * 64];
static uint8_t m7_pal_idx[256];
static snes_mode7_t m7_cfg;

/* ---- YUV canvas scene state ---- */
static jfb_t _canvas;
static uint32_t canvas_pal_yuv[256];  /* OSD index -> 0x00YYUUVV */
static BYTE _bCanvasActive = 0;

/* Set one pixel of a 2bpp tile (shared CHR helper) */
static void chr_set(uint8_t *tile, int row, int col, uint8_t ci)
{
    uint8_t mask = (uint8_t)(0x80 >> col);
    if (ci & 1) tile[row] |= mask; else tile[row] &= (uint8_t)~mask;
    if (ci & 2) tile[row + 8] |= mask; else tile[row + 8] &= (uint8_t)~mask;
}

static void build_nes_assets(void)
{
    int t, r, c, i;

    /* Tile 0: blank. Tile 1: brick. Tile 2: checker. Tile 3: dots. */
    for (i = 0; i < (int)sizeof(nes_chr); i++) nes_chr[i] = 0;
    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            uint8_t ci;
            /* brick: mortar lines at row 0/4 and staggered columns */
            ci = (uint8_t)((r == 0 || r == 4 || c == (r < 4 ? 0 : 4)) ? 3 : 1);
            chr_set(&nes_chr[16 * 1], r, c, ci);
            /* checker 4x4 */
            ci = (uint8_t)((((r >> 2) ^ (c >> 2)) & 1) ? 2 : 1);
            chr_set(&nes_chr[16 * 2], r, c, ci);
            /* dots */
            ci = (uint8_t)(((r & 3) == 1 && (c & 3) == 1) ? 3 : 0);
            chr_set(&nes_chr[16 * 3], r, c, ci);
        }
    }

    /* Ball sprite: filled circle, highlight at top-left */
    for (i = 0; i < 16; i++) nes_spr_chr[i] = 0;
    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            int dx = 2 * c - 7, dy = 2 * r - 7;
            if (dx * dx + dy * dy <= 49) {
                uint8_t ci = (uint8_t)((r <= 2 && c <= 3) ? 3 : 1);
                chr_set(nes_spr_chr, r, c, ci);
            }
        }
    }

    /* Nametable: brick ground rows, checker band, dotted sky */
    for (r = 0; r < NES_NT_H; r++)
        for (c = 0; c < NES_NT_W; c++)
            nes_nt[r * NES_NT_W + c] =
                (uint8_t)((r >= 24) ? 1 : (r >= 20 ? 2 : 3));

    /* Attributes: sky uses palette 1, band palette 2, ground palette 0 */
    for (i = 0; i < NES_NT_ATTRS; i++) {
        int aty = (i / 8) * 4;
        uint8_t p = (uint8_t)((aty >= 24) ? 0 : (aty >= 20 ? 2 : 1));
        nes_attr[i] = NES_ATTR(p, p, p, p);
    }

    for (i = 0; i < 32; i++) nes_pal_ram[i] = 0x0F;      /* black */
    nes_pal_ram[0] = 0x0F;                                /* backdrop */
    nes_pal_ram[1] = 0x16; nes_pal_ram[2] = 0x27; nes_pal_ram[3] = 0x30;
    nes_pal_ram[5] = 0x12; nes_pal_ram[6] = 0x2C; nes_pal_ram[7] = 0x30;
    nes_pal_ram[9] = 0x1A; nes_pal_ram[10] = 0x2A; nes_pal_ram[11] = 0x30;
    nes_pal_ram[17] = 0x28; nes_pal_ram[18] = 0x16; nes_pal_ram[19] = 0x30;

    nes_bg.chr = nes_chr;
    nes_bg.nametable = nes_nt;
    nes_bg.attribute = nes_attr;
    nes_bg.palette_ram = nes_pal_ram;
    nes_bg.scroll_x = 0;
    nes_bg.scroll_y = 0;
    nes_bg.line_scroll_x = NULL;
    nes_bg.enabled = 1;

    for (i = 0; i < 4; i++) {
        nes_ball_x[i] = (int16_t)(40 + i * 48);
        nes_ball_y[i] = (int16_t)(30 + i * 25);
        nes_ball_dx[i] = (int16_t)((i & 1) ? 2 : -2);
        nes_ball_dy[i] = (int16_t)((i & 2) ? 1 : -1);
        nes_oam[i].tile = 0;
        nes_oam[i].attr = 0;
    }
}

static void build_gb_assets(void)
{
    int r, c, i;
    static const uint32_t gb_bg_argb[4] = {
        0xFF9BBC0F, 0xFF8BAC0F, 0xFF306230, 0xFF0F380F  /* DMG greens */
    };
    static const uint32_t gb_spr_argb[4] = {
        0xFF9BBC0F, 0xFF0F380F, 0xFF306230, 0xFF9BBC0F
    };

    for (i = 0; i < (int)sizeof(gb_chr); i++) gb_chr[i] = 0;
    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            uint8_t ci;
            /* tile 1: diamond */
            int dx = (c < 4) ? (3 - c) : (c - 4);
            int dy = (r < 4) ? (3 - r) : (r - 4);
            ci = (uint8_t)((dx + dy <= 3) ? 2 : 1);
            chr_set(&gb_chr[16 * 1], r, c, ci);
            /* tile 2: frame */
            ci = (uint8_t)((r == 0 || r == 7 || c == 0 || c == 7) ? 3 : 0);
            chr_set(&gb_chr[16 * 2], r, c, ci);
        }
    }

    /* Sprite: smiley */
    for (i = 0; i < 16; i++) gb_spr_chr[i] = 0;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            int dx = 2 * c - 7, dy = 2 * r - 7;
            uint8_t ci = 0;
            if (dx * dx + dy * dy <= 49) ci = 1;
            if (r == 2 && (c == 2 || c == 5)) ci = 3;
            if (r == 5 && c >= 2 && c <= 5) ci = 3;
            if (ci) chr_set(gb_spr_chr, r, c, ci);
        }

    for (r = 0; r < GB_MAP_H; r++)
        for (c = 0; c < GB_MAP_W; c++)
            gb_map[r * GB_MAP_W + c] =
                (uint8_t)((((r ^ c) & 3) == 0) ? 2 : (((r + c) & 1) ? 1 : 0));

    /* Load GB colors into OSD palette: 4 BG at GB_BASE, 4 sprite at +4 */
    jvid_load_palette(JPAL_GB_BASE, gb_bg_argb, 4);
    jvid_load_palette(JPAL_GB_BASE + 4, gb_spr_argb, 4);

    for (i = 0; i < 32; i++) {
        gb_pal_idx[i] = (uint8_t)(JPAL_GB_BASE + (i & 3));
        gb_spr_pal_idx[i] = (uint8_t)(JPAL_GB_BASE + 4 + (i & 3));
    }

    gb_bg.chr = gb_chr;
    gb_bg.map = gb_map;
    gb_bg.map_attr = NULL;
    gb_bg.palette = gb_pal_idx;
    gb_bg.scroll_x = 0;
    gb_bg.scroll_y = 0;
    gb_bg.enabled = 1;

    gb_oam[0].tile = 0; gb_oam[0].attr = 0;
    gb_oam[1].tile = 0; gb_oam[1].attr = GB_SPR_HFLIP;
}

/* 4bpp tile builder (Genesis packed format: 2 px/byte, high nibble
 * first) */
static void gen_tile_set(uint8_t *tile, int row, int col, uint8_t ci)
{
    uint8_t *b = &tile[row * 4 + (col >> 1)];
    if (col & 1) *b = (uint8_t)((*b & 0xF0) | (ci & 0x0F));
    else         *b = (uint8_t)((*b & 0x0F) | (uint8_t)(ci << 4));
}

static void build_gen_assets(void)
{
    int r, c, i;
    uint32_t cram_argb[64];

    /* Tiles: 1 solid block w/ border, 2 checker, 3 hill, 4 ring */
    for (i = 0; i < (int)sizeof(gen_tiles); i++) gen_tiles[i] = 0;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            gen_tile_set(&gen_tiles[32 * 1], r, c,
                         (uint8_t)((r == 0 || c == 0) ? 12
                                   : ((r == 7 || c == 7) ? 2 : 7)));
            gen_tile_set(&gen_tiles[32 * 2], r, c,
                         (uint8_t)((((r >> 2) ^ (c >> 2)) & 1) ? 5 : 3));
            gen_tile_set(&gen_tiles[32 * 3], r, c,
                         (uint8_t)((r >= (7 - c)) ? 9 : 0));
            gen_tile_set(&gen_tiles[32 * 4], r, c,
                         (uint8_t)((r == 0 || r == 7 || c == 0 || c == 7)
                                   ? 14 : 0));
        }

    /* Plane B: checker field with hill strips. Plane A: sparse rings. */
    for (r = 0; r < 32; r++)
        for (c = 0; c < 32; c++) {
            gen_map_b[r * 32 + c] = (uint16_t)GEN_ENTRY(
                (r > 24) ? 3 : 2, (r > 24) ? 1 : 0, 0, 0);
            gen_map_a[r * 32 + c] = (uint16_t)(((r * 7 + c * 3) % 13 == 0)
                ? GEN_ENTRY(4, 2, c & 1, r & 1) : 0);
        }

    /* Window: HUD strip of bordered blocks */
    for (i = 0; i < 16 * 4; i++)
        gen_win_map[i] = (uint16_t)GEN_ENTRY(1, 3, 0, 0);

    /* CRAM: 4 palettes of 16 shades (blue, green, orange, gray) */
    for (i = 0; i < 16; i++) {
        uint32_t v = (uint32_t)(i * 16 + 15);
        cram_argb[i]      = 0xFF000000u | (v >> 2 << 16) | (v >> 1 << 8) | v;
        cram_argb[16 + i] = 0xFF000000u | (v >> 2 << 16) | (v << 8) | (v >> 2);
        cram_argb[32 + i] = 0xFF000000u | (v << 16) | (v >> 1 << 8) | (v >> 3);
        cram_argb[48 + i] = 0xFF000000u | (v << 16) | (v << 8) | v;
    }
    jvid_load_palette(JPAL_GEN_BASE, cram_argb, 64);
    for (i = 0; i < 64; i++)
        gen_cram_idx[i] = (uint8_t)(JPAL_GEN_BASE + i);

    gen_pa.tiles = gen_tiles; gen_pa.map = gen_map_a;
    gen_pa.cram = gen_cram_idx;
    gen_pa.scroll_x = 0; gen_pa.scroll_y = 0; gen_pa.line_hscroll = NULL;
    gen_pa.map_w = 32; gen_pa.map_h = 32; gen_pa.enabled = 1;

    gen_pb.tiles = gen_tiles; gen_pb.map = gen_map_b;
    gen_pb.cram = gen_cram_idx;
    gen_pb.scroll_x = 0; gen_pb.scroll_y = 0; gen_pb.line_hscroll = NULL;
    gen_pb.map_w = 32; gen_pb.map_h = 32; gen_pb.enabled = 1;

    gen_win.map = gen_win_map; gen_win.x = 8; gen_win.y = 8;
    gen_win.w = 128; gen_win.h = 16; gen_win.map_w = 16; gen_win.map_h = 4;

    for (i = 0; i < 2; i++) {
        gen_spr[i].tile = 1; gen_spr[i].w = 2; gen_spr[i].h = 2;
        gen_spr[i].pal = (uint8_t)(i + 1);
        gen_spr[i].fliph = 0; gen_spr[i].flipv = 0;
        gen_spr[i].enabled = 1;
    }
}

static void build_m7_assets(void)
{
    int r, c, i;

    /* Rings texture over a checker floor */
    for (r = 0; r < 64; r++)
        for (c = 0; c < 64; c++) {
            int dx = c - 32, dy = r - 32;
            int ring = ((dx * dx + dy * dy) >> 5) & 15;
            int check = (((r >> 3) ^ (c >> 3)) & 1) ? 2 : 0;
            m7_map[r * 64 + c] = (uint8_t)((ring + check) & 15);
        }

    /* Texture values 0..15 -> a warm ramp inside the NES master range */
    {
        static const uint8_t ramp[16] = {
            0x0F, 0x07, 0x17, 0x27, 0x37, 0x28, 0x38, 0x18,
            0x08, 0x09, 0x19, 0x29, 0x39, 0x2A, 0x1A, 0x0A
        };
        for (i = 0; i < 256; i++)
            m7_pal_idx[i] = (uint8_t)(JPAL_NES_BASE + ramp[i & 15]);
    }

    m7_cfg.cam_x = 0; m7_cfg.cam_y = 0;
    m7_cfg.angle = 0; m7_cfg.twist = 1;
    m7_cfg.horizon = 60; m7_cfg.space_z = 8000;
    m7_cfg.map = m7_map; m7_cfg.palette = m7_pal_idx;
    m7_cfg.map_w_bits = 6; m7_cfg.map_mask = 63;
}

/* Entry jingle: APU square arpeggio + noise hit, rendered offline into
 * DRAM scratch and submitted through the HAL raw-PCM path. */
static void play_jingle(void)
{
    uint32_t cap, chunk, i;
    int16_t *buf = jsnd_scratch(&cap);
    static const uint32_t notes[4] = {523, 659, 784, 1047}; /* C5 E5 G5 C6 */

    if (buf == NULL || cap == 0)
        return;

    chunk = JAUDIO_RATE / 10;             /* 100 ms per note */
    if (chunk * 4 > cap) chunk = cap / 4;

    jaudio_init();
    jaudio_apu_noise_on_env(10, 4, 0, -1, 2);
    for (i = 0; i < 4; i++) {
        jaudio_apu_note_on_env(0, notes[i], 12, 2, -1, 6);
        jaudio_render(buf + i * chunk, chunk);
        jaudio_apu_noise_off();
    }
    jaudio_apu_all_off();

    jsnd_play(buf, chunk * 4);
}

/* ---- Scene rendering ---- */

static void scene_colorbars(void)
{
    /* 8 SMPTE-order bars from NES master colors:
     * white, yellow, cyan, green, magenta, red, blue, black.
     * Drawn by the 2D engine when JAPP_USE_GPU -- this scene doubles
     * as the blitter's hardware bring-up test. */
    static const uint8_t bar_color[8] = {
        0x30, 0x28, 0x2C, 0x2A, 0x24, 0x16, 0x12, 0x0F
    };
    uint8_t *fb = jvid_fb();
    int i;
    int barw = JVID_W / 8;

    for (i = 0; i < 8; i++) {
        int w = (i == 7) ? (JVID_W - 7 * barw) : barw;
        uint8_t c = (uint8_t)(JPAL_NES_BASE + bar_color[i]);
#if JAPP_USE_GPU
        if (jgpu_fill((uint32_t)fb, JVID_PITCH,
                      (uint32_t)(i * barw), 0, (uint32_t)w, JVID_H,
                      c, JGPU_F_HP | JGPU_F_BURST_MAX) == 0)
            continue;
#endif
        jdraw_rect(fb, JVID_PITCH, i * barw, 0, w, JVID_H, c);
    }
}

static void scene_nes_frame(void)
{
    uint8_t *fb = jvid_fb();
    int i;

    nes_bg.scroll_x = (int16_t)(nes_bg.scroll_x + 1);

    for (i = 0; i < 4; i++) {
        nes_ball_x[i] = (int16_t)(nes_ball_x[i] + nes_ball_dx[i]);
        nes_ball_y[i] = (int16_t)(nes_ball_y[i] + nes_ball_dy[i]);
        if (nes_ball_x[i] <= 0 || nes_ball_x[i] >= NES_NATIVE_W - 8)
            nes_ball_dx[i] = (int16_t)-nes_ball_dx[i];
        if (nes_ball_y[i] <= 0 || nes_ball_y[i] >= NES_NATIVE_H - 9)
            nes_ball_dy[i] = (int16_t)-nes_ball_dy[i];
        nes_oam[i].x = (uint8_t)nes_ball_x[i];
        nes_oam[i].y = (uint8_t)(nes_ball_y[i] - 1);
    }

    nes_render8(fb, JVID_PITCH,
                (JVID_W - NES_NATIVE_W) / 2, (JVID_H - NES_NATIVE_H) / 2,
                JPAL_NES_BASE, &nes_bg, nes_spr_chr, nes_oam, 4);
}

static void scene_gb_frame(void)
{
    uint8_t *fb = jvid_fb();
    uint32_t t = jtime_ms() / 64;

    gb_bg.scroll_x = (int16_t)(gb_bg.scroll_x + 1);
    gb_bg.scroll_y = (int16_t)(t & 0xFF);

    gb_oam[0].x = (uint8_t)(8 + 40 + ((t * 3) % (GB_NATIVE_W - 48)));
    gb_oam[0].y = (uint8_t)(16 + 60);
    gb_oam[1].x = (uint8_t)(8 + GB_NATIVE_W - 48 - ((t * 3) % (GB_NATIVE_W - 48)));
    gb_oam[1].y = (uint8_t)(16 + 90);

    gb_render8(fb, JVID_PITCH,
               (JVID_W - GB_NATIVE_W) / 2, (JVID_H - GB_NATIVE_H) / 2,
               &gb_bg, gb_spr_chr, gb_spr_pal_idx, gb_oam, 2);
}

static void scene_gen_frame(void)
{
    uint8_t *fb = jvid_fb();
    uint32_t t = jtime_ms() / 33;

    /* Parallax: Plane A scrolls 2x faster than Plane B */
    gen_pb.scroll_x = (int32_t)(t & 0x7FFFFFFF);
    gen_pa.scroll_x = (int32_t)((t * 2) & 0x7FFFFFFF);

    gen_spr[0].x = (int16_t)(40 + ((t * 2) % (GEN_NATIVE_W - 56)));
    gen_spr[0].y = 100;
    gen_spr[1].x = (int16_t)(GEN_NATIVE_W - 56 -
                             ((t * 3) % (GEN_NATIVE_W - 56)));
    gen_spr[1].y = 150;

    genesis_render8(fb, JVID_PITCH,
                    (JVID_W - GEN_NATIVE_W) / 2, (JVID_H - GEN_NATIVE_H) / 2,
                    (uint8_t)(JPAL_NES_BASE + 0x0F),
                    &gen_pa, &gen_pb, &gen_win, gen_spr, 2);
}

static void scene_m7_frame(void)
{
    uint8_t *fb = jvid_fb();
    uint32_t x0 = (JVID_W - SNES_NATIVE_W) / 2;
    uint32_t y0 = (JVID_H - SNES_NATIVE_H) / 2;
    const int32_t *cos_lut = snes_cos_lut();
    const int32_t *sin_lut = snes_sin_lut();

    /* Fly forward along the current heading, slowly turning */
    m7_cfg.angle = (uint8_t)(m7_cfg.angle + 1);
    m7_cfg.cam_x += cos_lut[m7_cfg.angle] >> 9;
    m7_cfg.cam_y += sin_lut[m7_cfg.angle] >> 9;

    /* Sky above the horizon (NES light blue) */
    jdraw_rect(fb, JVID_PITCH, (int)x0, (int)y0,
               SNES_NATIVE_W, m7_cfg.horizon,
               (uint8_t)(JPAL_NES_BASE + 0x21));

    snes_mode7_render8(fb, JVID_PITCH, x0, y0, &m7_cfg);
}

/* Enter the video-plane canvas scene: the OSD is closed (it would
 * otherwise cover the video, and in the 2 MB DRAM map the OSD buffer
 * overlaps the frame-buffer region), the canvas bound to hardware
 * frame 0, and the NES master palette converted once to YUV. */
static void canvas_enter(void)
{
    int i;

    jvid_close();
    if (jcodec_canvas_open(&_canvas, 0) != 0)
        return;

    for (i = 0; i < 256; i++)
        canvas_pal_yuv[i] = 0x00108080;   /* black */
    for (i = 0; i < 64; i++)
        canvas_pal_yuv[JPAL_NES_BASE + i] =
            jup_argb_to_yuv(jup_nes_master_palette[i]);

    jfb_fill(&_canvas, 0, 0, _canvas.w, _canvas.h, 0x00108080);
    jcodec_canvas_show(0);
    _bCanvasActive = 1;
}

/* Leave the canvas scene: hide the video plane and restore the OSD
 * (region + palettes), since jvid_close() dropped it. */
static void canvas_leave(void)
{
    if (!_bCanvasActive)
        return;
    _bCanvasActive = 0;
    jcodec_canvas_hide();
    jvid_open(0);
    jvid_load_palette(JPAL_NES_BASE, jup_nes_master_palette, 64);
    build_gb_assets();    /* reloads the GB palette entries */
    build_gen_assets();   /* reloads the Genesis CRAM entries */
}

static void scene_canvas_frame(void)
{
    /* Reuse the NES scene's world state, but rasterize into a linear
     * indexed scratch and blit it full-color onto the video plane.
     * Scratch: hardware frame 1's memory -- unused while the canvas
     * displays frame 0 only. */
    uint8_t *scratch = (uint8_t *)__DISPFrameInfo[1].dwFY_Addr;
    int i;

    nes_bg.scroll_x = (int16_t)(nes_bg.scroll_x + 1);
    for (i = 0; i < 4; i++) {
        nes_ball_x[i] = (int16_t)(nes_ball_x[i] + nes_ball_dx[i]);
        nes_ball_y[i] = (int16_t)(nes_ball_y[i] + nes_ball_dy[i]);
        if (nes_ball_x[i] <= 0 || nes_ball_x[i] >= NES_NATIVE_W - 8)
            nes_ball_dx[i] = (int16_t)-nes_ball_dx[i];
        if (nes_ball_y[i] <= 0 || nes_ball_y[i] >= NES_NATIVE_H - 9)
            nes_ball_dy[i] = (int16_t)-nes_ball_dy[i];
        nes_oam[i].x = (uint8_t)nes_ball_x[i];
        nes_oam[i].y = (uint8_t)(nes_ball_y[i] - 1);
    }

    nes_render8(scratch, NES_NATIVE_W, 0, 0, JPAL_NES_BASE,
                &nes_bg, nes_spr_chr, nes_oam, 4);

    jfb_blit_indexed(&_canvas,
                     (_canvas.w - NES_NATIVE_W) / 2 & ~1u,
                     (_canvas.h - NES_NATIVE_H) / 2 & ~1u,
                     scratch, NES_NATIVE_W, NES_NATIVE_H,
                     NES_NATIVE_W, canvas_pal_yuv);
}

static void scene_enter(void)
{
    if (_bScene == JAPP_SCENE_CANVAS) {
        canvas_enter();
        _bSceneDirty = 1;
        play_jingle();
        return;
    }
    canvas_leave();

    /* Frame the scene area with the NES black; scenes draw inside */
#if JAPP_USE_GPU
    if (jgpu_fill((uint32_t)jvid_fb(), JVID_PITCH, 0, 0, JVID_W, JVID_H,
                  (uint8_t)(JPAL_NES_BASE + 0x0F),
                  JGPU_F_HP | JGPU_F_BURST_MAX) != 0)
#endif
        jdraw_clear(jvid_fb(), JVID_PITCH, JVID_W, JVID_H,
                    (uint8_t)(JPAL_NES_BASE + 0x0F));
    _bSceneDirty = 1;
    play_jingle();
}

/* ---- App lifecycle ---- */

static void jupiter_enter(void)
{
    jvid_open(0);   /* index 0 = transparent while palette loads */
    jvid_load_palette(JPAL_NES_BASE, jup_nes_master_palette, 64);
    build_nes_assets();
    build_gb_assets();
    build_gen_assets();
    build_m7_assets();
    _bScene = 0;
    _bJupActive = 1;
    _dwLastFrameMs = 0;
    scene_enter();
}

static void jupiter_exit(void)
{
    jaudio_apu_all_off();
    jaudio_genesis_all_off();
    if (_bCanvasActive) {
        _bCanvasActive = 0;
        jcodec_canvas_hide();
    }
    jvid_close();
    _bJupActive = 0;
}

BYTE JUPITER_IsActive(void)
{
    return _bJupActive;
}

BYTE JUPITER_ProcessKey(void)
{
    if (!_bJupActive) {
        if (__bKey == KEY_OSDGAME) {
            jupiter_enter();
            return KEY_NO_KEY;
        }
        return KEY_BYPASS;
    }

    switch (__bKey) {
    case KEY_OSDGAME:
    case KEY_STOP:
        jupiter_exit();
        break;
    case KEY_LEFT:
        _bScene = (BYTE)((_bScene + JAPP_SCENES - 1) % JAPP_SCENES);
        scene_enter();
        break;
    case KEY_RIGHT:
        _bScene = (BYTE)((_bScene + 1) % JAPP_SCENES);
        scene_enter();
        break;
    case KEY_ENTER:
    case KEY_PLAY:
        play_jingle();
        break;
    default:
        /* Everything else is consumed while the app is active; scene
         * logic reads what it needs via jinp_map_key. */
        break;
    }
    return KEY_NO_KEY;
}

void JUPITER_Trigger(void)
{
    uint32_t now;

    if (!_bJupActive)
        return;

    now = jtime_ms();
    if (!_bSceneDirty && (now - _dwLastFrameMs) < JAPP_FRAME_MS)
        return;
    _dwLastFrameMs = now;

    switch (_bScene) {
    case 0:
        if (_bSceneDirty)
            scene_colorbars();   /* static -- draw once */
        break;
    case 1:
        scene_nes_frame();
        break;
    case 2:
        scene_gb_frame();
        break;
    case 3:
        scene_gen_frame();
        break;
    case 4:
        scene_m7_frame();
        break;
    case JAPP_SCENE_CANVAS:
        if (_bCanvasActive)
            scene_canvas_frame();
        break;
    default:
        break;
    }
    _bSceneDirty = 0;
}
