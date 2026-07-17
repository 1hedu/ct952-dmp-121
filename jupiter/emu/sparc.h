/*
 * ct952emu -- SPARC V8 interpreter core.
 *
 * The CT909's PROC1/PROC2 are LEON2-class SPARC V8 integer units
 * (no FPU -- the firmware is -msoft-float). This core implements the
 * full V8 integer ISA: register windows, delayed control transfer,
 * traps/interrupts, UMUL/SMUL/UDIV/SDIV, MULScc, atomics, alternate-
 * space accesses. FP/CP ops deliver fp_disabled/cp_disabled traps.
 *
 * Memory goes through bus callbacks; the core is endian-clean (the
 * bus deals in values, not bytes).
 */
#ifndef CT952EMU_SPARC_H
#define CT952EMU_SPARC_H

#include <stdint.h>

#define SPARC_NWIN 8

/* PSR fields */
#define PSR_IMPL   0xF0000000u
#define PSR_VER    0x0F000000u
#define PSR_N      0x00800000u
#define PSR_Z      0x00400000u
#define PSR_V      0x00200000u
#define PSR_C      0x00100000u
#define PSR_EC     0x00002000u
#define PSR_EF     0x00001000u
#define PSR_PIL    0x00000F00u
#define PSR_S      0x00000080u
#define PSR_PS     0x00000040u
#define PSR_ET     0x00000020u
#define PSR_CWP    0x0000001Fu

/* Trap types */
#define TT_INSTR_ACCESS   0x01
#define TT_ILLEGAL        0x02
#define TT_PRIVILEGED     0x03
#define TT_FP_DISABLED    0x04
#define TT_WIN_OVERFLOW   0x05
#define TT_WIN_UNDERFLOW  0x06
#define TT_UNALIGNED      0x07
#define TT_DATA_ACCESS    0x09
#define TT_TAG_OVERFLOW   0x0A
#define TT_CP_DISABLED    0x24
#define TT_DIV_ZERO       0x2A
#define TT_IRQ(level)     (0x10 + (level))
#define TT_TRAP(n)        (0x80 + ((n) & 0x7F))

struct sparc;

typedef struct sparc_bus {
    /* size: 1/2/4. Return value zero-extended. *fault nonzero on bus
     * error. addr is pre-aligned by the core for the access size. */
    uint32_t (*read)(struct sparc_bus *bus, uint32_t addr, int size,
                     int *fault);
    void (*write)(struct sparc_bus *bus, uint32_t addr, uint32_t val,
                  int size, int *fault);
    /* Highest pending interrupt level (1-15), 0 if none. */
    int (*irq_level)(struct sparc_bus *bus);
    /* Called when the core takes interrupt `level` (ack/clear). */
    void (*irq_ack)(struct sparc_bus *bus, int level);
} sparc_bus_t;

typedef struct sparc {
    uint32_t g[8];                      /* globals (g0 forced 0) */
    uint32_t wr[SPARC_NWIN * 16];       /* windowed regs */
    uint32_t pc, npc;
    uint32_t psr, wim, tbr, y;
    sparc_bus_t *bus;
    uint64_t icount;
    int halted;                          /* error mode / external stop */
    char halt_reason[128];
    uint32_t halt_pc;
    /* control-flow trace ring: register-indirect jumps (jmpl) */
    uint32_t tr_from[32], tr_to[32];
    int tr_i;
    /* per-instruction PC ring: pinpoints the exact hot loop at halt */
    uint32_t pc_ring[64];
    int pc_ri;
} sparc_t;

/* Reset: PC=0, nPC=4, S=1, ET=0, CWP=0, impl/ver = LEON2-ish */
void sparc_reset(sparc_t *c, sparc_bus_t *bus);

/* Execute up to n instructions (stops early if halted). Returns the
 * number executed. */
uint64_t sparc_run(sparc_t *c, uint64_t n);

/* Register access helpers (r index 0..31 in the current window) */
uint32_t sparc_get_reg(sparc_t *c, int idx);
void sparc_set_reg(sparc_t *c, int idx, uint32_t v);

#endif /* CT952EMU_SPARC_H */
