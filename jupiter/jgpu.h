/*
 * JupiterSDK on CT952 -- 2D engine driver (jgpu)
 *
 * A proper driver for the CT952's 2D block-mover ("GPU", the VPU's
 * blitter face at REG_GPU_BASE 0x2880): hardware rectangle fills and
 * bitmap blits with color-key transparency, over 8bpp indexed surfaces
 * (the OSD format the whole jupiter/ port draws in).
 *
 * What this driver adds over the stock GDI paths (gdi.c):
 *   - arbitrary pitched surfaces and sub-rect blits, not just whole
 *     OSD regions / whole bitmap resources
 *   - async operation: kick an op, do CPU work, sync later (stock
 *     firmware only busy-waits)
 *   - the _HP ("higher-performance") opcodes and max DRAM burst
 *     thresholds the stock driver never enables
 *   - optional hardware beam-race gating (GPU_OP_JUDGE) for tear-free
 *     single-buffer drawing
 *
 * Register programming is lifted from the firmware's own accelerated
 * paths (fill: gdi.c:1260-1389; blit: gdi.c:2129-2214; opcodes:
 * gdi.c:96-131; register map: ctkav_gpu.h). The active CT909S variant
 * is implemented (10-bit sizes, 4-byte alignment, ag-offset-only).
 *
 * Layering:
 *   jgpu.c        pure op builders + a software model of the block
 *                 (portable; the model lets tests verify the register
 *                 math byte-exactly against the CPU reference)
 *   jgpu_ct952.c  firmware backend: submit/sync on the real registers
 *
 * Constraints (hardware, CT909S path):
 *   - max op size 1023x1023 px; surface pitch must be a multiple of 4
 *   - 8bpp surfaces here (the hardware also does 4/2bpp by packing;
 *     not exposed in this driver)
 *   - one op in flight at a time (the block has no command queue)
 *   - mirror is only valid with the HP blit opcode (ctkav_gpu.h:68)
 */
#ifndef JGPU_H
#define JGPU_H

#include "jup_types.h"

/* Op flags */
#define JGPU_F_HP        (1u << 0)  /* use the higher-performance opcode */
#define JGPU_F_KEY       (1u << 1)  /* blit: skip pixels == key color */
#define JGPU_F_MIRROR    (1u << 2)  /* blit: horizontal mirror (forces HP) */
#define JGPU_F_BURST_MAX (1u << 3)  /* DRAM burst threshold 7 (stock: 4) */
#define JGPU_F_BEAM_GATE (1u << 4)  /* gate on display scanline (OP_THRE) */

/* A fully-computed operation: every register value the hardware needs.
 * Builders are pure functions of their inputs -- testable off-target. */
typedef struct {
    uint32_t ctl0;       /* control word incl. opcode + START */
    uint32_t ctl1;       /* fill color / color key */
    uint32_t op_size;    /* (h<<16)|w */
    uint32_t ag_off;     /* (dst_ag_offset<<16)|src_ag_offset */
    uint32_t src_addr;   /* source byte address (0 for fills) */
    uint32_t dst_addr;   /* destination byte address */
    uint32_t op_thre;    /* scanline gate bounds (JGPU_F_BEAM_GATE) */
} jgpu_op_t;

/* Build a fill: color -> rect (x,y,w,h) of an 8bpp surface at byte
 * address `base` with row pitch `pitch` (multiple of 4). Returns 0,
 * or -1 if the rect/pitch violates hardware limits. */
int jgpu_build_fill(jgpu_op_t *op,
                    uint32_t base, uint32_t pitch,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint8_t color, uint32_t flags);

/* Build a blit: w x h pixels from (sx,sy) of the src surface to
 * (dx,dy) of the dst surface (8bpp, pitches multiple of 4). With
 * JGPU_F_KEY, pixels equal to `key` are skipped (transparent). */
int jgpu_build_blit(jgpu_op_t *op,
                    uint32_t dst_base, uint32_t dst_pitch,
                    uint32_t dx, uint32_t dy,
                    uint32_t src_base, uint32_t src_pitch,
                    uint32_t sx, uint32_t sy,
                    uint32_t w, uint32_t h,
                    uint8_t key, uint32_t flags);

/* Set the beam-gate scanline bounds on a built op (display lines
 * [top,bottom]; the hardware defers the op while the beam is inside).
 * Call after build, before submit, with JGPU_F_BEAM_GATE set. */
void jgpu_op_set_gate(jgpu_op_t *op, uint32_t top, uint32_t bottom);

/* ---- Software model (for tests; exact intended semantics) ----
 * Executes the op against host memory: dst_mem/src_mem map the flat
 * byte address spaces containing dst_addr/src_addr (address a maps to
 * mem[a - mem_base]). Fill ops ignore src. */
void jgpu_model_exec(const jgpu_op_t *op,
                     uint8_t *dst_mem, uint32_t dst_mem_base,
                     const uint8_t *src_mem, uint32_t src_mem_base);

/* ---- Firmware backend (jgpu_ct952.c) ----
 * submit: reset the VPU, program the op, kick it. Takes the GDI/JPU
 * arbitration mutex and HOLDS it until jgpu_sync() -- always pair.
 * sync: busy-wait completion (firmware idiom) and release the mutex.
 * busy: nonzero while the engine is still running (poll to overlap
 * CPU work between submit and sync). */
void jgpu_submit(const jgpu_op_t *op);
int  jgpu_busy(void);
void jgpu_sync(void);

/* Synchronous conveniences (submit + sync) */
int jgpu_fill(uint32_t base, uint32_t pitch,
              uint32_t x, uint32_t y, uint32_t w, uint32_t h,
              uint8_t color, uint32_t flags);
int jgpu_blit(uint32_t dst_base, uint32_t dst_pitch,
              uint32_t dx, uint32_t dy,
              uint32_t src_base, uint32_t src_pitch,
              uint32_t sx, uint32_t sy,
              uint32_t w, uint32_t h,
              uint8_t key, uint32_t flags);

#endif /* JGPU_H */
