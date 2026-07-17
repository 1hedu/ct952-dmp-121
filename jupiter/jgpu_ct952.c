/*
 * JupiterSDK on CT952 -- 2D engine firmware backend.
 *
 * Submits jgpu ops to the real blitter, following the firmware's own
 * protocol (gdi.c GXA paths): take the GDI/JPU arbitration mutex,
 * pulse the VPU reset, enable gpu_op, program the registers, memory
 * barrier, write CTL0 with START. Completion reuses the firmware's
 * _gdi_CheckCmdComplete() (spin on GPU_STATUS with a 50000-iteration
 * timeout + VPU reset on hang, gdi.c:3208-3234).
 *
 * The split submit/sync lets the CPU work while the blitter runs --
 * the one thing the stock driver never does.
 *
 * HARDWARE-UNVERIFIED: mirrors gdi.c's programming exactly, but has
 * not run on silicon in this effort.
 */
#include "Winav.h"
#include "ctkav.h"
#include "gdi.h"

#include "jgpu.h"

/* Firmware internals reused (declared in gdi.c, non-static) */
extern void _gdi_ResetVPU(void);
extern void _gdi_CheckCmdComplete(void);
extern MUTEX_T __mutexGDIIdle;

#define JGPU_GPU_OP      0x10000000u
#define JGPU_GPU_STATUS  0x00000200u

static BYTE _bJgpuInFlight = 0;

void jgpu_submit(const jgpu_op_t *op)
{
    OS_LockMutex(&__mutexGDIIdle);

    _gdi_ResetVPU();
    REG_GPU_CTL0 |= JGPU_GPU_OP;      /* enable gpu_op (gdi.c:1275) */

    REG_GPU_CTL1 = op->ctl1;
    REG_GPU_OP_SIZE = op->op_size;
    REG_GPU_AG_OFF = op->ag_off;
    REG_GPU_SRC_ADDR = op->src_addr;
    REG_GPU_DEST_ADDR = op->dst_addr;
    if (op->op_thre)
        REG_GPU_OP_THRE = op->op_thre;

    asm volatile ("" : : : "memory");  /* order regs before START */

    REG_GPU_CTL0 = op->ctl0;
    _bJgpuInFlight = 1;
}

int jgpu_busy(void)
{
    if (!_bJgpuInFlight)
        return 0;
    return (REG_GPU_CTL0 & JGPU_GPU_STATUS) != 0;
}

void jgpu_sync(void)
{
    if (!_bJgpuInFlight)
        return;
    _gdi_CheckCmdComplete();
    _bJgpuInFlight = 0;
    OS_UnlockMutex(&__mutexGDIIdle);
}

int jgpu_fill(uint32_t base, uint32_t pitch,
              uint32_t x, uint32_t y, uint32_t w, uint32_t h,
              uint8_t color, uint32_t flags)
{
    jgpu_op_t op;
    if (jgpu_build_fill(&op, base, pitch, x, y, w, h, color, flags) != 0)
        return -1;
    jgpu_submit(&op);
    jgpu_sync();
    return 0;
}

int jgpu_blit(uint32_t dst_base, uint32_t dst_pitch,
              uint32_t dx, uint32_t dy,
              uint32_t src_base, uint32_t src_pitch,
              uint32_t sx, uint32_t sy,
              uint32_t w, uint32_t h,
              uint8_t key, uint32_t flags)
{
    jgpu_op_t op;
    if (jgpu_build_blit(&op, dst_base, dst_pitch, dx, dy,
                        src_base, src_pitch, sx, sy, w, h,
                        key, flags) != 0)
        return -1;
    jgpu_submit(&op);
    jgpu_sync();
    return 0;
}
