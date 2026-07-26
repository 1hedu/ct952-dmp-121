/*
 * ct952emu -- SPARC V8 interpreter implementation.
 *
 * Reference: The SPARC Architecture Manual, Version 8.
 * Execution model: pc/npc pair; every control transfer is delayed.
 * Traps push into the previous window (CWP-1) without WIM check --
 * the window overflow/underflow handlers themselves run there, which
 * is why NWIN-1 windows are usable and WIM marks the invalid one.
 */
#include "sparc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Whole-run PC histogram (opt-in via CT952_PCHIST): bucket PROC1 PCs to
 * 16-byte granularity into an open-addressing table; dump the hottest at
 * exit. Finds the dominant region across the entire run, unlike the 64-entry
 * ring which only shows where the cutoff landed. */
#define PCH_N 65536u
static uint32_t g_pch_key[PCH_N];
static uint64_t g_pch_cnt[PCH_N];
static int g_pch_on = -1;
static void pch_sample(uint32_t pc)
{
    if (g_pch_on < 0) g_pch_on = getenv("CT952_PCHIST") ? 1 : 0;
    if (!g_pch_on) return;
    uint32_t k = pc >> 4;
    uint32_t h = (k * 2654435761u) & (PCH_N - 1);
    for (uint32_t i = 0; i < PCH_N; i++) {
        uint32_t s = (h + i) & (PCH_N - 1);
        if (g_pch_cnt[s] == 0) { g_pch_key[s] = k; g_pch_cnt[s] = 1; return; }
        if (g_pch_key[s] == k) { g_pch_cnt[s]++; return; }
    }
}
void sparc_pchist_dump(FILE *f, int topn)
{
    if (g_pch_on <= 0) return;
    for (int t = 0; t < topn; t++) {
        uint32_t best = 0; uint64_t bc = 0;
        for (uint32_t i = 0; i < PCH_N; i++)
            if (g_pch_cnt[i] > bc) { bc = g_pch_cnt[i]; best = i; }
        if (!bc) break;
        fprintf(f, "[PCHIST] %2d  pc~%08x  %llu\n", t,
                g_pch_key[best] << 4, (unsigned long long)bc);
        g_pch_cnt[best] = 0;   /* consume so next scan finds the next-hottest */
    }
}

#define CWP(c)   ((int)((c)->psr & PSR_CWP))
#define NWIN     SPARC_NWIN

static uint32_t win_slot(int cwp, int idx)   /* idx 8..31 */
{
    return (uint32_t)(((cwp * 16) + (idx - 8)) % (NWIN * 16));
}

uint32_t sparc_get_reg(sparc_t *c, int idx)
{
    if (idx == 0) return 0;
    if (idx < 8) return c->g[idx];
    return c->wr[win_slot(CWP(c), idx)];
}

void sparc_set_reg(sparc_t *c, int idx, uint32_t v)
{
    if (idx == 0) return;
    if (idx < 8) { c->g[idx] = v; return; }
    c->wr[win_slot(CWP(c), idx)] = v;
}

/* Register-window backtrace: walk from the current window outward (restore
 * direction, CWP+1), reading each frame's %i7 (return address, idx 31). Reliable
 * even when windows have not been spilled to the stack. Fills `out` with up to
 * `max` return addresses; returns the count. Stops at NWIN frames or a return
 * address outside XIP flash (<0x00100000). */
int sparc_win_backtrace(sparc_t *c, uint32_t *out, int max)
{
    int n = 0, w = CWP(c), k;
    for (k = 0; k < NWIN && n < max; k++) {
        uint32_t i7 = c->wr[win_slot(w, 31)];
        if (i7 == 0 || i7 >= 0x00100000u) break;   /* leave flash text -> stop */
        out[n++] = i7;
        w = (w + 1) % NWIN;
    }
    return n;
}

static void set_reg_w(sparc_t *c, int cwp, int idx, uint32_t v)
{
    if (idx == 0) return;
    if (idx < 8) { c->g[idx] = v; return; }
    c->wr[win_slot(cwp, idx)] = v;
}

void sparc_reset(sparc_t *c, sparc_bus_t *bus)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->pc = 0;
    c->npc = 4;
    /* PSR impl=0xA ver=0: the real CT952 SPARC chip ID. The stock boot
     * code reads PSR[31:24] and only takes its full clock/DRAM/section
     * init path when it reads 0xa0 (else it falls to a debugger-style
     * trampoline that expects pre-staged entry/SP registers). S=1, ET=0,
     * CWP=0. */
    c->psr = 0xA0000000u | PSR_S;
    c->wim = 0;
    c->tbr = 0;
}

static void halt(sparc_t *c, const char *why)
{
    c->halted = 1;
    c->halt_pc = c->pc;
    snprintf(c->halt_reason, sizeof(c->halt_reason), "%s", why);
}

/* Enter a trap. If ET=0 this is error mode -> halt. */
static void do_trap(sparc_t *c, uint32_t tt, uint32_t pc, uint32_t npc)
{
    int newcwp;
    char buf[64];

    if (!(c->psr & PSR_ET)) {
        snprintf(buf, sizeof(buf), "error mode: trap 0x%02x with ET=0",
                 (unsigned)tt);
        halt(c, buf);
        return;
    }
    c->psr &= ~PSR_ET;
    if (c->psr & PSR_S) c->psr |= PSR_PS; else c->psr &= ~PSR_PS;
    c->psr |= PSR_S;
    newcwp = (CWP(c) + NWIN - 1) % NWIN;
    c->psr = (c->psr & ~PSR_CWP) | (uint32_t)newcwp;
    set_reg_w(c, newcwp, 17, pc);    /* %l1 */
    set_reg_w(c, newcwp, 18, npc);   /* %l2 */
    c->tbr = (c->tbr & 0xFFFFF000u) | (tt << 4);
    c->pc = c->tbr;
    c->npc = c->tbr + 4;
}

