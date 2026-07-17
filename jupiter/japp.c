/*
 * JupiterSDK on CT952 -- demo application.
 *
 * Three scenes, switched with LEFT/RIGHT:
 *   0  color bars   -- palette / region bring-up check (static image)
 *   1  NES demo     -- jnes renderer: scrolling attributed background +
 *                     bouncing OAM sprites, APU jingle on entry
 *   2  Game Boy demo -- jgb renderer: DMG-green scrolling background +
 *                     sprites
 *
 * All assets are generated procedurally at entry -- no bitmap resources,
 * no ROMs. The scenes exercise every ported subsystem: jnes, jgb,
 * jdraw, jaudio, jrgb2yuv and the jshim platform layer.
 */
#include "Winav.h"
#include "gdi.h"
#include "input.h"

#include "jshim.h"
#include "jdraw.h"
#include "jnes.h"
#include "jgb.h"
#include "jaudio.h"
#include "japp.h"

extern BYTE __bKey;

/* ---- OSD palette layout while the Jupiter app owns the screen ----
 * [JPAL_NES_BASE .. +63]  NES master palette (also used by color bars)
 * [JPAL_GB_BASE  .. +7]   GB demo colors (4 BG + 4 sprite)
 * Chosen above the firmware's reserved UI entries (0..154 are the GDI
 * UI ranges; the app owns the screen, but staying high keeps the UI
 * palette intact for instant restore on exit). */
#define JPAL_NES_BASE  64
#define JPAL_GB_BASE   192

#define JAPP_SCENES    3
#define JAPP_FRAME_MS  33   /* ~30 fps target; degrades gracefully */

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
     * white, yellow, cyan, green, magenta, red, blue, black */
    static const uint8_t bar_color[8] = {
        0x30, 0x28, 0x2C, 0x2A, 0x24, 0x16, 0x12, 0x0F
    };
    uint8_t *fb = jvid_fb();
    int i;
    int barw = JVID_W / 8;

    for (i = 0; i < 8; i++)
        jdraw_rect(fb, JVID_PITCH, i * barw, 0,
                   (i == 7) ? (JVID_W - 7 * barw) : barw, JVID_H,
                   (uint8_t)(JPAL_NES_BASE + bar_color[i]));
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

static void scene_enter(void)
{
    /* Frame the scene area with the NES black; scenes draw inside */
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
    _bScene = 0;
    _bJupActive = 1;
    _dwLastFrameMs = 0;
    scene_enter();
}

static void jupiter_exit(void)
{
    jaudio_apu_all_off();
    jaudio_genesis_all_off();
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
    default:
        break;
    }
    _bSceneDirty = 0;
}
