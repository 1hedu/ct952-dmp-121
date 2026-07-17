/*
 * JupiterSDK on CT952 -- SNES-style renderer, indexed-8bpp port of
 * Jupiter SDK lib/snes.c.
 *
 * Tile-column compositor: per scanline, cache tilemap entries for all
 * BGs at each tile column, then resolve 8 pixels from cached entries.
 * Single-buffer layering: bottom BG opaque first, then upper BGs
 * front-to-back per pixel (first opaque wins; nothing opaque = leave
 * the bottom BG pixel).
 */
#include "jsnes.h"

/* Pre-resolved tile column */
typedef struct {
    const uint8_t *trow;
    uint32_t pal_off;
    uint16_t tidx;          /* tile index (0=transparent) */
    uint8_t  flipx;
    uint8_t  bpp;
} tcache_t;

static void cache_tile(tcache_t *tc, const snes_bg_t *bg,
                       uint32_t tile_col, uint32_t fine_y)
{
    uint16_t entry;
    uint32_t flipy, fy, trb, tsz, cpp;

    if (!bg || !bg->enabled) { tc->tidx = 0; return; }

    entry = bg->map[((fine_y >> 3) % bg->map_h) * bg->map_w + tile_col];
    tc->tidx  = SNES_GET_TILE(entry);
    tc->flipx = (uint8_t)SNES_GET_FLIPX(entry);
    tc->bpp   = bg->bpp;

    if (tc->tidx == 0) return;

    flipy = SNES_GET_FLIPY(entry);
    fy = flipy ? (7 - (fine_y & 7)) : (fine_y & 7);

    if (bg->bpp == 8)      { trb = 8; tsz = 64; cpp = 256; }
    else if (bg->bpp == 4) { trb = 4; tsz = 32; cpp = 16;  }
    else                   { trb = 2; tsz = 16; cpp = 4;   }

    tc->trow = bg->tiles + tc->tidx * tsz + fy * trb;
    tc->pal_off = (uint32_t)SNES_GET_PAL(entry) * cpp;
}

