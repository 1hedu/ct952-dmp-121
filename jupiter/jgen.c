/*
 * JupiterSDK on CT952 -- Genesis VDP-style renderer, indexed-8bpp port
 * of Jupiter SDK lib/genesis.c.
 *
 * Composites back-to-front into one 8bpp buffer:
 *   Plane B  opaque (backdrop where empty)
 *   Plane A  transparent-skip, excluded from the window rectangle
 *   Window   transparent-skip inside its rectangle
 *   Sprites  transparent-skip, on top (Plane A's tiles + CRAM)
 */
#include "jgen.h"

/* Pre-resolved tile column: cached entry + decoded tile row pointer */
typedef struct {
    const uint8_t *trow;   /* pointer to this row's tile pixel data */
    uint32_t pal_off;      /* palette offset in CRAM (pal_num * 16) */
    uint16_t tidx;         /* tile index (0 = transparent/empty) */
    uint8_t  fliph;
} tcache_t;

/* Cache one tile column for one plane at the current scanline. */
static void cache_tile(tcache_t *tc, const genesis_plane_t *p,
                       uint32_t tile_col, uint32_t world_y)
{
    uint32_t row, flipv, fy;
    uint16_t entry;

    row = (world_y >> 3) & (uint32_t)(p->map_h - 1);
    entry = p->map[row * p->map_w + tile_col];
    tc->tidx  = GEN_GET_TILE(entry);
    tc->fliph = (uint8_t)GEN_GET_FLIPH(entry);

    if (tc->tidx == 0) return;

    flipv = GEN_GET_FLIPV(entry);
    fy = flipv ? (7 - (world_y & 7)) : (world_y & 7);

    tc->trow    = p->tiles + tc->tidx * 32 + fy * 4;
    tc->pal_off = (uint32_t)GEN_GET_PAL(entry) * 16;
}

