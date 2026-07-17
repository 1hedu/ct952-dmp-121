/*
 * JupiterSDK on CT952 -- 2D engine op builders + software model.
 *
 * Pure code, no registers touched: builders compute the exact register
 * words the CT909S blitter takes, following the firmware's own
 * programming (fill: gdi.c:1290-1373, blit: gdi.c:2156-2206). The
 * model executes an op descriptor against plain memory with the same
 * walk the hardware performs, so tests can prove the register math
 * (sizes, alignment lanes, row advance, color key) byte-exact against
 * a CPU reference.
 */
#include "jgpu.h"

/* ctkav_gpu.h control-word fields (mirrored here so this file stays
 * portable; values are the CT909S ones used by gdi.c) */
#define GPU_OP            0x10000000u   /* CTL0[28] gpu_op enable */
#define GPU_OP_JUDGE_EN   0x00000800u   /* CTL0[11] scanline gate */
#define GPU_INT_EN        0x00000400u   /* CTL0[10] completion IRQ */
#define GPU_COL_KEY_EN    0x00000100u   /* CTL0[8] */
#define GPU_MIRROR_EN     0x00000040u   /* CTL0[6], HP blit only */
#define GPU_START         0x00000002u   /* CTL0[1] */

/* CT909S opcodes (gdi.c:96-104), field CTL0[4:2] */
#define OPC_BMPCOPY       0x4u
#define OPC_BMPCOPY_HP    0x5u
#define OPC_FILLRECT      0x6u
#define OPC_FILLRECT_HP   0x7u

/* Burst thresholds CTL0[18:16] read / [22:20] write; stock uses 4 */
static uint32_t ctl0_base(uint32_t flags)
{
    uint32_t t = (flags & JGPU_F_BURST_MAX) ? 7u : 4u;
    return GPU_INT_EN | GPU_OP | (t << 16) | (t << 20);
}

/* ag_offset for one surface row (CT909S, 8bpp): DWORD row-advance gap.
 * ag_width = DWORDs touched by the row (start-lane + width, rounded);
 * ag_offset = pitch_DWs - ag_width + 1.        (gdi.c:1296-1297) */
static uint32_t ag_offset(uint32_t pitch, uint32_t addr, uint32_t w)
{
    uint32_t ag_width = (w + (addr & 3) + 3) >> 2;
    return ((pitch + 3) >> 2) - ag_width + 1;
}

int jgpu_build_fill(jgpu_op_t *op,
                    uint32_t base, uint32_t pitch,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint8_t color, uint32_t flags)
{
    uint32_t addr, opc;

    if (op == NULL || w == 0 || h == 0 || w > 1023 || h > 1023)
        return -1;
    if (pitch == 0 || (pitch & 3) != 0)
        return -1;

    addr = base + y * pitch + x;
    opc = (flags & JGPU_F_HP) ? OPC_FILLRECT_HP : OPC_FILLRECT;

    op->ctl1 = (uint32_t)color << 24;                /* gdi.c:1321 */
    op->op_size = (h << 16) | w;                     /* gdi.c:1322 */
    op->ag_off = ag_offset(pitch, addr, w) << 16;    /* gdi.c:1325 */
    op->src_addr = 0;
    op->dst_addr = addr;
    op->op_thre = 0;
    op->ctl0 = ctl0_base(flags) | (opc << 2) | GPU_START;
    if (flags & JGPU_F_BEAM_GATE)
        op->ctl0 |= GPU_OP_JUDGE_EN;
    return 0;
}

