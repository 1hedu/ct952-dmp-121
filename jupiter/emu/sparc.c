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
    /* impl=0xF ver=3 (LEON-ish), S=1, ET=0, CWP=0 */
    c->psr = 0xF3000000u | PSR_S;
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
    c->icount++;

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

uint64_t sparc_run(sparc_t *c, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        if (c->halted) break;
        if (step(c)) break;
    }
    return i;
}