/* Decode one 4bpp pixel from a cached tile column. */
static uint8_t tc_pixel(const tcache_t *tc, uint32_t fine_x)
{
    uint32_t fx;
    uint8_t b;
    if (tc->tidx == 0) return 0;
    fx = tc->fliph ? (7 - fine_x) : fine_x;
    b = tc->trow[fx >> 1];
    return (fx & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
}

/* Render a horizontal span [lx0,lx1) of one plane row.
 * opaque: write backdrop where empty; else skip (composite). */
static void render_plane_row_span(uint8_t *row, const genesis_plane_t *p,
                                  uint32_t lpy, int32_t lx0, int32_t lx1,
                                  int opaque, uint8_t backdrop)
{
    tcache_t tc;
    uint32_t cur = 0xFFFFFFFFu;
    int32_t lx;
    int32_t sx = p->scroll_x;
    uint32_t mw = (uint32_t)(p->map_w - 1);
    uint32_t wy;

    if (p->line_hscroll) sx += p->line_hscroll[lpy];
    wy = (uint32_t)((int32_t)lpy + p->scroll_y);

    tc.tidx = 0;

    for (lx = lx0; lx < lx1; lx++) {
        uint32_t wx = (uint32_t)(lx + sx);
        uint32_t col = (wx >> 3) & mw;
        uint8_t ci;

        if (col != cur) { cache_tile(&tc, p, col, wy); cur = col; }

        if (tc.tidx) {
            ci = tc_pixel(&tc, wx & 7);
            if (ci) { row[lx] = p->cram[tc.pal_off + ci]; continue; }
        }
        if (opaque) row[lx] = backdrop;
    }
}

static void fill_row_span(uint8_t *row, int32_t lx0, int32_t lx1, uint8_t v)
{
    int32_t lx;
    for (lx = lx0; lx < lx1; lx++) row[lx] = v;
}

/* Sprite on top; coords relative to the render rect. Tiles column-major. */
static void render_sprite(uint8_t *fb, uint32_t pitch,
                          uint32_t rx0, uint32_t ry0,
                          const genesis_sprite_t *s,
                          const uint8_t *tiles,
                          const uint8_t *cram)
{
    int32_t sw, sh, lx0, ly0, lx1, ly1, ly;
    uint32_t pal_off;

    if (!s || !s->enabled || !s->w || !s->h) return;

    sw = (int32_t)s->w * 8;
    sh = (int32_t)s->h * 8;

    lx0 = s->x; ly0 = s->y;
    lx1 = lx0 + sw; ly1 = ly0 + sh;
    if (lx0 < 0) lx0 = 0;
    if (ly0 < 0) ly0 = 0;
    if (lx1 > GEN_NATIVE_W) lx1 = GEN_NATIVE_W;
    if (ly1 > GEN_NATIVE_H) ly1 = GEN_NATIVE_H;
    if (lx0 >= lx1 || ly0 >= ly1) return;

    pal_off = (uint32_t)s->pal * 16;

    for (ly = ly0; ly < ly1; ly++) {
        int32_t sprite_y = ly - s->y;
        int32_t actual_y = s->flipv ? (sh - 1 - sprite_y) : sprite_y;
        uint32_t tile_y = (uint32_t)(actual_y >> 3);
        uint32_t fine_y = (uint32_t)(actual_y & 7);

        uint8_t *row = fb + (ry0 + (uint32_t)ly) * pitch + rx0;

        const uint8_t *cur_trow = 0;
        int32_t cur_tile_x = -1;
        int32_t lx;

        for (lx = lx0; lx < lx1; lx++) {
            int32_t sprite_x = lx - s->x;
            int32_t actual_x = s->fliph ? (sw - 1 - sprite_x) : sprite_x;
            int32_t tile_x = actual_x >> 3;
            uint32_t fine_x;
            uint8_t b, ci;

            if (tile_x != cur_tile_x) {
                /* Column-major: tile index = base + tx*h + ty */
                uint16_t tile_idx =
                    (uint16_t)(s->tile + (uint32_t)tile_x * s->h + tile_y);
                cur_trow = tiles + tile_idx * 32 + fine_y * 4;
                cur_tile_x = tile_x;
            }

            fine_x = (uint32_t)(actual_x & 7);
            b = cur_trow[fine_x >> 1];
            ci = (fine_x & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);

            if (ci) row[lx] = cram[pal_off + ci];
        }
    }
}

/* Window on top of the buffer inside its rectangle (transparent-skip;
 * Plane A was excluded there, so empty window pixels show Plane B). */
static void render_window(uint8_t *fb, uint32_t pitch,
                          uint32_t x0, uint32_t y0,
                          const genesis_window_t *w,
                          const genesis_plane_t *plane_a)
{
    uint32_t wx0, wy0, wx1, wy1, ly;
    uint32_t mw, mh;

    if (!w || !w->map || !plane_a || !plane_a->enabled) return;

    wx0 = w->x; wy0 = w->y;
    wx1 = (uint32_t)w->x + w->w; if (wx1 > GEN_NATIVE_W) wx1 = GEN_NATIVE_W;
    wy1 = (uint32_t)w->y + w->h; if (wy1 > GEN_NATIVE_H) wy1 = GEN_NATIVE_H;
    if (wx0 >= wx1 || wy0 >= wy1) return;

    mw = (uint32_t)(w->map_w - 1);
    mh = (uint32_t)(w->map_h - 1);

    for (ly = wy0; ly < wy1; ly++) {
        uint32_t local_y = ly - wy0;
        uint32_t ty = (local_y >> 3) & mh;
        uint32_t fy = local_y & 7;
        uint8_t *row = fb + (y0 + ly) * pitch + x0;

        tcache_t tc;
        uint32_t cur = 0xFFFFFFFFu;
        uint32_t lx;

        tc.tidx = 0;

        for (lx = wx0; lx < wx1; lx++) {
            uint32_t local_x = lx - wx0;
            uint32_t col = (local_x >> 3) & mw;
            uint8_t ci;

            if (col != cur) {
                uint16_t entry = w->map[ty * w->map_w + col];
                tc.tidx  = GEN_GET_TILE(entry);
                tc.fliph = (uint8_t)GEN_GET_FLIPH(entry);
                if (tc.tidx) {
                    uint32_t flipv = GEN_GET_FLIPV(entry);
                    uint32_t ry = flipv ? (7 - fy) : fy;
                    tc.trow    = plane_a->tiles + tc.tidx * 32 + ry * 4;
                    tc.pal_off = (uint32_t)GEN_GET_PAL(entry) * 16;
                }
                cur = col;
            }

            if (tc.tidx) {
                ci = tc_pixel(&tc, local_x & 7);
                if (ci) row[lx] = plane_a->cram[tc.pal_off + ci];
            }
        }
    }
}

void genesis_render8(uint8_t *fb, uint32_t pitch,
                     uint32_t x0, uint32_t y0,
                     uint8_t backdrop,
                     const genesis_plane_t *plane_a,
                     const genesis_plane_t *plane_b,
                     const genesis_window_t *window,
                     const genesis_sprite_t *sprites, uint32_t num_sprites)
{
    uint32_t ly, i;
    int win_active = (window && window->map &&
                      plane_a && plane_a->enabled);
    uint32_t wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0;

    if (win_active) {
        wx0 = window->x; wy0 = window->y;
        wx1 = (uint32_t)window->x + window->w;
        wy1 = (uint32_t)window->y + window->h;
        if (wx1 > GEN_NATIVE_W) wx1 = GEN_NATIVE_W;
        if (wy1 > GEN_NATIVE_H) wy1 = GEN_NATIVE_H;
        if (wx0 >= wx1 || wy0 >= wy1) win_active = 0;
    }

    for (ly = 0; ly < GEN_NATIVE_H; ly++) {
        uint8_t *row = fb + (y0 + ly) * pitch + x0;

        /* Plane B: opaque back layer */
        if (plane_b && plane_b->enabled)
            render_plane_row_span(row, plane_b, ly, 0, GEN_NATIVE_W,
                                  1, backdrop);
        else
            fill_row_span(row, 0, GEN_NATIVE_W, backdrop);

        /* Plane A: composited, excluded from the window rectangle */
        if (plane_a && plane_a->enabled) {
            if (win_active && ly >= wy0 && ly < wy1) {
                render_plane_row_span(row, plane_a, ly, 0, (int32_t)wx0,
                                      0, 0);
                render_plane_row_span(row, plane_a, ly, (int32_t)wx1,
                                      GEN_NATIVE_W, 0, 0);
            } else {
                render_plane_row_span(row, plane_a, ly, 0, GEN_NATIVE_W,
                                      0, 0);
            }
        }
    }

    render_window(fb, pitch, x0, y0, window, plane_a);

    /* Sprites on top, sharing Plane A's tiles/CRAM */
    if (sprites && num_sprites && plane_a && plane_a->enabled) {
        for (i = 0; i < num_sprites; i++)
            render_sprite(fb, pitch, x0, y0,
                          &sprites[i], plane_a->tiles, plane_a->cram);
    }
}