static uint8_t tc_pixel(const tcache_t *tc, uint32_t fine_x)
{
    uint32_t fx;
    uint8_t b;

    if (tc->tidx == 0) return 0;
    fx = tc->flipx ? (7 - fine_x) : fine_x;

    if (tc->bpp == 8) return tc->trow[fx];
    if (tc->bpp == 4) {
        b = tc->trow[fx >> 1];
        return (fx & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
    }
    b = tc->trow[fx >> 2];
    return (uint8_t)((b >> ((3 - (fx & 3)) * 2)) & 3);
}

/* ================================================================
 *  Bottom BG: opaque with backdrop
 * ================================================================ */
static void render_bg_opaque(uint8_t *fb, uint32_t pitch,
                             uint32_t x0, uint32_t y0,
                             const snes_bg_t *bg, uint8_t backdrop)
{
    uint32_t lpy, lpx;

    if (!bg || !bg->enabled) {
        for (lpy = 0; lpy < SNES_NATIVE_H; lpy++) {
            uint8_t *row = fb + (y0 + lpy) * pitch + x0;
            for (lpx = 0; lpx < SNES_NATIVE_W; lpx++) row[lpx] = backdrop;
        }
        return;
    }

    for (lpy = 0; lpy < SNES_NATIVE_H; lpy++) {
        uint32_t wy = (uint32_t)((int32_t)lpy + bg->scroll_y);
        uint8_t *row = fb + (y0 + lpy) * pitch + x0;
        tcache_t tc;
        uint32_t cur = 0xFFFFFFFFu;

        tc.tidx = 0;

        for (lpx = 0; lpx < SNES_NATIVE_W; lpx++) {
            uint32_t wx = (uint32_t)((int32_t)lpx + bg->scroll_x);
            uint32_t col = (wx >> 3) % bg->map_w;
            uint8_t ci;

            if (col != cur) { cache_tile(&tc, bg, col, wy); cur = col; }

            if (tc.tidx == 0) { row[lpx] = backdrop; continue; }

            ci = tc_pixel(&tc, wx & 7);
            row[lpx] = ci ? bg->palette[tc.pal_off + ci] : backdrop;
        }
    }
}

/* ================================================================
 *  Upper-BG compositors: front-to-back, first opaque wins, else the
 *  already-drawn bottom layer shows through (skip write)
 * ================================================================ */

/* Up to 3 BGs, bg[0] = front-most. Disabled/NULL entries skipped. */
static void composite_bgs(uint8_t *fb, uint32_t pitch,
                          uint32_t x0, uint32_t y0,
                          const snes_bg_t * const *bgs, int nbg)
{
    uint32_t lpy, lpx;
    int i;

    for (lpy = 0; lpy < SNES_NATIVE_H; lpy++) {
        uint8_t *row = fb + (y0 + lpy) * pitch + x0;
        tcache_t tc[3];
        uint32_t cur[3];
        uint32_t wy[3];

        for (i = 0; i < nbg; i++) {
            tc[i].tidx = 0;
            cur[i] = 0xFFFFFFFFu;
            wy[i] = bgs[i] ? (uint32_t)((int32_t)lpy + bgs[i]->scroll_y) : 0;
        }

        for (lpx = 0; lpx < SNES_NATIVE_W; lpx++) {
            for (i = 0; i < nbg; i++) {
                const snes_bg_t *bg = bgs[i];
                uint32_t wx, col;
                uint8_t ci;

                if (!bg || !bg->enabled) continue;

                wx = (uint32_t)((int32_t)lpx + bg->scroll_x);
                col = (wx >> 3) % bg->map_w;
                if (col != cur[i]) {
                    cache_tile(&tc[i], bg, col, wy[i]);
                    cur[i] = col;
                }

                if (tc[i].tidx) {
                    ci = tc_pixel(&tc[i], wx & 7);
                    if (ci) {
                        row[lpx] = bg->palette[tc[i].pal_off + ci];
                        break;   /* first opaque wins */
                    }
                }
            }
        }
    }
}

/* One BG with per-tile-column offset, composited (transparent-skip).
 * opaque != 0: bottom-layer variant (backdrop where empty). */
static void composite_bg_ofs(uint8_t *fb, uint32_t pitch,
                             uint32_t x0, uint32_t y0,
                             const snes_bg_t *bg,
                             const snes_tile_offset_t *ofs,
                             int opaque, uint8_t backdrop)
{
    uint32_t lpy, lpx;
    uint32_t tile_cols = SNES_NATIVE_W / 8;

    if (!bg || !bg->enabled) {
        if (opaque) {
            for (lpy = 0; lpy < SNES_NATIVE_H; lpy++) {
                uint8_t *row = fb + (y0 + lpy) * pitch + x0;
                for (lpx = 0; lpx < SNES_NATIVE_W; lpx++)
                    row[lpx] = backdrop;
            }
        }
        return;
    }

    for (lpy = 0; lpy < SNES_NATIVE_H; lpy++) {
        uint8_t *row = fb + (y0 + lpy) * pitch + x0;
        tcache_t tc;
        uint32_t cur = 0xFFFFFFFFu;
        int32_t cur_ofs = 0;

        tc.tidx = 0;

        for (lpx = 0; lpx < SNES_NATIVE_W; lpx++) {
            uint32_t scol = lpx >> 3;
            int32_t sx1, sy1;
            uint32_t wx1, wy1, col;
            uint8_t ci;

            if ((lpx & 7) == 0 && ofs && ofs->col_offset && scol < tile_cols)
                cur_ofs = ofs->col_offset[scol];

            sx1 = bg->scroll_x + ((ofs && !ofs->vertical) ? cur_ofs : 0);
            sy1 = bg->scroll_y + ((ofs && ofs->vertical)  ? cur_ofs : 0);
            wx1 = (uint32_t)((int32_t)lpx + sx1);
            wy1 = (uint32_t)((int32_t)lpy + sy1);

            col = (wx1 >> 3) % bg->map_w;
            if (col != cur || (lpx & 7) == 0) {
                cache_tile(&tc, bg, col, wy1);
                cur = col;
            }

            if (tc.tidx) {
                ci = tc_pixel(&tc, wx1 & 7);
                if (ci) {
                    row[lpx] = bg->palette[tc.pal_off + ci];
                    continue;
                }
            }
            if (opaque) row[lpx] = backdrop;
        }
    }
}

/* ---- Mode wrappers (layering identical to the SDK) ---- */

void snes_mode0_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_bg_t *bg3, const snes_bg_t *bg4)
{
    const snes_bg_t *bgs[3];
    bgs[0] = bg1; bgs[1] = bg2; bgs[2] = bg3;
    render_bg_opaque(fb, pitch, x0, y0, bg4, backdrop);
    composite_bgs(fb, pitch, x0, y0, bgs, 3);
}

void snes_mode1_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_bg_t *bg3)
{
    const snes_bg_t *bgs[2];
    bgs[0] = bg1; bgs[1] = bg2;
    render_bg_opaque(fb, pitch, x0, y0, bg3, backdrop);
    composite_bgs(fb, pitch, x0, y0, bgs, 2);
}

