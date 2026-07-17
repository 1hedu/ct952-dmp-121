/*
 * ct952emu -- animated Mode 7, executed on the emulated CT909/CT952.
 *
 * Renders ANIM_FRAMES frames of the jsnes Mode 7 affine ground on the
 * emulated SPARC CPU, sweeping the camera yaw so the racetrack spins,
 * writing each frame's 8bpp OSD plane to a successive DRAM slot. The
 * host driver (anim_run) reads the frames + the GAM_OSD palette back out
 * and assembles an animated GIF. Same machine model that boots the stock
 * ROM; no firmware, no libc.
 */
#include "jup_types.h"
#include "demo_de.h"
#include "jsnes.h"

#define AW   256
#define AH   224
#define ANIM_FRAMES 24
#define ANIM_BASE   0x40300000u          /* frame 0 base */
#define ANIM_STRIDE (AW * AH)            /* bytes between frames */

static uint32_t g_pal[256];
static uint32_t lerp_rgb(uint32_t c0, uint32_t c1, int t, int tmax)
{
    uint32_t r = ((c0>>16&0xFF)*(uint32_t)(tmax-t)+(c1>>16&0xFF)*(uint32_t)t)/(uint32_t)tmax;
    uint32_t g = ((c0>>8&0xFF)*(uint32_t)(tmax-t)+(c1>>8&0xFF)*(uint32_t)t)/(uint32_t)tmax;
    uint32_t b = ((c0&0xFF)*(uint32_t)(tmax-t)+(c1&0xFF)*(uint32_t)t)/(uint32_t)tmax;
    return 0xFF000000u | (r<<16) | (g<<8) | b;
}
static void build_palette(void)
{
    int i;
    for (i = 0; i < 256; i++) g_pal[i] = 0xFF000000u;
    g_pal[16]=0xFF3A4048u; g_pal[17]=0xFF2C3238u;   /* asphalt */
    g_pal[18]=0xFFB03030u; g_pal[19]=0xFFE8E8E8u;   /* kerb */
    g_pal[20]=0xFF20A040u; g_pal[21]=0xFF186030u;   /* grass */
    for (i = 0; i < 32; i++)                          /* horizon sky */
        g_pal[96 + i] = lerp_rgb(0x1A2A55, 0xF0C0A0, i, 31);
}

#define M7_W 128
static uint8_t m7_map[M7_W * M7_W];
static uint8_t m7_pal[256];
static void build_track(void)
{
    int x, y, i;
    for (y = 0; y < M7_W; y++)
        for (x = 0; x < M7_W; x++) {
            uint8_t v;
            int cx = x - 64, cy = y - 64, rr = cx*cx + cy*cy;
            if (rr > 60*60)      v = (uint8_t)(((x>>3)^(y>>3))&1 ? 20 : 21);
            else if (rr > 54*54) v = (uint8_t)(((x>>2)+(y>>2))&1 ? 18 : 19);
            else {
                v = (uint8_t)(((x>>4)^(y>>4))&1 ? 16 : 17);
                if (((x & 31) == 0) || ((y & 31) == 0)) v = 19;
            }
            m7_map[y*M7_W + x] = v;
        }
    for (i = 0; i < 256; i++) m7_pal[i] = (uint8_t)i;
}

unsigned testmain(void)
{
    snes_mode7_t m7;
    int f, y, i;

    build_palette();
    build_track();
    m7.cam_x = 200; m7.cam_y = 130; m7.twist = 0;
    m7.horizon = 74; m7.space_z = 7200;
    m7.map = m7_map; m7.palette = m7_pal;
    m7.map_w_bits = 7; m7.map_mask = M7_W - 1;

    for (f = 0; f < ANIM_FRAMES; f++) {
        uint8_t *fb = (uint8_t *)(ANIM_BASE + (uint32_t)f * ANIM_STRIDE);
        /* paint the sky band, then the affine ground for this yaw */
        for (y = 0; y < (int)m7.horizon; y++) {
            uint8_t sky = (uint8_t)(96 + (y * 31 / (m7.horizon - 1)));
            for (i = 0; i < AW; i++) fb[y*AW + i] = sky;
        }
        m7.angle = (uint8_t)(f * (256 / ANIM_FRAMES));
        /* also drift the camera forward for a sense of travel */
        m7.cam_y = 130 + f * 6;
        /* renders the full 256x224 rect at (0,0); rows above `horizon`
         * are left untouched, so our pre-painted sky shows through */
        snes_mode7_render8(fb, AW, 0, 0, &m7);
    }

    /* publish palette to the display engine (host reads GAM_OSD) */
    de_program(ANIM_BASE, AW, AH, g_pal);
    return ANIM_BASE;
}
