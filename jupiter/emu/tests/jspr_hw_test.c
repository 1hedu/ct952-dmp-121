/*
 * ct952emu jspr verification: run the Jupiter sprite API (jspr) on the
 * emulated CT952 2-D GPU. jspr clips each sprite and builds a jgpu op;
 * one executor submits it to the real engine (the firmware path), the
 * other runs jgpu's software model over flat DRAM. Byte-exact agreement
 * across clipped / flipped / opaque sprites proves jspr drives the real
 * engine exactly as its model intends.
 *
 * Freestanding SPARC: start.S calls testmain(); return = mismatching
 * bytes (0 = pass).
 */
#include "jup_types.h"
#include "jgpu.h"
#include "jspr.h"
#include "testapi.h"

/* CT952 2-D GPU registers */
#define GPU 0x80002880u
#define GREG(o) (*(volatile uint32_t *)(GPU + (o)))
static void gpu_submit(const jgpu_op_t *op)
{
    GREG(0x04) = op->ctl1;    GREG(0x0C) = op->op_size;
    GREG(0x10) = op->ag_off;  GREG(0x14) = op->src_addr;
    GREG(0x18) = op->dst_addr;
    __asm__ volatile ("" ::: "memory");
    GREG(0x00) = op->ctl0;
}
static void exec_gpu(const jgpu_op_t *op) { gpu_submit(op); }
static void exec_model(const jgpu_op_t *op)
{
    /* whole DRAM addressed flat at its own address */
    jgpu_model_exec(op, (uint8_t *)0x40000000u, 0x40000000u,
                        (const uint8_t *)0x40000000u, 0x40000000u);
}

#define DPITCH 128u
#define DROWS  96u
#define DPLANE (DPITCH * DROWS)
#define GP_BASE 0x40300000u
#define MD_BASE 0x40340000u
#define ATLAS_BASE 0x40390000u
#define APITCH 64u
#define AKEY 0xFFu
#define SPRW 16u
#define SPRH 16u

static uint8_t *const gp = (uint8_t *)GP_BASE;
static uint8_t *const md = (uint8_t *)MD_BASE;
static uint8_t *const at = (uint8_t *)ATLAS_BASE;

static void build_atlas(void)
{
    uint32_t x, y;
    for (y = 0; y < SPRH; y++)
        for (x = 0; x < SPRW; x++) {
            uint8_t v;
            if (x == 0 || y == 0 || x == SPRW - 1 || y == SPRH - 1) v = 0x2A;
            else if (x == y)                v = 0xC3;   /* asymmetric: shows flip */
            else if ((x + 2 * y) & 3)        v = (uint8_t)(0x50 + ((x + y) & 0x2F));
            else v = AKEY;                                /* transparent hole */
            at[y * APITCH + x] = v;
        }
}

struct spr { int32_t dx, dy; uint32_t flags; };
static const struct spr SPR[] = {
    {  10,  8, 0 },                         /* inside, keyed              */
    {  -6,  20, 0 },                        /* clipped left               */
    { 118, 30, 0 },                         /* clipped right (DPITCH-w+6) */
    {  40, -5, 0 },                         /* clipped top                */
    {  70, 86, 0 },                         /* clipped bottom             */
    {  20, 50, JSPR_HFLIP },                /* mirrored, inside           */
    {  -4, 66, JSPR_HFLIP },                /* mirrored + clipped left    */
    {  90, 60, JSPR_OPAQUE },               /* rect copy (no key)         */
};
#define NSPR (int)(sizeof(SPR)/sizeof(SPR[0]))

static void run(uint32_t base)
{
    jspr_surface_t dst = { base, DPITCH, DPITCH, DROWS };
    jspr_surface_t atl = { ATLAS_BASE, APITCH, SPRW, SPRH };
    int i;
    jspr_fill(&dst, 0, 0, (int32_t)DPITCH, (int32_t)DROWS, 0x07);  /* bg */
    for (i = 0; i < NSPR; i++)
        jspr_blit(&dst, SPR[i].dx, SPR[i].dy, &atl, 0, 0,
                  SPRW, SPRH, AKEY, SPR[i].flags);
}

unsigned testmain(void)
{
    uint32_t i;
    int mism = 0;
    for (i = 0; i < DPLANE; i++) { gp[i] = 0; md[i] = 0; }
    build_atlas();

    jspr_set_exec(exec_gpu);   run(GP_BASE);
    jspr_set_exec(exec_model); run(MD_BASE);

    for (i = 0; i < DPLANE; i++)
        if (gp[i] != md[i]) mism++;
    return (unsigned)mism;
}