void snes_mode2_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_tile_offset_t *ofs)
{
    render_bg_opaque(fb, pitch, x0, y0, bg2, backdrop);
    composite_bg_ofs(fb, pitch, x0, y0, bg1, ofs, 0, 0);
}

void snes_mode3_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2)
{
    const snes_bg_t *bgs[1];
    bgs[0] = bg1;
    render_bg_opaque(fb, pitch, x0, y0, bg2, backdrop);
    composite_bgs(fb, pitch, x0, y0, bgs, 1);
}

void snes_mode4_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2,
                        const snes_tile_offset_t *ofs)
{
    render_bg_opaque(fb, pitch, x0, y0, bg2, backdrop);
    composite_bg_ofs(fb, pitch, x0, y0, bg1, ofs, 0, 0);
}

void snes_mode5_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1, const snes_bg_t *bg2)
{
    const snes_bg_t *bgs[1];
    bgs[0] = bg1;
    render_bg_opaque(fb, pitch, x0, y0, bg2, backdrop);
    composite_bgs(fb, pitch, x0, y0, bgs, 1);
}

void snes_mode6_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint8_t backdrop,
                        const snes_bg_t *bg1,
                        const snes_tile_offset_t *ofs)
{
    composite_bg_ofs(fb, pitch, x0, y0, bg1, ofs, 1, backdrop);
}

/* ================================================================== */
/* SNES 4bpp pixel decode (planar pairs)                               */
/* ================================================================== */
static uint8_t snes_4bpp_pixel(const uint8_t *tile, int row, int col)
{
    int shift = 7 - col;
    uint8_t b0 = (uint8_t)((tile[row * 2]      >> shift) & 1);
    uint8_t b1 = (uint8_t)((tile[row * 2 + 1]  >> shift) & 1);
    uint8_t b2 = (uint8_t)((tile[row * 2 + 16] >> shift) & 1);
    uint8_t b3 = (uint8_t)((tile[row * 2 + 17] >> shift) & 1);
    return (uint8_t)((b3 << 3) | (b2 << 2) | (b1 << 1) | b0);
}

/* ================================================================== */
/* Sprites                                                             */
/* ================================================================== */
void snes_render_sprites8(uint8_t *fb, uint32_t pitch,
                          uint32_t x0, uint32_t y0,
                          const uint8_t *sprite_chr,
                          const uint8_t *sprite_pal,
                          const snes_sprite_t *sprites,
                          uint32_t num_sprites)
{
    uint8_t line_count[SNES_NATIVE_H];
    uint32_t i;
    int s;

    if (!sprites || !sprite_chr || !sprite_pal || num_sprites == 0) return;

    for (i = 0; i < SNES_NATIVE_H; i++) line_count[i] = 0;

    /* Reverse order (lower index = higher priority, drawn last) */
    for (s = (int)num_sprites - 1; s >= 0; s--) {
        const snes_sprite_t *sp = &sprites[s];
        int32_t spw, sph, py, px;

        if (!sp->enabled || !sp->w || !sp->h) continue;

        spw = (int32_t)sp->w * 8;
        sph = (int32_t)sp->h * 8;

        for (py = 0; py < sph; py++) {
            int32_t screen_y = sp->y + py;
            int32_t actual_y, tile_y, fine_y;
            int drew;

            if (screen_y < 0 || screen_y >= SNES_NATIVE_H) continue;
            if (line_count[screen_y] >= SNES_SPRITES_PER_LINE) continue;

            actual_y = sp->flipv ? (sph - 1 - py) : py;
            tile_y = actual_y >> 3;
            fine_y = actual_y & 7;
            drew = 0;

            for (px = 0; px < spw; px++) {
                int32_t screen_x = sp->x + px;
                int32_t actual_x, tile_x;
                uint16_t tile_idx;
                const uint8_t *tile;
                uint8_t ci;

                if (screen_x < 0 || screen_x >= SNES_NATIVE_W) continue;

                actual_x = sp->fliph ? (spw - 1 - px) : px;
                tile_x = actual_x >> 3;

                /* Column-major: tile = base + tx * h + ty */
                tile_idx = (uint16_t)(sp->tile +
                                      (uint32_t)tile_x * sp->h + tile_y);
                tile = sprite_chr + tile_idx * 32;

                ci = snes_4bpp_pixel(tile, (int)fine_y, (int)(actual_x & 7));
                if (ci == 0) continue;

                fb[(y0 + (uint32_t)screen_y) * pitch +
                   x0 + (uint32_t)screen_x] = sprite_pal[sp->pal * 16 + ci];
                drew = 1;
            }
            if (drew) line_count[screen_y]++;
        }
    }
}