/* icc helpers */
static void set_icc_logic(sparc_t *c, uint32_t r)
{
    c->psr &= ~(PSR_N | PSR_Z | PSR_V | PSR_C);
    if (r & 0x80000000u) c->psr |= PSR_N;
    if (r == 0) c->psr |= PSR_Z;
}

static void set_icc_add(sparc_t *c, uint32_t a, uint32_t b, uint32_t r)
{
    c->psr &= ~(PSR_N | PSR_Z | PSR_V | PSR_C);
    if (r & 0x80000000u) c->psr |= PSR_N;
    if (r == 0) c->psr |= PSR_Z;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c->psr |= PSR_V;
    if (((a & b) | (~r & (a | b))) & 0x80000000u) c->psr |= PSR_C;
}

static void set_icc_sub(sparc_t *c, uint32_t a, uint32_t b, uint32_t r)
{
    c->psr &= ~(PSR_N | PSR_Z | PSR_V | PSR_C);
    if (r & 0x80000000u) c->psr |= PSR_N;
    if (r == 0) c->psr |= PSR_Z;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c->psr |= PSR_V;
    if (((~a & b) | (r & (~a | b))) & 0x80000000u) c->psr |= PSR_C;
}

static int cond_true(sparc_t *c, int cond)
{
    int n = !!(c->psr & PSR_N), z = !!(c->psr & PSR_Z);
    int v = !!(c->psr & PSR_V), cf = !!(c->psr & PSR_C);
    switch (cond & 0xF) {
    case 0x0: return 0;                     /* n   */
    case 0x1: return z;                     /* e   */
    case 0x2: return z || (n != v);         /* le  */
    case 0x3: return n != v;                /* l   */
    case 0x4: return cf || z;               /* leu */
    case 0x5: return cf;                    /* cs  */
    case 0x6: return n;                     /* neg */
    case 0x7: return v;                     /* vs  */
    case 0x8: return 1;                     /* a   */
    case 0x9: return !z;                    /* ne  */
    case 0xA: return !(z || (n != v));      /* g   */
    case 0xB: return n == v;                /* ge  */
    case 0xC: return !(cf || z);            /* gu  */
    case 0xD: return !cf;                   /* cc  */
    case 0xE: return !n;                    /* pos */
    default:  return !v;                    /* vc  */
    }
}

