/*
 * JupiterSDK on CT952 -- hardware sprite engine (jspr)
 *
 * The port's answer to the Jupiter SDK's sprite.c + sprite_neon.S:
 * color-keyed sprite blits from an atlas surface, executed by the 2D
 * engine (jgpu) instead of NEON rows. Horizontal flip uses the
 * blitter's hardware mirror. Clipping against the destination surface
 * is done here (the hardware has none), including the mirrored-clip
 * source adjustment.
 *
 * Surfaces are pitched 8bpp DRAM buffers (the OSD format): the OSD
 * frame, or any scratch area, can be atlas or target. Sprites are
 * just rects in an atlas -- unpack art once, blit forever.
 *
 * Execution is pluggable so the same code runs everywhere:
 *   firmware  default exec = jgpu_submit + jgpu_sync (real blitter)
 *   tests     exec = jgpu_model_exec against host buffers
 * jspr never touches registers itself.
 */
#ifndef JSPR_H
#define JSPR_H

#include "jup_types.h"
#include "jgpu.h"

/* A pitched 8bpp surface, addressed by hardware byte address */
typedef struct {
    uint32_t base;    /* byte address of pixel (0,0) */
    uint32_t pitch;   /* bytes per row, multiple of 4 */
    uint32_t w, h;    /* bounds used for clipping */
} jspr_surface_t;

/* Sprite flags */
#define JSPR_HFLIP  (1u << 0)   /* hardware mirror (HP opcode) */
#define JSPR_OPAQUE (1u << 1)   /* no color key (rect copy) */

/* Executor hook: runs one built op to completion. */
typedef void (*jspr_exec_fn)(const jgpu_op_t *op);

/* Set the executor. Pass NULL to restore the platform default
 * (firmware: synchronous jgpu submit+sync; host builds: no default --
 * tests must install a model-backed executor). */
void jspr_set_exec(jspr_exec_fn fn);

/* Blit a w x h sprite from (sx,sy) of the atlas to (dx,dy) of dst
 * (signed -- may hang off any edge; clipped here). Pixels equal to
 * `key` are transparent unless JSPR_OPAQUE. Returns 0 if anything was
 * drawn, -1 if fully clipped or invalid. */
int jspr_blit(const jspr_surface_t *dst, int32_t dx, int32_t dy,
              const jspr_surface_t *atlas, uint32_t sx, uint32_t sy,
              uint32_t w, uint32_t h, uint8_t key, uint32_t flags);

/* Fill helper on the same surface type (hardware fill-rect), clipped. */
int jspr_fill(const jspr_surface_t *dst, int32_t x, int32_t y,
              int32_t w, int32_t h, uint8_t color);

#endif /* JSPR_H */