/* ================================================================== */
/* Mode 7 -- affine ground projection (plain C)                        */
/* ================================================================== */

#define M7_FP_SIN     12
#define M7_FP_UV      8
#define M7_SIN_N      256
#define M7_SIN_MASK   255

static int32_t s_m7_sin[M7_SIN_N];
static int32_t s_m7_cos[M7_SIN_N];
static int     s_m7_lut_built = 0;

static void m7_build_lut_once(void)
{
    int i;
    if (s_m7_lut_built) return;
    s_m7_lut_built = 1;
    for (i = 0; i < M7_SIN_N; i++) {
        int j = i & 127;
        int32_t val = (4 * j * (128 - j)) >> (14 - M7_FP_SIN);
        if (i >= 128) val = -val;
        s_m7_sin[i] = val;
    }
    for (i = 0; i < M7_SIN_N; i++)
        s_m7_cos[i] = s_m7_sin[(i + 64) & M7_SIN_MASK];
}

const int32_t *snes_sin_lut(void) { m7_build_lut_once(); return s_m7_sin; }
const int32_t *snes_cos_lut(void) { m7_build_lut_once(); return s_m7_cos; }

/* C replacement for mode7_scanline (mode7_neon.S): walk (u,v) across
 * one scanline, sample the tiled texture, emit palette indices. */
static void m7_scanline(uint8_t *row, const uint8_t *map,
                        const uint8_t *palette,
                        int32_t u, int32_t v, int32_t du, int32_t dv,
                        uint32_t count, uint32_t map_w_bits,
                        uint32_t map_mask)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t tx = ((uint32_t)u >> M7_FP_UV) & map_mask;
        uint32_t ty = ((uint32_t)v >> M7_FP_UV) & map_mask;
        row[i] = palette[map[(ty << map_w_bits) | tx]];
        u += du;
        v += dv;
    }
}

void snes_mode7_render8(uint8_t *fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0,
                        const snes_mode7_t *m7)
{
    uint32_t rw = SNES_NATIVE_W, rh = SNES_NATIVE_H;
    uint32_t horizon, floor_h, m7_h, sy;
    int32_t half_w, cam_x, cam_y, base_a, twist, space_z;

    m7_build_lut_once();

    horizon = m7->horizon < rh ? m7->horizon : rh;
    floor_h = rh - horizon;
    m7_h    = floor_h / 2;   /* half-vertical-res, line-doubled */

    half_w  = (int32_t)rw / 2;
    cam_x   = m7->cam_x;
    cam_y   = m7->cam_y;
    base_a  = m7->angle;
    twist   = m7->twist;
    space_z = m7->space_z ? m7->space_z : 8000;

    for (sy = 0; sy < m7_h; sy++) {
        int32_t p   = (int32_t)(sy * 2 + 1);
        int32_t lam = space_z / p;

        /* Per-scanline angle = base + (lam >> 2) * twist. twist=0 ->
         * rigid affine rotation; 1 -> classic SNES vortex swirl. */
        int32_t angle = (base_a + ((lam >> 2) * twist)) & M7_SIN_MASK;
        int32_t ca = s_m7_cos[angle];
        int32_t sa = s_m7_sin[angle];

        int32_t du = ((-sa) * lam / (int32_t)rw) >> (M7_FP_SIN - M7_FP_UV);
        int32_t dv = (( ca) * lam / (int32_t)rw) >> (M7_FP_SIN - M7_FP_UV);
        int32_t cx = cam_x + ((ca * lam) >> M7_FP_SIN);
        int32_t cy = cam_y + ((sa * lam) >> M7_FP_SIN);
        int32_t u  = (cx << M7_FP_UV) - du * half_w;
        int32_t v  = (cy << M7_FP_UV) - dv * half_w;

        uint32_t ry = horizon + sy * 2;
        uint8_t *row;
        uint32_t k;

        if (ry + 1 >= rh) break;

        row = fb + (y0 + ry) * pitch + x0;
        m7_scanline(row, m7->map, m7->palette, u, v, du, dv,
                    rw, m7->map_w_bits, m7->map_mask);
        /* Vertical line-doubling (was NEON memcpy on the V3s) */
        for (k = 0; k < rw; k++)
            row[pitch + k] = row[k];
    }
}