/* One instruction. Returns 0 normally, 1 if halted. */
static int step(sparc_t *c)
{
    uint32_t pc = c->pc, npc = c->npc;
    uint32_t inst;
    int fault = 0;
    int irq;

    /* interrupts */
    if ((c->psr & PSR_ET) && c->bus->irq_level) {
        irq = c->bus->irq_level(c->bus);
        if (irq > 0 &&
            (irq == 15 || (uint32_t)irq > ((c->psr & PSR_PIL) >> 8))) {
            if (c->bus->irq_ack) c->bus->irq_ack(c->bus, irq);
            do_trap(c, (uint32_t)TT_IRQ(irq), pc, npc);
            return c->halted;
        }
    }

    if (pc & 3) {
        do_trap(c, TT_UNALIGNED, pc, npc);
        return c->halted;
    }
    inst = c->bus->read(c->bus, pc, 4, &fault);
    if (fault) {
        do_trap(c, TT_INSTR_ACCESS, pc, npc);
        return c->halted;
    }
    c->pc_ring[c->pc_ri++ & 63] = pc;
    c->icount++;
    pch_sample(pc);

    {
        uint32_t op = inst >> 30;
        uint32_t rd = (inst >> 25) & 31;
        uint32_t op3 = (inst >> 19) & 63;
        uint32_t rs1 = (inst >> 14) & 31;
        uint32_t imm = (inst >> 13) & 1;
        uint32_t asi = (inst >> 5) & 0xFF;
        uint32_t rs2 = inst & 31;
        uint32_t simm13 = inst & 0x1FFF;
        uint32_t operand2;
        uint32_t a_bit, cond, disp22;

        if (simm13 & 0x1000) simm13 |= 0xFFFFE000u;   /* sign extend */
        operand2 = imm ? simm13 : sparc_get_reg(c, (int)rs2);

        c->pc = npc;
        c->npc = npc + 4;

        switch (op) {
        case 1: {  /* CALL */
            uint32_t disp = inst << 2;
            sparc_set_reg(c, 15, pc);
            c->npc = pc + disp;
            return 0;
        }
        case 0: {  /* Bicc / SETHI / UNIMP */
            uint32_t op2 = (inst >> 22) & 7;
            switch (op2) {
            case 4:  /* SETHI */
                sparc_set_reg(c, (int)rd, inst << 10);
                return 0;
            case 2: {  /* Bicc */
                a_bit = (inst >> 29) & 1;
                cond = (inst >> 25) & 0xF;
                disp22 = inst & 0x3FFFFF;
                if (disp22 & 0x200000) disp22 |= 0xFFC00000u;
                if (cond_true(c, (int)cond)) {
                    c->npc = pc + (disp22 << 2);
                    if (cond == 8 && a_bit)   /* ba,a: annul slot */
                        { c->pc = c->npc; c->npc += 4; }
                } else if (a_bit) {           /* untaken, annul slot */
                    c->pc = c->npc; c->npc += 4;
                }
                return 0;
            }
            case 6:  /* FBfcc */
                do_trap(c, TT_FP_DISABLED, pc, npc);
                return c->halted;
            case 7:  /* CBccc */
                do_trap(c, TT_CP_DISABLED, pc, npc);
                return c->halted;
            default:
                do_trap(c, TT_ILLEGAL, pc, npc);
                return c->halted;
            }
        }
        case 2: {  /* arithmetic / control */
            uint32_t a = sparc_get_reg(c, (int)rs1);
            uint32_t b = operand2;
            uint32_t r;
            switch (op3) {
            case 0x00: sparc_set_reg(c, (int)rd, a + b); return 0;
            case 0x01: sparc_set_reg(c, (int)rd, a & b); return 0;
            case 0x02: sparc_set_reg(c, (int)rd, a | b); return 0;
            case 0x03: sparc_set_reg(c, (int)rd, a ^ b); return 0;
            case 0x04: sparc_set_reg(c, (int)rd, a - b); return 0;
            case 0x05: sparc_set_reg(c, (int)rd, a & ~b); return 0;
            case 0x06: sparc_set_reg(c, (int)rd, a | ~b); return 0;
            case 0x07: sparc_set_reg(c, (int)rd, ~(a ^ b)); return 0;
            case 0x08:  /* addx */
                r = a + b + (!!(c->psr & PSR_C));
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x0C:  /* subx */
                r = a - b - (!!(c->psr & PSR_C));
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x0A: {  /* umul */
                uint64_t p = (uint64_t)a * (uint64_t)b;
                c->y = (uint32_t)(p >> 32);
                sparc_set_reg(c, (int)rd, (uint32_t)p); return 0;
            }
            case 0x0B: {  /* smul */
                int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
                c->y = (uint32_t)((uint64_t)p >> 32);
                sparc_set_reg(c, (int)rd, (uint32_t)p); return 0;
            }
            case 0x0E: {  /* udiv */
                uint64_t dend = ((uint64_t)c->y << 32) | a;
                uint64_t q;
                if (b == 0) { do_trap(c, TT_DIV_ZERO, pc, npc); return c->halted; }
                q = dend / b;
                if (q > 0xFFFFFFFFu) q = 0xFFFFFFFFu;
                sparc_set_reg(c, (int)rd, (uint32_t)q); return 0;
            }
            case 0x0F: {  /* sdiv */
                int64_t dend = (int64_t)(((uint64_t)c->y << 32) | a);
                int64_t q;
                if (b == 0) { do_trap(c, TT_DIV_ZERO, pc, npc); return c->halted; }
                q = dend / (int32_t)b;
                if (q > 0x7FFFFFFFLL) q = 0x7FFFFFFFLL;
                if (q < -0x80000000LL) q = -0x80000000LL;
                sparc_set_reg(c, (int)rd, (uint32_t)q); return 0;
            }
            case 0x10: r = a + b; set_icc_add(c, a, b, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x11: r = a & b; set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x12: r = a | b; set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x13: r = a ^ b; set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x14: r = a - b; set_icc_sub(c, a, b, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x15: r = a & ~b; set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x16: r = a | ~b; set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x17: r = ~(a ^ b); set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            case 0x18: {  /* addxcc */
                uint32_t cin = !!(c->psr & PSR_C);
                r = a + b + cin;
                set_icc_add(c, a, b, r);
                /* carry-in can generate carry when r==a with b=~0 */
                if (cin && (b + cin) == 0) c->psr |= PSR_C;
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x1C: {  /* subxcc */
                uint32_t cin = !!(c->psr & PSR_C);
                r = a - b - cin;
                set_icc_sub(c, a, b, r);
                if (cin && b == 0xFFFFFFFFu) c->psr |= PSR_C;
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x1A: {  /* umulcc */
                uint64_t p = (uint64_t)a * (uint64_t)b;
                c->y = (uint32_t)(p >> 32);
                r = (uint32_t)p;
                set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x1B: {  /* smulcc */
                int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
                c->y = (uint32_t)((uint64_t)p >> 32);
                r = (uint32_t)p;
                set_icc_logic(c, r);
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x1E: {  /* udivcc */
                uint64_t dend = ((uint64_t)c->y << 32) | a;
                uint64_t q;
                int ovf = 0;
                if (b == 0) { do_trap(c, TT_DIV_ZERO, pc, npc); return c->halted; }
                q = dend / b;
                if (q > 0xFFFFFFFFu) { q = 0xFFFFFFFFu; ovf = 1; }
                r = (uint32_t)q;
                set_icc_logic(c, r);
                if (ovf) c->psr |= PSR_V;
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x1F: {  /* sdivcc */
                int64_t dend = (int64_t)(((uint64_t)c->y << 32) | a);
                int64_t q;
                int ovf = 0;
                if (b == 0) { do_trap(c, TT_DIV_ZERO, pc, npc); return c->halted; }
                q = dend / (int32_t)b;
                if (q > 0x7FFFFFFFLL) { q = 0x7FFFFFFFLL; ovf = 1; }
                if (q < -0x80000000LL) { q = -0x80000000LL; ovf = 1; }
                r = (uint32_t)q;
                set_icc_logic(c, r);
                if (ovf) c->psr |= PSR_V;
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x20: case 0x22: {  /* taddcc(tv) */
                r = a + b;
                set_icc_add(c, a, b, r);
                if ((a | b) & 3) c->psr |= PSR_V;
                if (op3 == 0x22 && (c->psr & PSR_V)) {
                    do_trap(c, TT_TAG_OVERFLOW, pc, npc); return c->halted;
                }
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x21: case 0x23: {  /* tsubcc(tv) */
                r = a - b;
                set_icc_sub(c, a, b, r);
                if ((a | b) & 3) c->psr |= PSR_V;
                if (op3 == 0x23 && (c->psr & PSR_V)) {
                    do_trap(c, TT_TAG_OVERFLOW, pc, npc); return c->halted;
                }
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x24: {  /* mulscc */
                uint32_t nv = (!!(c->psr & PSR_N)) ^ (!!(c->psr & PSR_V));
                uint32_t o1 = (a >> 1) | (nv << 31);
                uint32_t add = (c->y & 1) ? b : 0;
                r = o1 + add;
                set_icc_add(c, o1, add, r);
                c->y = (c->y >> 1) | (a << 31);
                sparc_set_reg(c, (int)rd, r); return 0;
            }
            case 0x25: sparc_set_reg(c, (int)rd, a << (b & 31)); return 0;
            case 0x26: sparc_set_reg(c, (int)rd, a >> (b & 31)); return 0;
            case 0x27:
                sparc_set_reg(c, (int)rd,
                              (uint32_t)((int32_t)a >> (b & 31)));
                return 0;
            case 0x28:  /* rdy / rdasr */
                sparc_set_reg(c, (int)rd, (rs1 == 0) ? c->y : 0);
                return 0;
            case 0x29:  /* rdpsr */
                sparc_set_reg(c, (int)rd, c->psr); return 0;
            case 0x2A:  /* rdwim */
                sparc_set_reg(c, (int)rd, c->wim); return 0;
            case 0x2B:  /* rdtbr */
                sparc_set_reg(c, (int)rd, c->tbr); return 0;
            case 0x30:  /* wry / wrasr */
                if (rd == 0) c->y = a ^ b;
                return 0;
            case 0x31: {  /* wrpsr */
                uint32_t v = a ^ b;
                c->psr = (c->psr & (PSR_IMPL | PSR_VER)) |
                         (v & ~(PSR_IMPL | PSR_VER));
                if ((c->psr & PSR_CWP) >= NWIN)
                    c->psr = (c->psr & ~PSR_CWP) | ((v & PSR_CWP) % NWIN);
                return 0;
            }
            case 0x32:  /* wrwim */
                c->wim = (a ^ b) & ((1u << NWIN) - 1); return 0;
            case 0x33:  /* wrtbr */
                c->tbr = (a ^ b) & 0xFFFFF000u; return 0;
            case 0x34: case 0x35:  /* FPop */
                do_trap(c, TT_FP_DISABLED, pc, npc); return c->halted;
            case 0x36: case 0x37:  /* CPop */
                do_trap(c, TT_CP_DISABLED, pc, npc); return c->halted;
            case 0x38: {  /* jmpl */
                uint32_t target = a + b;
                c->tr_from[c->tr_i & 31] = pc;
                c->tr_to[c->tr_i & 31] = target;
                c->tr_i++;
                if (target & 3) {
                    do_trap(c, TT_UNALIGNED, pc, npc); return c->halted;
                }
                sparc_set_reg(c, (int)rd, pc);
                c->npc = target;
                return 0;
            }
            case 0x39: {  /* rett */
                uint32_t target = a + b;
                int newcwp = (CWP(c) + 1) % NWIN;
                if (c->psr & PSR_ET) {
                    do_trap(c, TT_ILLEGAL, pc, npc); return c->halted;
                }
                if (!(c->psr & PSR_S)) { halt(c, "rett in user mode"); return 1; }
                if (c->wim & (1u << newcwp)) {
                    do_trap(c, TT_WIN_UNDERFLOW, pc, npc); return c->halted;
                }
                if (target & 3) {
                    do_trap(c, TT_UNALIGNED, pc, npc); return c->halted;
                }
                c->psr = (c->psr & ~PSR_CWP) | (uint32_t)newcwp;
                c->psr |= PSR_ET;
                if (c->psr & PSR_PS) c->psr |= PSR_S; else c->psr &= ~PSR_S;
                c->npc = target;
                return 0;
            }
            case 0x3A: {  /* Ticc */
                cond = (inst >> 25) & 0xF;
                if (cond_true(c, (int)cond)) {
                    uint32_t tn = (a + b) & 0x7F;
                    do_trap(c, TT_TRAP(tn), pc, npc);
                    return c->halted;
                }
                return 0;
            }
            case 0x3B:  /* iflush */
                return 0;
            case 0x3C: {  /* save */
                int newcwp = (CWP(c) + NWIN - 1) % NWIN;
                if (c->wim & (1u << newcwp)) {
                    do_trap(c, TT_WIN_OVERFLOW, pc, npc); return c->halted;
                }
                r = a + b;   /* computed in OLD window */
                c->psr = (c->psr & ~PSR_CWP) | (uint32_t)newcwp;
                set_reg_w(c, newcwp, (int)rd, r);
                return 0;
            }
            case 0x3D: {  /* restore */
                int newcwp = (CWP(c) + 1) % NWIN;
                if (c->wim & (1u << newcwp)) {
                    do_trap(c, TT_WIN_UNDERFLOW, pc, npc); return c->halted;
                }
                r = a + b;
                c->psr = (c->psr & ~PSR_CWP) | (uint32_t)newcwp;
                set_reg_w(c, newcwp, (int)rd, r);
                return 0;
            }
            default:
                do_trap(c, TT_ILLEGAL, pc, npc);
                return c->halted;
            }
        }
        case 3: {  /* load/store */
            uint32_t a = sparc_get_reg(c, (int)rs1);
            uint32_t addr = a + operand2;
            uint32_t v;
            int is_alt = (op3 & 0x30) == 0x10;

            if (is_alt) {
                if (imm) { do_trap(c, TT_ILLEGAL, pc, npc); return c->halted; }
                if (!(c->psr & PSR_S)) {
                    do_trap(c, TT_PRIVILEGED, pc, npc); return c->halted;
                }
                /* LEON: ASI 2 = system control regs -- read 0/ignore.
                 * ASIs 0x01/0x08-0x0B behave as normal memory; cache
                 * flush ASIs (0x05/0x06/0x10/0x11) are no-ops. */
                if (asi == 0x02 || asi == 0x05 || asi == 0x06 ||
                    asi == 0x10 || asi == 0x11) {
                    if ((op3 & 0x0F) == 0x00)       /* lda */
                        sparc_set_reg(c, (int)rd, 0);
                    return 0;
                }
            }

            switch (op3 & 0x0F) {
            case 0x0:  /* ld */
                if (addr & 3) { do_trap(c, TT_UNALIGNED, pc, npc); return c->halted; }
                v = c->bus->read(c->bus, addr, 4, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, v);
                return 0;
            case 0x1:  /* ldub */
                v = c->bus->read(c->bus, addr, 1, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, v & 0xFF);
                return 0;
            case 0x2:  /* lduh */
                if (addr & 1) { do_trap(c, TT_UNALIGNED, pc, npc); return c->halted; }
                v = c->bus->read(c->bus, addr, 2, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, v & 0xFFFF);
                return 0;
            case 0x3: {  /* ldd */
                uint32_t v2;
                if ((addr & 7) || (rd & 1)) {
                    do_trap(c, TT_UNALIGNED, pc, npc); return c->halted;
                }
                v = c->bus->read(c->bus, addr, 4, &fault);
                v2 = c->bus->read(c->bus, addr + 4, 4, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, v);
                sparc_set_reg(c, (int)rd + 1, v2);
                return 0;
            }
            case 0x4:  /* st */
                if (addr & 3) { do_trap(c, TT_UNALIGNED, pc, npc); return c->halted; }
                c->bus->write(c->bus, addr, sparc_get_reg(c, (int)rd), 4, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                return 0;
            case 0x5:  /* stb */
                c->bus->write(c->bus, addr, sparc_get_reg(c, (int)rd) & 0xFF,
                              1, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                return 0;
            case 0x6:  /* sth */
                if (addr & 1) { do_trap(c, TT_UNALIGNED, pc, npc); return c->halted; }
                c->bus->write(c->bus, addr, sparc_get_reg(c, (int)rd) & 0xFFFF,
                              2, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                return 0;
            case 0x7:  /* std */
                if ((addr & 7) || (rd & 1)) {
                    do_trap(c, TT_UNALIGNED, pc, npc); return c->halted;
                }
                c->bus->write(c->bus, addr, sparc_get_reg(c, (int)rd), 4, &fault);
                c->bus->write(c->bus, addr + 4, sparc_get_reg(c, (int)rd + 1),
                              4, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                return 0;
            case 0x9:  /* ldsb */
                v = c->bus->read(c->bus, addr, 1, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, (uint32_t)(int32_t)(int8_t)(v & 0xFF));
                return 0;
            case 0xA:  /* ldsh */
                if (addr & 1) { do_trap(c, TT_UNALIGNED, pc, npc); return c->halted; }
                v = c->bus->read(c->bus, addr, 2, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd,
                              (uint32_t)(int32_t)(int16_t)(v & 0xFFFF));
                return 0;
            case 0xD:  /* ldstub */
                v = c->bus->read(c->bus, addr, 1, &fault);
                c->bus->write(c->bus, addr, 0xFF, 1, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, v & 0xFF);
                return 0;
            case 0xF:  /* swap */
                if (addr & 3) { do_trap(c, TT_UNALIGNED, pc, npc); return c->halted; }
                v = c->bus->read(c->bus, addr, 4, &fault);
                c->bus->write(c->bus, addr, sparc_get_reg(c, (int)rd), 4, &fault);
                if (fault) { do_trap(c, TT_DATA_ACCESS, pc, npc); return c->halted; }
                sparc_set_reg(c, (int)rd, v);
                return 0;
            default:
                do_trap(c, TT_ILLEGAL, pc, npc);
                return c->halted;
            }
        }
        }
    }
    return 0;
}

/* Full-speed execution-PC watch (CT952_PCHIT): §12.42 -- log if/when any of the
 * CC-mailbox event producers / PostEvent execute, and their caller. A handful of
 * integer compares per instruction; no single-stepping. Reliable where the
 * gdbstub's Ctrl-C interrupt is not (10.49). */
uint64_t sparc_run(sparc_t *c, uint64_t n)
{
    uint64_t i;
    static int pchit = -1;
    static const uint32_t WL[] = {
        0x00012f10u, /* PostEvent->list */   0x00006eecu, /* EvtDispatch_bit80 */
        0x00006430u, 0x00006798u, 0x000075d0u, /* mbox-post wrappers */
        0x000ad4ccu,                           /* 0xad4cc mbox-put */
        0x00061cf8u, /* mode-7 handler (flag setter) */
    };
    static uint16_t hit[8];
    if (pchit < 0) pchit = getenv("CT952_PCHIT") ? 1 : 0;
    /* Configurable PC first-hit tracer (CT952_PCWATCH="0xaaa,0xbbb,..."): log the
     * first N times execution reaches each listed PC, with %o0/%o1 and icount.
     * Pinpoints how far a call chain gets (which callee never returns). */
    static int pw = -1; static uint32_t pwl[16]; static int pwn = 0;
    static uint16_t pwh[16];
    if (pw < 0) { pw = 0; const char *e = getenv("CT952_PCWATCH");
        if (e) { char b[256]; strncpy(b, e, 255); b[255]=0;
            char *t = strtok(b, ","); while (t && pwn < 16) {
                pwl[pwn++] = (uint32_t)strtoul(t, NULL, 0); t = strtok(NULL, ","); }
            pw = pwn ? 1 : 0; } }
    /* mbox-get caller trace (CT952_MBOXTRACE): log every call to the generic
     * mbox-get wrapper 0x5969c with its caller-site (%o7) and object arg (%o0),
     * past the park window. The FINAL non-returning get names the CC_DVD_MainLoop
     * poll-call that blocks (§12.46). */
    static int mbt = -1; static uint32_t mbtn = 0; static uint64_t mbfrom = 0;
    if (mbt < 0) { mbt = getenv("CT952_MBOXTRACE") ? 1 : 0;
        const char *e = getenv("CT952_MBOX_FROM"); mbfrom = e ? strtoull(e,NULL,0) : 34000000ull; }
    /* Flag-wait trace (CT952_FLAGTRACE=<from>): log every cyg_flag_wait (eCos
     * 0x4001dffc) past <from> with the flag object (%o0), mask (%o1) and caller
     * (%i7) -- names what the parked threads block on when the boot plateaus. */
    static long ftr = -2; static uint32_t ftn = 0;
    if (ftr == -2) { const char *e = getenv("CT952_FLAGTRACE");
                     ftr = e ? (long)strtoull(e, NULL, 0) : -1; }
    /* Periodic PC sampler (CT952_PCSAMPLE=N): every N instructions print the
     * current PC so a single run reveals the boot trajectory / terminal hot loop
     * without many slow run-to samples. */
    static long pcsamp = -2; static uint64_t pcs_next = 0;
    if (pcsamp == -2) { const char *e = getenv("CT952_PCSAMPLE");
        pcsamp = e ? (long)strtoul(e, NULL, 0) : 0; }
    /* DIAGNOSTIC (CT952_SKIP_READY): the boot-init readiness call 0x5abb4(7) at
     * 0x41a90 never returns (§12.63), so the worker-resume at 0x41b04 is never
     * reached. Skip the call (jump straight to its return site 0x41a98, faking
     * return 0 in %o0) so the boot reaches the resume path -- tests whether
     * starting the CC-event worker cascades the whole boot forward. */
    static int skipready = -1;
    if (skipready < 0) skipready = getenv("CT952_SKIP_READY") ? 1 : 0;
    for (i = 0; i < n; i++) {
        if (c->halted) break;
        if (c->brk_pc && c->pc == c->brk_pc) break;   /* stop AT the bp, don't execute it */
        if (skipready && c->pc == 0x41a90u) {
            /* Jump straight to the worker-resume call 0x41b04 (skip 0x5abb4 AND
             * the i0!=0 skip-branch), with %o0=8 as that call expects. Decisive
             * test: does starting the CC-event worker cascade the boot? */
            sparc_set_reg(c, 8, 8);          /* %o0 = 8 (arg 0x66dc expects) */
            c->pc = 0x41b04u; c->npc = 0x41b08u;
            fprintf(stderr, "[SKIPREADY] jumped to worker-resume 0x41b04 at icount=%llu\n",
                    (unsigned long long)c->icount);
            continue;
        }
        /* Indirect-call target logger (CT952_ICALL): at the message-pump indirect
         * dispatch sites 0xa720 (call %o1) and 0xa778 (call %o0), log the handler
         * target and message type (%l0/%o0). Reveals which event handlers actually
         * run -- and whether a POWERONMENU-triggering message ever arrives. The
         * pump uses interprocedural indirect dispatch, invisible to static callers. */
        /* Experiment (CT952_FORCE_POM=<icount>): the message pump loops handler
         * 0xa0 (splash tick 0x2747c) forever because the splash->menu transition
         * flags never reach ready. Once past <icount>, redirect ONE indirect
         * dispatch at 0xa778 to the POWERONMENU wrapper 0x2620c, so the pump calls
         * POWERONMENU_Initial(1) in-context. Validates the downstream menu/OSDSS
         * slideshow render (acceptance test) while the faithful gate is isolated. */
        { static long fp = -2; static int done = 0;
          if (fp == -2) { const char *e = getenv("CT952_FORCE_POM");
              fp = e ? (long)strtoull(e, NULL, 0) : -1; }
          if (fp >= 0 && !done && c->pc == 0xa778u &&
              c->icount >= (uint64_t)fp && sparc_get_reg(c, 8) == 0x261ccu) {
              sparc_set_reg(c, 8, 0x2620cu);   /* %o0 = POWERONMENU wrapper */
              done = 1;
              fprintf(stderr, "[FORCEPOM] redirected pump dispatch -> 0x2620c at icount=%llu\n",
                      (unsigned long long)c->icount);
          }
        }
        /* Experiment (CT952_FORCE_OSDSS=<icount>): OSDSS_Monitor(0x591b4) reaches
         * its idle-time compare at 0x59244 (`cmp %o0(idle_ms), %o2(0xe260==58s)`)
         * but the idle never exceeds the threshold (the device is actively running
         * the demo attract slideshow, which is legitimate activity, so the idle
         * timer keeps getting reset). Past <icount>, force %o0 huge at 0x59244 so
         * the idle looks elapsed; combined with __bPOWERONMENUInitial=1 (SET_POM)
         * and clock/alarm==0, OSDSS_Monitor then calls OSDSS_Entry(0x59108) on its
         * own -- the genuine OSDSS screen-saver entry, in-context. */
        { static long fo = -2;
          if (fo == -2) { const char *e = getenv("CT952_FORCE_OSDSS");
              fo = e ? (long)strtoull(e, NULL, 0) : -1; }
          if (fo >= 0 && c->pc == 0x59244u && c->icount >= (uint64_t)fo) {
              sparc_set_reg(c, 8, 0x00ffffffu);   /* %o0 = idle -> huge */
          }
        }
        /* UI-transition tracer (CT952_UITRACE): log every OSD_ChangeUI(0x4a754)
         * call -- %o0=UI index, %o1=mode (0=ENTER,1=EXIT,...), %o7=caller. This
         * directly shows the UI progression during boot and reveals whether the
         * interactive MM-UI (0x12) is ever entered on its own, or the boot parks
         * in the attract/display UI. */
        { static int ut=-1;
          if (ut<0) ut = getenv("CT952_UICHANGE") ? 1 : 0;
          if (ut && c->pc==0x4a754u) {
              fprintf(stderr, "[UICHANGE] ui=%02x mode=%x caller=%08x icount=%llu\n",
                  sparc_get_reg(c,8)&0xff, sparc_get_reg(c,9)&0xff,
                  sparc_get_reg(c,15), (unsigned long long)c->icount);
          }
        }
        /* Pump-arg tracer (CT952_PUMPARG[=<from_icount>]): at the mode-handler
         * dispatch 0xa720 (call %o1), log the message-type arg %o0 and handler
         * %o1 the active-UI handler receives. Reveals the stream of pump messages
         * -- and whether an injected key ever arrives as a non-idle message type
         * (i.e. whether keys reach the interactive dispatch). Rate-limited. */
        { static long pa=-2; static int pn=0;
          if (pa==-2) { const char *e=getenv("CT952_PUMPARG");
              pa = e ? (long)strtoull(e,NULL,0) : -1; }
          if (pa>=0 && c->pc==0xa720u && c->icount>=(uint64_t)pa && pn<400) {
              fprintf(stderr, "[PUMP] arg=%02x handler=%08x icount=%llu\n",
                  sparc_get_reg(c,8)&0xff, sparc_get_reg(c,9),
                  (unsigned long long)c->icount);
              pn++;
          }
          /* Also log the message-record fetch result at 0xa6ec (delay slot after
           * call 0xb090): %o0 = active-UI record ptr (or 0 = none). Shows whether
           * an active UI record even exists to dispatch through. */
          if (pa>=0 && c->pc==0xa6ecu && c->icount>=(uint64_t)pa && pn<400) {
              uint32_t rec = sparc_get_reg(c,8);
              int f=0; uint32_t t=rec&0xff;
              /* rate-limit identical consecutive by low byte via static */
              static uint32_t last=0xdeadbeef; if (t!=(last&0xff)||rec!=last){f=1; last=rec;}
              if (f) { fprintf(stderr, "[PUMPREC] rec=%08x icount=%llu\n",
                  rec, (unsigned long long)c->icount); pn++; }
          }
        }
        { static int ic=-1; static uint32_t seen[64]; static int nseen=0;
          if (ic<0) ic = getenv("CT952_ICALL") ? 1 : 0;
          if (ic && (c->pc==0xa720u || c->pc==0xa778u)) {
              uint32_t tgt = sparc_get_reg(c, c->pc==0xa720u ? 9 : 8);
              int fresh=1; for(int k=0;k<nseen;k++) if(seen[k]==tgt){fresh=0;break;}
              if (fresh && nseen<64) { seen[nseen++]=tgt;
                  fprintf(stderr, "[ICALL] site=%08x target=%08x msgtype=%02x icount=%llu\n",
                      c->pc, tgt, sparc_get_reg(c,16)&0xff,
                      (unsigned long long)c->icount);
              }
          }
        }
        if (pcsamp > 0 && c->icount >= pcs_next) {
            pcs_next = c->icount + (uint64_t)pcsamp;
            fprintf(stderr, "[PCS] pc=%08x o7=%08x sp=%08x icount=%llu\n",
                    c->pc, sparc_get_reg(c, 15), sparc_get_reg(c, 14),
                    (unsigned long long)c->icount);
        }
        /* Backtrace-at-PC (CT952_BTAT=<pc>[,<splo>,<sphi>][;from=<icount>]): when
         * execution reaches <pc> with %fp in [splo,sphi], walk the register-window
         * backtrace ([fp+0x3c]=saved i7, [fp+0x38]=saved fp) to name the caller
         * chain. Prints up to 8 times past <from>. Generalizes STACKW so the MAIN
         * thread's OS_DelayTime park (pc=0x59864) can be attributed to its app
         * caller (INITIAL_System / INITIAL_PowerONStatus / a retry loop). */
        { static long bt = -2; static uint32_t btpc=0, btlo=0x40000000u, bthi=0x40800000u;
          static uint64_t btfrom=0; static int btn=0;
          if (bt == -2) { const char *e = getenv("CT952_BTAT");
              if (e) { char b[128]; strncpy(b,e,127); b[127]=0;
                  char *t=strtok(b,",;"); btpc=t?(uint32_t)strtoul(t,NULL,0):0;
                  t=strtok(NULL,",;"); if(t) btlo=(uint32_t)strtoul(t,NULL,0);
                  t=strtok(NULL,",;"); if(t) bthi=(uint32_t)strtoul(t,NULL,0);
                  const char *f=getenv("CT952_BT_FROM"); btfrom=f?strtoull(f,NULL,0):0;
                  bt = btpc?1:0; } else bt=0; }
          if (bt && c->pc==btpc && btn<8 && c->icount>=btfrom) {
              uint32_t fp = sparc_get_reg(c, 30);
              if (fp>=btlo && fp<bthi) { btn++;
                  fprintf(stderr, "[BT] icount=%llu pc=%08x o7=%08x fp=%08x\n  ",
                      (unsigned long long)c->icount, c->pc,
                      sparc_get_reg(c,15), fp);
                  for (int d=0; d<16 && fp>=0x40000000u && fp<0x40800000u; d++) {
                      int fault=0;
                      uint32_t ret=c->bus->read(c->bus, fp+0x3c, 4, &fault);
                      uint32_t nfp=c->bus->read(c->bus, fp+0x38, 4, &fault);
                      if (fault) break;
                      fprintf(stderr, "%08x ", ret);
                      if (nfp<=fp) break;
                      fp=nfp;
                  }
                  fprintf(stderr, "\n");
              }
          }
        }
        /* Call-trail from 0x5abb4(7) entry (CT952_CALLTRAIL): the instant the
         * main thread reaches the boot-init 0x41a90 call, log every subsequent
         * CALL (pc -> target, %o0 arg) for N calls -- shows exactly how 0x5abb4(7)
         * descends and where it blocks/loops (settles poll-vs-block, §12.69). */
        { static int ct = -1; static int on = 0, nlog = 0;
          if (ct < 0) ct = getenv("CT952_CALLTRAIL") ? 1 : 0;
          if (ct) {
              if (c->pc == 0x41a90u) { on = 1;
                  fprintf(stderr, "[CT] === enter 0x5abb4(o0=%x) icount=%llu ===\n",
                          sparc_get_reg(c, 8), (unsigned long long)c->icount); }
              if (on && nlog < 400) {
                  int fault = 0;
                  uint32_t insn = c->bus->read(c->bus, c->pc, 4, &fault);
                  if (!fault && (insn >> 30) == 1) {   /* CALL */
                      uint32_t disp = insn & 0x3fffffff;
                      if (disp & 0x20000000) disp |= 0xc0000000u;
                      uint32_t tgt = c->pc + (disp << 2);
                      fprintf(stderr, "[CT] %08x call %08x o0=%08x o1=%08x\n",
                              c->pc, tgt, sparc_get_reg(c, 8), sparc_get_reg(c, 9));
                      nlog++;
                  }
              }
          } }
        if (pw) {
            for (int k = 0; k < pwn; k++)
                if (c->pc == pwl[k] && pwh[k] < 40) {
                    pwh[k]++;
                    fprintf(stderr, "[PW] %08x #%u o0=%08x o1=%08x o7=%08x icount=%llu\n",
                            c->pc, pwh[k], sparc_get_reg(c, 8), sparc_get_reg(c, 9),
                            sparc_get_reg(c, 15), (unsigned long long)c->icount);
                }
        }
        if (ftr >= 0 && c->pc == 0x4001dffcu && c->icount >= (uint64_t)ftr && ftn < 400) {
            ftn++;
            fprintf(stderr, "[FLAG] wait obj=%08x mask=%08x i7=%08x icount=%llu\n",
                    sparc_get_reg(c, 8), sparc_get_reg(c, 9), sparc_get_reg(c, 31),
                    (unsigned long long)c->icount);
        }
        if (mbt && c->pc == 0x5969cu && c->icount > mbfrom && mbtn < 2000) {
            mbtn++;
            fprintf(stderr, "[MBOX] get caller o7=%08x arg o0=%08x icount=%llu\n",
                    sparc_get_reg(c, 15), sparc_get_reg(c, 8),
                    (unsigned long long)c->icount);
        }
        /* Stack-unwind at the bounded-wait mbox poll (CT952_STACKW): when the
         * 0xa33d8 wait helper (o7=0xa33fc) polls the CC mbox, walk the SPARC
         * register-window backtrace (saved %i7 at [fp+0x3c], saved %fp at
         * [fp+0x38]) to name the high-level caller chain -- DISP_DisplayCtrl /
         * POWERONMENU_Initial if that is the blocked op. Printed a few times. */
        if (c->pc == 0x5969cu && sparc_get_reg(c, 15) == 0xa33fcu) {
            static int sw = -1, swn = 0; static uint64_t swfloor = 0;
            if (sw < 0) { sw = getenv("CT952_STACKW") ? 1 : 0;
                const char *e = getenv("CT952_STACKW_FROM");
                swfloor = e ? strtoull(e, NULL, 0) : 0; }
            if (sw && swn < 12 && c->icount >= swfloor) {
                swn++;
                fprintf(stderr, "[STACKW] icount=%llu i7=%08x fp=%08x\n  ",
                        (unsigned long long)c->icount,
                        sparc_get_reg(c, 31), sparc_get_reg(c, 30));
                uint32_t fp = sparc_get_reg(c, 30);
                for (int d = 0; d < 14 && fp >= 0x40000000u && fp < 0x40800000u; d++) {
                    int fault = 0;
                    uint32_t ret = c->bus->read(c->bus, fp + 0x3c, 4, &fault);
                    uint32_t nfp = c->bus->read(c->bus, fp + 0x38, 4, &fault);
                    if (fault) break;
                    fprintf(stderr, "%08x ", ret);
                    if (nfp <= fp) break;   /* stacks grow down; stop if not */
                    fp = nfp;
                }
                fprintf(stderr, "\n");
            }
        }
        if (pchit) {
            uint32_t pc = c->pc;
            for (unsigned k = 0; k < sizeof(WL)/sizeof(WL[0]); k++)
                if (pc == WL[k] && hit[k] < 60) {
                    hit[k]++;
                    fprintf(stderr, "[PCHIT] %08x #%u  o7=%08x i7=%08x sp=%08x icount=%llu\n",
                            pc, hit[k], sparc_get_reg(c, 15), sparc_get_reg(c, 31),
                            sparc_get_reg(c, 14), (unsigned long long)c->icount);
                }
        }
        if (step(c)) break;
    }
    return i;
}
