/*
 * ct952emu jgpu verification: drive the emulated CT952 2-D GPU engine
 * through the Jupiter blitter driver's own op-builders (jgpu.c), then
 * check the pixels the emulated engine produced byte-for-byte against
 * jgpu's software model (jgpu_model_exec, already verified against a CPU
 * reference in test_gpu). This closes the loop the port could not close
 * before the emulator existed: jgpu register programming -> real engine
 * semantics -> pixels.
 *
 * Freestanding SPARC (no libc): start.S calls testmain(), whose return
 * value is the count of mismatching bytes across all fills (0 = the
 * driver programs the engine exactly as its model intends).
 */
#include "jup_types.h"
#include "jgpu.h"
#include "testapi.h"

/* CT952 2-D GPU register block (ctkav_gpu.h) */
#define GPU 0x80002880u
#define GREG(o) (*(volatile uint32_t *)(GPU + (o)))
#define CTL0 0x00
#define CTL1 0x04
#define OPSZ 0x0C
#define AGOF 0x10
#define SRCA 0x14
#define DSTA 0x18

/* Submit an op exactly as jgpu_ct952.c does (minus the firmware GDI/JPU
 * mutex + VPU reset, which are arbitration, not op semantics): program
 * the registers, barrier, then CTL0 with START -- which triggers the
 * emulator's gpu_exec. */
static void gpu_submit(const jgpu_op_t *op)
{
    GREG(CTL1) = op->ctl1;
    GREG(OPSZ) = op->op_size;
    GREG(AGOF) = op->ag_off;
    GREG(SRCA) = op->src_addr;
    GREG(DSTA) = op->dst_addr;
    __asm__ volatile ("" ::: "memory");
    GREG(CTL0) = op->ctl0;          /* GPU_START -> engine runs */
}

#define PITCH 128u
#define ROWS  96u
#define PLANE (PITCH * ROWS)
#define GP_BASE 0x40300000u         /* engine target   */
#define MD_BASE 0x40340000u         /* model target    */

static uint8_t *const gp = (uint8_t *)GP_BASE;
static uint8_t *const md = (uint8_t *)MD_BASE;

struct fill { uint32_t x, y, w, h; uint8_t c; };

/* full-width clears and partial-width rects (the interesting stride case) */
static const struct fill FILLS[] = {
    {   0,  0, PITCH, ROWS, 0x11 },   /* full clear                */
    {   8,  4,   40,  20,  0xA5 },   /* partial: narrow rect       */
    {  60, 30,   50,  40,  0x3C },   /* partial: mid                */
    { 100, 10,   20,  80,  0x7E },   /* partial: tall thin, right   */
    {   0, 50, PITCH, 10,  0x22 },   /* full-width band             */
    {   3, 70,   12,  18,  0x5B },   /* partial: unaligned x        */
};
#define NFILL (int)(sizeof(FILLS)/sizeof(FILLS[0]))

unsigned testmain(void)
{
    uint32_t i;
    int f, mism = 0;

    for (i = 0; i < PLANE; i++) { gp[i] = 0; md[i] = 0; }

    for (f = 0; f < NFILL; f++) {
        jgpu_op_t og, omd;
        /* engine path */
        if (jgpu_build_fill(&og, GP_BASE, PITCH,
                            FILLS[f].x, FILLS[f].y, FILLS[f].w, FILLS[f].h,
                            FILLS[f].c, 0) != 0)
            return 0xE0000000u | (uint32_t)f;   /* builder rejected */
        gpu_submit(&og);
        /* model path (same op, different base) */
        if (jgpu_build_fill(&omd, MD_BASE, PITCH,
                            FILLS[f].x, FILLS[f].y, FILLS[f].w, FILLS[f].h,
                            FILLS[f].c, 0) != 0)
            return 0xE1000000u | (uint32_t)f;
        jgpu_model_exec(&omd, md, MD_BASE, (const uint8_t *)0, 0);
    }

    for (i = 0; i < PLANE; i++)
        if (gp[i] != md[i]) mism++;

    return (unsigned)mism;      /* 0 = engine == model, byte-exact */
}