int jgpu_build_blit(jgpu_op_t *op,
                    uint32_t dst_base, uint32_t dst_pitch,
                    uint32_t dx, uint32_t dy,
                    uint32_t src_base, uint32_t src_pitch,
                    uint32_t sx, uint32_t sy,
                    uint32_t w, uint32_t h,
                    uint8_t key, uint32_t flags)
{
    uint32_t saddr, daddr, opc;

    if (op == NULL || w == 0 || h == 0 || w > 1023 || h > 1023)
        return -1;
    if ((dst_pitch & 3) != 0 || (src_pitch & 3) != 0 ||
        dst_pitch == 0 || src_pitch == 0)
        return -1;

    saddr = src_base + sy * src_pitch + sx;
    daddr = dst_base + dy * dst_pitch + dx;

    /* Mirror is only defined for op mode 101 (ctkav_gpu.h:68) */
    opc = ((flags & (JGPU_F_HP | JGPU_F_MIRROR)) != 0)
              ? OPC_BMPCOPY_HP : OPC_BMPCOPY;

    op->ctl1 = (uint32_t)key << 16;                  /* GPU_COL_KEY field */
    op->op_size = (h << 16) | w;                     /* gdi.c:2188 */
    op->ag_off = (ag_offset(dst_pitch, daddr, w) << 16) |
                 ag_offset(src_pitch, saddr, w);     /* gdi.c:2191 */
    op->src_addr = saddr;
    op->dst_addr = daddr;
    op->op_thre = 0;
    /* HV increment bits [15:12] all zero = increment both (gdi.c:2185
     * ORs constants that are all 0 in the INC direction) */
    op->ctl0 = ctl0_base(flags) | (opc << 2) | GPU_START;
    if (flags & JGPU_F_KEY)
        op->ctl0 |= GPU_COL_KEY_EN;
    if (flags & JGPU_F_MIRROR)
        op->ctl0 |= GPU_MIRROR_EN;
    if (flags & JGPU_F_BEAM_GATE)
        op->ctl0 |= GPU_OP_JUDGE_EN;
    return 0;
}

void jgpu_op_set_gate(jgpu_op_t *op, uint32_t top, uint32_t bottom)
{
    /* gdi.c:1367: (top<<16)|bottom, 10-bit fields */
    op->op_thre = ((top & 0x3FF) << 16) | (bottom & 0x3FF);
    op->ctl0 |= GPU_OP_JUDGE_EN;
}

/* ================================================================== */
/* Software model                                                      */
/*                                                                     */
/* Walks rows exactly as the ag math implies: each row starts at       */
/* row_addr = addr + row*pitch (because ag_width + ag_offset - 1 =     */
/* pitch/4 when pitch%4==0, the DWORD walk lands on the same byte      */
/* lane every row -- verified structurally by the builders' shared     */
/* ag_offset()). Mirror reverses the source row horizontally.          */
/* ================================================================== */
void jgpu_model_exec(const jgpu_op_t *op,
                     uint8_t *dst_mem, uint32_t dst_mem_base,
                     const uint8_t *src_mem, uint32_t src_mem_base)
{
    uint32_t w = op->op_size & 0x3FF;
    uint32_t h = (op->op_size >> 16) & 0x3FF;
    uint32_t opc = (op->ctl0 >> 2) & 7;
    uint32_t dst_gap = (op->ag_off >> 16) & 0xFFFF;
    uint32_t src_gap = op->ag_off & 0xFFFF;
    uint32_t r, c;

    /* Reconstruct pitches from the ag gaps (inverse of ag_offset()) */
    uint32_t dst_agw = (w + (op->dst_addr & 3) + 3) >> 2;
    uint32_t dst_pitch = (dst_gap + dst_agw - 1) * 4;
    uint32_t src_agw = (w + (op->src_addr & 3) + 3) >> 2;
    uint32_t src_pitch = (src_gap + src_agw - 1) * 4;

    if (opc == OPC_FILLRECT || opc == OPC_FILLRECT_HP) {
        uint8_t color = (uint8_t)(op->ctl1 >> 24);
        for (r = 0; r < h; r++) {
            uint8_t *d = dst_mem + (op->dst_addr - dst_mem_base)
                       + r * dst_pitch;
            for (c = 0; c < w; c++)
                d[c] = color;
        }
        return;
    }

    if (opc == OPC_BMPCOPY || opc == OPC_BMPCOPY_HP) {
        int keyed = (op->ctl0 & GPU_COL_KEY_EN) != 0;
        int mirror = (op->ctl0 & GPU_MIRROR_EN) != 0;
        uint8_t key = (uint8_t)(op->ctl1 >> 16);
        for (r = 0; r < h; r++) {
            const uint8_t *s = src_mem + (op->src_addr - src_mem_base)
                             + r * src_pitch;
            uint8_t *d = dst_mem + (op->dst_addr - dst_mem_base)
                       + r * dst_pitch;
            for (c = 0; c < w; c++) {
                uint8_t px = mirror ? s[w - 1 - c] : s[c];
                if (keyed && px == key)
                    continue;
                d[c] = px;
            }
        }
    }
}
