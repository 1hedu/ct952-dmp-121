/*
 * JupiterSDK on CT952 -- hardware sprite engine implementation.
 *
 * Pure logic: clip, adjust source (mirror-aware), build the jgpu op,
 * hand it to the executor. On firmware the default executor drives the
 * real blitter; tests install a software-model executor.
 *
 * Mirrored clipping: the hardware reverses the programmed rect, so
 * cutting n pixels off the destination's LEFT keeps the source origin,
 * while cutting m off the RIGHT advances the source by m -- the exact
 * mirror of the normal case (derivation in the port notes).
 */
#include "jspr.h"

#ifndef JUP_HOST_BUILD
/* Firmware default: synchronous run on the real 2D engine */
static void exec_hw(const jgpu_op_t *op)
{
    jgpu_submit(op);
    jgpu_sync();
}
static jspr_exec_fn _exec = exec_hw;
#else
static jspr_exec_fn _exec = 0;
#endif

void jspr_set_exec(jspr_exec_fn fn)
{
#ifndef JUP_HOST_BUILD
    _exec = fn ? fn : exec_hw;
#else
    _exec = fn;
#endif
}

int jspr_blit(const jspr_surface_t *dst, int32_t dx, int32_t dy,
              const jspr_surface_t *atlas, uint32_t sx, uint32_t sy,
              uint32_t w, uint32_t h, uint8_t key, uint32_t flags)
{
    jgpu_op_t op;
    uint32_t gflags;
    int32_t cw = (int32_t)w, ch = (int32_t)h;
    int mirror = (flags & JSPR_HFLIP) != 0;

    if (!dst || !atlas || !_exec || w == 0 || h == 0)
        return -1;

    /* Vertical clip (same for both orientations) */
    if (dy < 0) { sy += (uint32_t)(-dy); ch += dy; dy = 0; }
    if (dy + ch > (int32_t)dst->h) ch = (int32_t)dst->h - dy;

    /* Horizontal clip, mirror-aware */
    if (dx < 0) {
        int32_t n = -dx;
        if (!mirror) sx += (uint32_t)n;   /* mirrored: source keeps origin */
        cw -= n;
        dx = 0;
    }
    if (dx + cw > (int32_t)dst->w) {
        int32_t m = dx + cw - (int32_t)dst->w;
        if (mirror) sx += (uint32_t)m;    /* mirrored: right clip eats source start */
        cw -= m;
    }

    if (cw <= 0 || ch <= 0)
        return -1;
    if (sx + (uint32_t)cw > atlas->w || sy + (uint32_t)ch > atlas->h)
        return -1;

    gflags = JGPU_F_HP | JGPU_F_BURST_MAX;
    if (!(flags & JSPR_OPAQUE)) gflags |= JGPU_F_KEY;
    if (mirror) gflags |= JGPU_F_MIRROR;

    if (jgpu_build_blit(&op, dst->base, dst->pitch,
                        (uint32_t)dx, (uint32_t)dy,
                        atlas->base, atlas->pitch, sx, sy,
                        (uint32_t)cw, (uint32_t)ch, key, gflags) != 0)
        return -1;

    _exec(&op);
    return 0;
}

int jspr_fill(const jspr_surface_t *dst, int32_t x, int32_t y,
              int32_t w, int32_t h, uint8_t color)
{
    jgpu_op_t op;

    if (!dst || !_exec)
        return -1;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)dst->w) w = (int32_t)dst->w - x;
    if (y + h > (int32_t)dst->h) h = (int32_t)dst->h - y;
    if (w <= 0 || h <= 0)
        return -1;

    if (jgpu_build_fill(&op, dst->base, dst->pitch,
                        (uint32_t)x, (uint32_t)y,
                        (uint32_t)w, (uint32_t)h, color,
                        JGPU_F_HP | JGPU_F_BURST_MAX) != 0)
        return -1;

    _exec(&op);
    return 0;
}
