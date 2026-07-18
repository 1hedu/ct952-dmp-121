/*
 * ct952emu -- machine model implementation.
 * Register offsets from ctkav_platform.h (cited per block).
 */
#include "machine.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* LEON core block offsets (ctkav_platform.h:19-97) */
#define R_TIMER1_CNT   0x040
#define R_TIMER1_RLD   0x044
#define R_TIMER1_CTL   0x048
#define R_WATCHDOG     0x04C
#define R_TIMER2_CNT   0x050
#define R_TIMER2_RLD   0x054
#define R_TIMER2_CTL   0x058
#define R_PRESC_CNT    0x060
#define R_PRESC_RLD    0x064
#define R_TIMER3_CTL   0x068
#define R_TIMER3_VAL   0x06C
#define R_UART1_DATA   0x070
#define R_UART1_STAT   0x074
#define R_UART2_DATA   0x080
#define R_UART2_STAT   0x084
#define R_INT_MASK     0x090
#define R_INT_PENDING  0x094
#define R_INT_FORCE    0x098
#define R_INT_CLEAR    0x09C
/* Secondary "PROC1 1st" interrupt controller (ctkav_platform.h:58-64):
 * cascades into LEON interrupt line 13 (INT_NO_PROC1_1ST). Bit0 = VSYNC,
 * the display-timing tick that drives the firmware's display/slideshow
 * state machine (interrupt.c: INT_Proc1_1st_isr -> ISR_DISPSaveClearStatus). */
#define R_P1_1ST_MASK  0x0B0        /* MASK_ENABLE: direct RW enable mask */
#define R_P1_1ST_PEND  0x0B4        /* PENDING: RW; VSYNC source ORs bit0 */
#define R_P1_1ST_STCL  0x0B8        /* STATUS(R) / CLEAR(W1C) */
#define R_P1_1ST_MDIS  0x0BC        /* MASK_DISABLE (W1C into MASK) */
#define INT_NO_PROC1_1ST 13
#define IRQ_P1_1ST_VSYNC 0x1u
#define R_DSU_UART_DATA 0x0C0
#define R_DSU_UART_STAT 0x0C4
/* Command block (ctkav_platform.h:474-481): PARAMETER1 = 0x364 */
#define R_PARAM1       0x364
#define R_PARAM2       0x368
/* AIU GR bank: PROC2_SP GR21 0x7D4, PROC2_STARTADR GR22 0x7D8,
 * audio start/ACK word 0x7E4 (hdecoder.c:702-737) */
#define R_PROC2_SP     0x7D4
#define R_PROC2_START  0x7D8
#define R_AUDIO_CMD    0x7E4

/* Hardware IIC/EEPROM master (undocumented in the headers; the firmware
 * pokes it directly at 0x80004200). The boot config/EEPROM read routine
 * (flash 0x62538) writes a command to 0x4210 -- clears bit0, then sets
 * 0x24 (bit2 = start/trigger, bit5 = mode) -- and spins on bit2 until the
 * hardware clears it, with a ~1000-tick timeout. On real silicon bit2 is
 * a self-clearing "transaction in progress" flag; the store/readback
 * register file would leave it stuck set forever, so we clear it on read
 * (the transaction completes instantly in the model). 0x4204 is the
 * status/result word; a companion routine (flash 0x625b0) reads it and
 * defaults to the 0xAA55 EEPROM signature. */
#define R_IIC_STAT     0x4204
#define R_IIC_CMD      0x4210
#define R_IIC_DATA     0x4214
#define IIC_BUSY       0x4u

#define TIMER_ENABLE   1u
#define TIMER_RELOAD   2u
#define TIMER_LOAD     4u

#define UART_STAT_READY 0x6u   /* TX shift + holding empty, no RX data */
#define UART_STAT_DATA_READY 0x1u   /* RX byte available (ctkav_platform.h) */

static machine_t *M(sparc_bus_t *b) { return (machine_t *)b; }

static uint32_t io_get(machine_t *m, uint32_t off)
{
    return m->io[off / 4];
}

static void io_set(machine_t *m, uint32_t off, uint32_t v)
{
    m->io[off / 4] = v;
}

static void log_access(machine_t *m, uint32_t addr, int is_write,
                       uint32_t val)
{
    int i;
    for (i = 0; i < m->log_n; i++)
        if (m->log[i].addr == addr) {
            if (is_write) { m->log[i].writes++; m->log[i].last_write = val; }
            else m->log[i].reads++;
            return;
        }
    if (m->log_n < MACH_LOG_MAX) {
        m->log[m->log_n].addr = addr;
        m->log[m->log_n].reads = is_write ? 0 : 1;
        m->log[m->log_n].writes = is_write ? 1 : 0;
        m->log[m->log_n].last_write = is_write ? val : 0;
        m->log_n++;
    }
}

static void uart_tx(machine_t *m, int ch, uint32_t v)
{
    (void)ch;
    if (m->uart_echo) {
        fputc((int)(v & 0xFF), stdout);
        fflush(stdout);
    }
    if (m->uart_file)
        fputc((int)(v & 0xFF), m->uart_file);
}

/* ---- I/O page ---- */

static uint32_t io_read(machine_t *m, uint32_t off)
{
    switch (off) {
    case R_UART1_STAT:
        /* TX always ready; RX ready iff bytes are queued (host -> device) */
        return UART_STAT_READY |
               ((m->rx_pos < m->rx_len) ? UART_STAT_DATA_READY : 0u);
    case R_UART2_STAT:
    case R_DSU_UART_STAT:
        return UART_STAT_READY;
    case R_UART1_DATA:
        /* pop one queued RX byte (or 0 if none) */
        if (m->rx_pos < m->rx_len)
            return m->rx_buf[m->rx_pos++];
        return 0;
    case R_UART2_DATA:
    case R_DSU_UART_DATA:
        return 0;                     /* RX not modeled on these ports */
    case R_TIMER3_VAL:
        return (uint32_t)m->t3_value;
    case R_PRESC_CNT:
        return m->presc_cnt;
    case R_INT_PENDING:
        return io_get(m, R_INT_PENDING);
    case R_P1_1ST_STCL:
        /* STATUS read: the live secondary pending register */
        return io_get(m, R_P1_1ST_PEND);
    case R_IIC_CMD:
        /* trigger/busy bit self-clears: transaction done immediately */
        return io_get(m, R_IIC_CMD) & ~IIC_BUSY;
    default:
        log_access(m, 0x80000000u + off, 0, 0);
        return io_get(m, off);
    }
}

/* ---- GPU 2-D engine (ctkav_gpu.h offsets; programming per gdi.c) ---- */
#define R_GPU_CTL0     0x2880
#define R_GPU_CTL1     0x2884
#define R_GPU_COL_NDX  0x2888
#define R_GPU_OP_SIZE  0x288C
#define R_GPU_AG_OFF   0x2890
#define R_GPU_SRC_ADDR 0x2894
#define R_GPU_DEST     0x2898
#define R_GPU_FONT_ADR 0x289C
#define R_GPU_FONT_CFG 0x28A0
#define R_GPU_FONT_IDX 0x28A8
#define GPU_START_BIT  0x2u
#define GPU_STATUS_BIT 0x200u        /* CTL0[9] busy */
#define GPU_FONT_1BIT  0x20u         /* CTL0[5] */

static uint8_t *dram_rw(machine_t *m, uint32_t addr, uint32_t span)
{
    uint32_t off;
    if (addr < 0x40000000u) return NULL;
    off = addr - 0x40000000u;
    if ((uint64_t)off + span > MACH_DRAM_SIZE) return NULL;
    return m->dram + off;
}

/* Execute one GPU op triggered by a CTL0 write with GPU_START. Fill and
 * 1-bit font expansion into the 8bpp OSD plane; the firmware's UI drawing
 * (gdi.c) programs these. Row stride comes from AG_OFF (CT909P encoding:
 * ((ag_width<<8)+ag_offset)<<16, both in 8-byte units, so bytes/row =
 * (ag_width+ag_offset-1)*8). */
static void gpu_exec(machine_t *m, uint32_t ctl0)
{
    uint32_t sz = io_get(m, R_GPU_OP_SIZE);
    uint32_t w = sz & 0xFFFF, h = (sz >> 16) & 0x7FF;
    uint32_t ag = io_get(m, R_GPU_AG_OFF) >> 16;
    uint32_t agw = (ag >> 8) & 0xFF, ago = ag & 0xFF;
    uint32_t stride = (agw + ago > 1) ? (agw + ago - 1) * 8u : 616u;
    uint32_t dest = io_get(m, R_GPU_DEST);
    uint32_t opmode = (ctl0 >> 2) & 0x7;

    m->gpu_ops++;
    if (ctl0 & GPU_FONT_1BIT) m->gpu_font_ops++; else m->gpu_mode_ops[opmode]++;

    if (ctl0 & GPU_FONT_1BIT) {
        /* 1-bit font expansion into the 8bpp OSD plane. Glyph table at
         * FONT_ADDR, each glyph = capacity DWs (glyph_DW*4 bytes/row,
         * MSB-first 1-bit rows). FONT_CONFIG = width_DW<<24 | len<<16 |
         * capacity. COL_NDX low byte = fg index, next byte = bg. Height
         * from OP_SIZE[26:16]; glyph advance = glyph_DW*8 pixels. */
        uint32_t cfg = io_get(m, R_GPU_FONT_CFG);
        uint32_t fbase = io_get(m, R_GPU_FONT_ADR);
        uint32_t wdw = (cfg >> 24) & 0xFF; if (!wdw) wdw = 1;
        uint32_t cap = cfg & 0xFFF;       /* DW per glyph */
        uint32_t gh = h ? h : (cap / wdw);   /* glyph height in rows */
        uint32_t adv = wdw * 8;              /* pixel advance per glyph */
        uint8_t fg = io_get(m, R_GPU_COL_NDX) & 0xFF;
        uint8_t bg = (io_get(m, R_GPU_COL_NDX) >> 8) & 0xFF;
        uint32_t xoff = 0;
        int gi;
        if (!gh) gh = 16;
        for (gi = 0; gi < m->gpu_fontn; gi++) {
            uint32_t gnum = m->gpu_fontq[gi] & 0x1FF;
            uint32_t gaddr = fbase + gnum * cap * 4u;
            const uint8_t *gp = dram_rw(m, gaddr, cap * 4u);
            uint32_t row, col;
            if (!gp) { xoff += adv; continue; }
            for (row = 0; row < gh; row++) {
                for (col = 0; col < adv; col++) {
                    uint32_t bytei = (col >> 3);
                    uint8_t rb = gp[row * wdw * 4u + bytei];
                    uint8_t bit = (rb >> (7 - (col & 7))) & 1;
                    uint32_t px = dest + row * stride + xoff + col;
                    uint8_t *d = dram_rw(m, px, 1);
                    if (d) *d = bit ? fg : bg;
                }
            }
            xoff += adv;
        }
        m->gpu_fontn = 0;
        return;
    }

    if (opmode == 6 && !(ctl0 & GPU_FONT_1BIT)) {   /* GPU_FILLRECTANGLE */
        uint8_t color = (io_get(m, R_GPU_CTL1) >> 24) & 0xFF;
        uint8_t *fb;
        uint32_t r, c;
        if (!w || !h) return;
        fb = dram_rw(m, dest, (h - 1) * stride + w);
        if (!fb) return;
        for (r = 0; r < h; r++)
            for (c = 0; c < w; c++)
                fb[r * stride + c] = color;
    }
    m->gpu_fontn = 0;   /* consume the font-index queue */
}

static void io_write(machine_t *m, uint32_t off, uint32_t v)
{
    switch (off) {
    case R_GPU_CTL0:
        if (v & GPU_START_BIT) gpu_exec(m, v);
        io_set(m, off, v & ~GPU_STATUS_BIT);   /* op completes: clear busy */
        return;
    case R_GPU_FONT_IDX:
        if (m->gpu_fontn < 1024) m->gpu_fontq[m->gpu_fontn++] = (uint16_t)v;
        io_set(m, off, v);
        return;
    case R_UART1_DATA: uart_tx(m, 1, v); return;
    case R_UART2_DATA: uart_tx(m, 2, v); return;
    case R_DSU_UART_DATA: uart_tx(m, 3, v); return;
    case R_TIMER1_CTL:
    case R_TIMER2_CTL: {
        uint32_t cnt_off = (off == R_TIMER1_CTL) ? R_TIMER1_CNT : R_TIMER2_CNT;
        uint32_t rld_off = (off == R_TIMER1_CTL) ? R_TIMER1_RLD : R_TIMER2_RLD;
        io_set(m, off, v);
        if (v & TIMER_LOAD)
            io_set(m, cnt_off, io_get(m, rld_off));
        return;
    }
    case R_INT_CLEAR:
        /* write-1-to-clear pending + force (LEON) */
        io_set(m, R_INT_PENDING, io_get(m, R_INT_PENDING) & ~v);
        io_set(m, R_INT_FORCE, io_get(m, R_INT_FORCE) & ~v);
        return;
    case R_P1_1ST_STCL:
        /* secondary CLEAR: write-1-to-clear pending bits */
        io_set(m, R_P1_1ST_PEND, io_get(m, R_P1_1ST_PEND) & ~v);
        return;
    case R_P1_1ST_MDIS:
        /* secondary MASK_DISABLE: clear the named enable bits */
        io_set(m, R_P1_1ST_MASK, io_get(m, R_P1_1ST_MASK) & ~v);
        return;
    case R_PARAM1:
        /* AM mailbox: PROC1 writes cmd with [31:30]=1 write / 2 read;
         * PROC2 acks by clearing [31:30] (hdecoder.c:1718-1727).
         * Stand-in DSP: ack immediately; reads return 0 via PARAM2.
         * Skipped once the real PROC2 core is running (it acks for real). */
        if (!m->proc2_enable) {
            if ((v >> 30) == 2)
                io_set(m, R_PARAM2, 0);
            io_set(m, R_PARAM1, v & 0x3FFFFFFFu);
            return;
        }
        io_set(m, R_PARAM1, v);
        return;
    case R_AUDIO_CMD:
        /* PROC1 writes 0x10003, then spins reading this word and shifting
         * right 16; it breaks when [31:16] == 0 (hdecoder.c:724-737).
         * PROC2 signals "audio boot OK" by clearing the high half. The
         * stand-in acks instantly; the real PROC2 core does it itself. */
        io_set(m, R_AUDIO_CMD, m->proc2_enable ? v : (v & 0xFFFFu));
        return;
    default:
        log_access(m, 0x80000000u + off, 1, v);
        io_set(m, off, v);
        return;
    }
}

/* ---- bus ---- */

static uint32_t mem_read_raw(const uint8_t *p, int size)
{
    /* big-endian memory image */
    if (size == 1) return p[0];
    if (size == 2) return ((uint32_t)p[0] << 8) | p[1];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void mem_write_raw(uint8_t *p, uint32_t v, int size)
{
    if (size == 1) { p[0] = (uint8_t)v; return; }
    if (size == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* PROC2 vdec command -> completion-state ack (comdec.h EN_VDEC_CMD).
 * The decoder microcode overwrites REG_SRAM_PLAYMODE with these once it
 * has consumed the command; PROC1 wait loops poll for them. */
static uint8_t proc2_ack_of(uint8_t cmd)
{
    switch (cmd) {
    case 0x00: return 0x10;   /* reset/NONE     -> decoder idle=STOP */
    case 0x10: return 0x11;   /* MODE_STOP      -> MODE_STOPPED      */
    case 0x40: return 0x12;   /* MODE_SCAN      -> MODE_SCAN_DONE    */
    case 0x80: return 0x13;   /* MODE_PREDECODE -> MODE_PREDEC_DONE  */
    default:   return cmd;    /* PLAY/...: state stays as set        */
    }
}

static uint32_t bus_rd(machine_t *m, uint32_t addr, int size, int *fault)
{
    *fault = 0;

    /* DSU2 block (0x98000000): PROC1 reads PROC2's live PC here to monitor
     * it (REG_PLAT_DSU2_PC = 0x98080010). Back the PC/nPC; rest reads 0. */
    if (addr >= 0x98000000u && addr < 0x98100000u) {
        if (addr == 0x98080010u) return m->cpu2.pc;
        if (addr == 0x98080014u) return m->cpu2.npc;
        return 0;
    }

    /* PROC2 vdec stand-in: deliver the command ack after a read latency.
     * Skipped once the real PROC2 core is running -- it drives PLAYMODE. */
    if (!m->proc2_enable && addr == 0xB0000190u &&
        m->proc2_ack_countdown > 0 && --m->proc2_ack_countdown == 0)
        m->bram[0x190] = proc2_ack_of(m->proc2_cmd);

    if (addr < MACH_FLASH_MAX) {
        if (addr + (uint32_t)size <= m->flash_size)
            return mem_read_raw(m->flash + addr, size);
        return 0xFFFFFFFFu;   /* erased flash */
    }
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE) {
        if (m->skip_panelcfg && addr == 0x4002f770u)
            return 0xFFFFFFFFu;   /* desc+0x14 = -1: take the skip path */
        return mem_read_raw(m->dram + (addr - 0x40000000u), size);
    }
    if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE)
        return mem_read_raw(m->dram + (addr - 0xC0000000u), size);
    if (addr >= 0x80000000u && addr < 0x80000000u + MACH_IO_SIZE) {
        uint32_t off = (addr - 0x80000000u) & ~3u;
        uint32_t v = io_read(m, off);
        if (size == 4) return v;
        /* sub-word I/O read: extract big-endian lane */
        if (size == 1) return (v >> ((3 - (addr & 3)) * 8)) & 0xFF;
        return (v >> ((addr & 2) ? 0 : 16)) & 0xFFFF;
    }
    if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u)
        return mem_read_raw(m->bram + (addr - 0xB0000000u), size);
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return 0;                        /* DSU stub */
    if (addr >= 0xA0000000u && addr < 0xA0010000u) {
        log_access(m, addr & ~3u, 0, 0);
        return 0;                        /* FCR/SDC/NFC stub */
    }
    m->unmapped_reads++;
    log_access(m, addr & ~3u, 0, 0);   /* record so it names the blocker */
    return 0;
}

/* boot PROC2: seed the second core at the entry/SP PROC1 staged in the AIU GR
 * bank (GR22 = 0x800007d8 start, GR21 = 0x800007d4 SP) and let it run. */
static void proc2_boot(machine_t *m)
{
    uint32_t entry = io_get(m, R_PROC2_START);
    uint32_t sp    = io_get(m, R_PROC2_SP);
    if (m->proc2_on || !m->proc2_enable || (entry & 0xF0000000u) != 0x40000000u)
        return;
    sparc_reset(&m->cpu2, &m->bus2);
    m->cpu2.pc = entry;
    m->cpu2.npc = entry + 4;
    sparc_set_reg(&m->cpu2, 14, sp);   /* %o6 / %sp */
    m->proc2_on = 1;
}

static void proc2_halt(machine_t *m)
{
    m->proc2_on = 0;
}

static void bus_wr(machine_t *m, uint32_t addr, uint32_t val,
                   int size, int *fault)
{
    *fault = 0;

    /* PROC2 reset/debug control (writes from PROC1) */
    if (m->proc2_enable) {
        int was = m->proc2_on;
        if (addr == 0x80000304u && (val & 0x1u)) { proc2_boot(m); }  /* RESET_DISABLE: release */
        else if (addr == 0x80000324u && (val & 0x1u)) { proc2_halt(m); } /* RESET_ENABLE: hold */
        else if (addr == 0x98000000u) {              /* DSU2 control */
            if (val & 0x00080000u) proc2_boot(m);    /* PLAT_DSU_CTL_RE */
            else if (val & 0x000000A0u) proc2_halt(m); /* BN|BW: break */
        }
        if (was != m->proc2_on && getenv("CT952_TRACE"))
            fprintf(stderr, "[P2] %s via %08x=%08x entry=%08x pc1=%08x\n",
                    m->proc2_on ? "BOOT" : "HALT", addr, val,
                    io_get(m, R_PROC2_START), m->cpu.pc);
    }
    if (addr >= 0x98000000u && addr < 0x98100000u)
        return;   /* DSU2 register file: writes accepted, not modeled */

    /* PROC1 issued a VDEC command via REG_SRAM_PLAYMODE: arm the PROC2 ack
     * (delivered after a few status polls, mimicking the microcode latency). */
    if (addr == 0xB0000190u) {
        m->proc2_cmd = (uint8_t)val;
        m->proc2_ack_countdown = (proc2_ack_of((uint8_t)val) != (uint8_t)val)
                                 ? 8 : 0;
    }

    if (addr < MACH_FLASH_MAX)
        return;                          /* XIP flash: ignore writes */
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE) {
        mem_write_raw(m->dram + (addr - 0x40000000u), val, size);
        return;
    }
    if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE) {
        mem_write_raw(m->dram + (addr - 0xC0000000u), val, size);
        return;
    }
    if (addr >= 0x80000000u && addr < 0x80000000u + MACH_IO_SIZE) {
        uint32_t off = (addr - 0x80000000u) & ~3u;
        if (size != 4) {
            /* merge sub-word write into the 32-bit register */
            uint32_t cur = io_get(m, off);
            uint32_t sh = (size == 1) ? (3 - (addr & 3)) * 8
                                      : ((addr & 2) ? 0 : 16);
            uint32_t mask = (size == 1) ? 0xFFu : 0xFFFFu;
            val = (cur & ~(mask << sh)) | ((val & mask) << sh);
        }
        io_write(m, off, val);
        return;
    }
    if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u) {
        mem_write_raw(m->bram + (addr - 0xB0000000u), val, size);
        return;
    }
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return;
    if (addr >= 0xA0000000u && addr < 0xA0010000u) {
        log_access(m, addr & ~3u, 1, val);
        return;
    }
    m->unmapped_writes++;
}

/* ---- bus dispatch: two views onto the same machine ---- */
/* cpu1's bus is machine.bus (first field -> plain cast). cpu2's bus is
 * machine.bus2 (container-of by offset). Both hit the same memory/I/O; only
 * the interrupt wiring differs. */
static machine_t *M2(sparc_bus_t *b)
{ return (machine_t *)((char *)b - offsetof(machine_t, bus2)); }

static uint32_t bus_read(sparc_bus_t *b, uint32_t a, int s, int *f)
{ return bus_rd(M(b), a, s, f); }
static void bus_write(sparc_bus_t *b, uint32_t a, uint32_t v, int s, int *f)
{ bus_wr(M(b), a, v, s, f); }
static uint32_t bus2_read(sparc_bus_t *b, uint32_t a, int s, int *f)
{ return bus_rd(M2(b), a, s, f); }
static void bus2_write(sparc_bus_t *b, uint32_t a, uint32_t v, int s, int *f)
{ bus_wr(M2(b), a, v, s, f); }
/* PROC2 has its own interrupt controller; we don't wire it yet -- the
 * decoder microcode drives the datapath by polling, so run it with no
 * asynchronous interrupts rather than misdelivering PROC1's. */
static int  bus2_irq_level(sparc_bus_t *b) { (void)b; return 0; }
static void bus2_irq_ack(sparc_bus_t *b, int lvl) { (void)b; (void)lvl; }

static int bus_irq_level(sparc_bus_t *b)
{
    machine_t *m = M(b);
    uint32_t pend = io_get(m, R_INT_PENDING) | io_get(m, R_INT_FORCE);
    /* cascade: the secondary PROC1-1st controller drives LEON line 13
     * whenever any of its enabled sources is pending (level-triggered). */
    if (io_get(m, R_P1_1ST_PEND) & io_get(m, R_P1_1ST_MASK))
        pend |= (1u << INT_NO_PROC1_1ST);
    uint32_t eff = pend & io_get(m, R_INT_MASK);
    int lvl;
    for (lvl = 15; lvl >= 1; lvl--)
        if (eff & (1u << lvl))
            return lvl;
    return 0;
}

static void bus_irq_ack(sparc_bus_t *b, int level)
{
    machine_t *m = M(b);
    io_set(m, R_INT_PENDING, io_get(m, R_INT_PENDING) & ~(1u << level));
    io_set(m, R_INT_FORCE, io_get(m, R_INT_FORCE) & ~(1u << level));
}

/* ---- timers: 1 cycle per instruction ---- */

static void timer_tick_one(machine_t *m, uint32_t cnt_off, uint32_t rld_off,
                           uint32_t ctl_off, uint32_t irq_bit)
{
    uint32_t ctl = io_get(m, ctl_off);
    uint32_t cnt;
    if (!(ctl & TIMER_ENABLE)) return;
    cnt = io_get(m, cnt_off);
    if (cnt == 0) {
        if (ctl & TIMER_RELOAD)
            io_set(m, cnt_off, io_get(m, rld_off));
        io_set(m, R_INT_PENDING, io_get(m, R_INT_PENDING) | irq_bit);
    } else {
        io_set(m, cnt_off, cnt - 1);
    }
}

static void machine_cycle(machine_t *m)
{
    m->cycles++;
    /* Display VSYNC tick: raise the secondary VSYNC-pending bit at the
     * panel field rate so the firmware's display state machine advances.
     * Real timing is ~MCLK/50Hz (~2.66M cycles); we use a shorter, env-
     * tunable divider so many fields elapse within a bring-up run. */
    if (++m->vsync_cnt >= m->vsync_div) {
        m->vsync_cnt = 0;
        io_set(m, R_P1_1ST_PEND,
               io_get(m, R_P1_1ST_PEND) | IRQ_P1_1ST_VSYNC);
    }
    if (m->presc_cnt == 0) {
        m->presc_cnt = io_get(m, R_PRESC_RLD);
        m->t3_value++;
        timer_tick_one(m, R_TIMER1_CNT, R_TIMER1_RLD, R_TIMER1_CTL,
                       0x100u);   /* INT_TIMER1 */
        timer_tick_one(m, R_TIMER2_CNT, R_TIMER2_RLD, R_TIMER2_CTL,
                       0x200u);   /* INT_TIMER2 */
        /* watchdog counts on the same tick; 0 means untouched/disabled
         * here (real hw needs SYSCFG enable; we only fire if armed) */
        {
            uint32_t wd = io_get(m, R_WATCHDOG);
            if (wd > 1)
                io_set(m, R_WATCHDOG, wd - 1);
            else if (wd == 1)
                m->watchdog_fired = 1;
        }
    } else {
        m->presc_cnt--;
    }
}

int machine_init(machine_t *m, const uint8_t *flash, uint32_t flash_size)
{
    memset(m, 0, sizeof(*m));
    if (flash_size > MACH_FLASH_MAX)
        return -1;
    m->flash = (uint8_t *)malloc(MACH_FLASH_MAX);
    m->dram = (uint8_t *)malloc(MACH_DRAM_SIZE);
    m->bram = (uint8_t *)malloc(0x10000u);
    if (!m->flash || !m->dram || !m->bram)
        return -1;
    memset(m->flash, 0xFF, MACH_FLASH_MAX);
    memcpy(m->flash, flash, flash_size);
    memset(m->dram, 0, MACH_DRAM_SIZE);
    memset(m->bram, 0, 0x10000u);
    m->flash_size = flash_size;
    m->uart_echo = 1;

    /* display field-rate divider for the VSYNC IRQ (see machine_cycle) */
    {
        const char *e = getenv("CT952_VSYNC_DIV");
        m->vsync_cnt = 0;
        m->vsync_div = e ? (uint32_t)strtoul(e, NULL, 0) : 200000u;
        if (m->vsync_div == 0) m->vsync_div = 200000u;
    }

    /* SYSTEM_CONFIGURATION1 (0x8000031c): hardware strapping the boot code
     * decodes for DRAM/flash type. Bits[4:0] must be 0b11xxx or the AP
     * code-area calc (flash 0x40260) returns the 0x50000000 "unknown DRAM"
     * sentinel and boot-config aborts. Overridable via CT952_SYSCFG1 for
     * bring-up sweeps. */
    {
        const char *e = getenv("CT952_SYSCFG1");
        /* 0x1e -> AP-calc returns 0x40800000 (8 MB / 64 Mbit DRAM top),
         * matching this model's DRAM size. */
        m->io[0x31c / 4] = e ? (uint32_t)strtoul(e, NULL, 0) : 0x1eu;
    }

    m->bus.read = bus_read;
    m->bus.write = bus_write;
    m->bus.irq_level = bus_irq_level;
    m->bus.irq_ack = bus_irq_ack;
    sparc_reset(&m->cpu, &m->bus);

    /* PROC2 second core (gated during bring-up) */
    m->bus2.read = bus2_read;
    m->bus2.write = bus2_write;
    m->bus2.irq_level = bus2_irq_level;
    m->bus2.irq_ack = bus2_irq_ack;
    m->proc2_enable = getenv("CT952_PROC2") ? 1 : 0;
    m->proc2_on = 0;
    sparc_reset(&m->cpu2, &m->bus2);
    m->cpu2.halted = 1;      /* idle until PROC1 releases it */
    return 0;
}

void machine_seed_boot(machine_t *m, uint32_t entry, uint32_t sp)
{
    if (entry) io_set(m, R_PROC2_START, entry);
    if (sp)    io_set(m, R_PROC2_SP, sp);
}

void machine_uart_feed(machine_t *m, const uint8_t *data, uint32_t len)
{
    uint8_t *nb = (uint8_t *)realloc(m->rx_buf, m->rx_len + len);
    if (!nb) return;
    m->rx_buf = nb;
    memcpy(m->rx_buf + m->rx_len, data, len);
    m->rx_len += len;
}

void machine_free(machine_t *m)
{
    if (getenv("CT952_TRACE"))
        fprintf(stderr, "[EXIT] pc1=%08x  PROC2 on=%d pc=%08x icount=%llu "
                "halted=%d (%s)\n", m->cpu.pc, m->proc2_on, m->cpu2.pc,
                (unsigned long long)m->cpu2.icount, m->cpu2.halted,
                m->cpu2.halt_reason[0] ? m->cpu2.halt_reason : "-");
    free(m->flash);
    free(m->dram);
    free(m->bram);
    free(m->rx_buf);
    m->flash = NULL;
    m->dram = NULL;
    m->bram = NULL;
    m->rx_buf = NULL;
}

uint64_t machine_run(machine_t *m, uint64_t n)
{
    uint64_t done = 0;
    while (done < n && !m->cpu.halted && !m->watchdog_fired) {
        uint64_t chunk = n - done;
        uint64_t ran, i;
        /* Faithful panel-config build (opt-in): at the first fetch of the
         * config thunk (flash 0x3d564), run the firmware's own descriptor
         * builder (0x3ce60) on the live machine -- it reads the real SETD
         * settings sector -- preserving the boot CPU context across the
         * call. Reproduces the default-init pass the eCos init-callback
         * list would run before the apply. NOTE: builds the descriptor
         * (desc+0x10/0x14 from SETD) but is not yet sufficient on its own
         * -- the inner register table at 0x40042000 is populated by the
         * apply itself, which is the next layer. */
        if (m->build_panelcfg && !m->panelcfg_built) {
            if (m->cpu.pc == 0x3d564u) {
                sparc_t save = m->cpu;
                machine_call(m, 0x3ce60u, 0, 0, 0, 0x40700000u, 50000000ull);
                m->cpu = save;
                m->panelcfg_built = 1;
            } else {
                chunk = 1;   /* single-step until the thunk is reached */
            }
        }
        if (chunk > 4096) chunk = 4096;
        ran = sparc_run(&m->cpu, chunk);
        done += ran;
        for (i = 0; i < ran; i++)
            machine_cycle(m);
        /* Interleave PROC2 on the same wall-clock budget. It shares the bus
         * (DRAM / vdec SRAM / JPU), so its decode work is visible to PROC1. */
        if (m->proc2_on && !m->cpu2.halted)
            sparc_run(&m->cpu2, ran ? ran : chunk);
        if (ran < chunk)
            break;
    }
    return done;
}

#define CALL_SENTINEL 0xE0000000u

int machine_call(machine_t *m, uint32_t entry,
                 uint32_t a0, uint32_t a1, uint32_t a2,
                 uint32_t sp, uint64_t budget)
{
    sparc_t *c = &m->cpu;
    uint64_t i;

    /* Trap-free environment: S=1, ET=0, PIL=15, CWP=0; WIM=0 so save/
     * restore just rotate windows (no overflow/underflow traps). */
    c->halted = 0;
    c->psr = 0xF3000F00u | PSR_S;    /* impl/ver | PIL=15 | S, ET=0 */
    c->wim = 0;
    c->pc = entry;
    c->npc = entry + 4;
    sparc_set_reg(c, 8, a0);          /* %o0 */
    sparc_set_reg(c, 9, a1);          /* %o1 */
    sparc_set_reg(c, 10, a2);         /* %o2 */
    sparc_set_reg(c, 11, 0);          /* %o3 (extra args -> 0/NULL) */
    sparc_set_reg(c, 12, 0);          /* %o4 */
    sparc_set_reg(c, 13, 0);          /* %o5 */
    sparc_set_reg(c, 14, sp);         /* %o6 / %sp */
    sparc_set_reg(c, 15, CALL_SENTINEL - 8); /* %o7: retl -> sentinel */

    for (i = 0; i < budget; i++) {
        if (c->pc == CALL_SENTINEL) return 0;
        if (c->halted) return -1;
        sparc_run(c, 1);
    }
    return -2;
}

uint32_t machine_dram_rd(machine_t *m, uint32_t addr, int size)
{
    int f = 0;
    return bus_read(&m->bus, addr, size, &f);
}

uint8_t *machine_dram_ptr(machine_t *m, uint32_t addr)
{
    if (addr >= 0x40000000u && addr < 0x40000000u + MACH_DRAM_SIZE)
        return m->dram + (addr - 0x40000000u);
    return NULL;
}

/* ---- Stock-ROM section loader (mask-ROM equivalent) ---- */

#define UZIP_DECODE  0x00002C50u   /* UZIP blob @ flash 0x2000, wrapper +0xc50 */
#define UZIP_WORKMEM 0x40400000u   /* decompress scratch (clear of sections) */
#define UZIP_SP      0x40700000u

static uint32_t flash_be32(machine_t *m, uint32_t off)
{
    if (off + 4 > m->flash_size) return 0;
    return ((uint32_t)m->flash[off] << 24) | ((uint32_t)m->flash[off+1] << 16) |
           ((uint32_t)m->flash[off+2] << 8) | m->flash[off+3];
}

/* One load pass. only_data: 0 = load everything except DATA, 1 = only
 * DATA. DATA is applied last so it wins its overlap with SFAT (the VSR
 * table at DATA's base must be authoritative). */
static uint32_t rom_load_pass(machine_t *m, FILE *log, int only_data)
{
    uint32_t off = 0x10, romv_entry = 0;

    while (off + 24 <= m->flash_size) {
        char nm[5];
        uint32_t lma, rma, lsz, rsz;
        int i, printable = 1, is_data;
        for (i = 0; i < 4; i++) {
            nm[i] = (char)m->flash[off + i];
            if (nm[i] < 32 || nm[i] > 126) printable = 0;
        }
        nm[4] = 0;
        if (!printable) break;
        lma = flash_be32(m, off + 4);  rma = flash_be32(m, off + 8);
        lsz = flash_be32(m, off + 12); rsz = flash_be32(m, off + 16);
        off += 24;

        is_data = !memcmp(nm, "DATA", 4);
        if (is_data != only_data)
            continue;

        if (lma < 0x40000000u || lma + lsz > 0x40000000u + MACH_DRAM_SIZE) {
            if (log) fprintf(log, "  %-4s %10x %9x %9x %8x  (flash/skip)\n",
                             nm, lma, rma, lsz, rsz);
            continue;   /* flash-XIP or non-DRAM: nothing to stage */
        }
        if (!memcmp(nm, "ROMV", 4)) romv_entry = lma;

        if (rsz < lsz) {
            /* zipped: run the firmware's decompressor */
            int rc = machine_call(m, UZIP_DECODE, rma, lma, UZIP_WORKMEM,
                                  UZIP_SP, 200000000ull);
            if (log) fprintf(log, "  %-4s %10x %9x %9x %8x  UNZIP rc=%d\n",
                             nm, lma, rma, lsz, rsz, rc);
            if (rc != 0) return 0;
        } else {
            /* raw: copy flash -> DRAM */
            uint8_t *dst = machine_dram_ptr(m, lma);
            if (dst && rma + lsz <= m->flash_size)
                memcpy(dst, m->flash + rma, lsz);
            if (log) fprintf(log, "  %-4s %10x %9x %9x %8x  copy\n",
                             nm, lma, rma, lsz, rsz);
        }
    }
    return romv_entry;
}

uint32_t machine_rom_load(machine_t *m, FILE *log)
{
    uint32_t entry;
    if (log) fprintf(log, "# section  run       flash     unpacked  packed   action\n");
    rom_load_pass(m, log, 0);              /* everything except DATA */
    entry = rom_load_pass(m, log, 1);      /* DATA last (wins SFAT overlap) */
    /* entry (ROMV) came from pass 0; recover it */
    if (!entry) {
        /* ROMV is loaded in pass 0; re-scan for its LMA */
        uint32_t off = 0x10;
        while (off + 24 <= m->flash_size) {
            if (m->flash[off] < 32 || m->flash[off] > 126) break;
            if (!memcmp(m->flash + off, "ROMV", 4)) {
                entry = flash_be32(m, off + 4); break;
            }
            off += 24;
        }
    }
    sparc_reset(&m->cpu, &m->bus);   /* clean boot state; DRAM preserved */
    return entry;
}

static int cmp_log(const void *a, const void *b)
{
    const mach_logent_t *x = (const mach_logent_t *)a;
    const mach_logent_t *y = (const mach_logent_t *)b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

void machine_dump_iolog(machine_t *m, FILE *f)
{
    int i;
    qsort(m->log, (size_t)m->log_n, sizeof(m->log[0]), cmp_log);
    fprintf(f, "# unmodeled I/O access inventory (%d unique regs)\n",
            m->log_n);
    fprintf(f, "# addr        reads   writes  last_write\n");
    for (i = 0; i < m->log_n; i++)
        fprintf(f, "0x%08x  %6u  %6u  0x%08x\n",
                m->log[i].addr, m->log[i].reads, m->log[i].writes,
                m->log[i].last_write);
    fprintf(f, "# unmapped: %u reads, %u writes\n",
            m->unmapped_reads, m->unmapped_writes);
}

/* ---- DISP display engine: OSD plane scanout ------------------------ *
 * The stock display path is a blob (display.a), but the OSD plane it
 * scans is fully described by header-visible state: the 8bpp palette-
 * indexed pixels live linearly in DRAM (the firmware's OSD region,
 * DS_OSDFRAME_ST = 0x4005F000), and the colour palette is the DISP
 * GAM_OSD RAM at 0x80001C00 -- 256 words of 0x00YYUUVV, BT.601 studio
 * range (jrgb2yuv.c). This composites that plane to an RGB PPM, exactly
 * what the DISP scan-out does before the panel TCON. Register offsets
 * from ctkav_disp.h. */
#define R_DISP_OSD_SIZE  0x1A54          /* bit28 = DISP_OSD_EN */
#define R_DISP_GAM_OSD   0x1C00          /* GAM_OSD[n] = +n*4, 256 entries */
#define DISP_OSD_EN      0x10000000u

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* BT.601 studio-range YCbCr (0x00YYUUVV) -> packed 0x00RRGGBB: the exact
 * inverse of the SDK's jup_argb_to_yuv, so a colour loaded into the OSD
 * palette scans back out to its original ARGB. */
static uint32_t disp_yuv_to_rgb(uint32_t yuv)
{
    int y = (int)((yuv >> 16) & 0xFF);
    int u = (int)((yuv >> 8) & 0xFF);
    int v = (int)(yuv & 0xFF);
    int c = y - 16, d = u - 128, e = v - 128;
    int r = clamp8((298 * c + 409 * e + 128) >> 8);
    int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    int b = clamp8((298 * c + 516 * d + 128) >> 8);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int machine_disp_scanout(machine_t *m, uint32_t osd_base,
                         uint32_t w, uint32_t h, uint32_t stride,
                         const char *ppm_path)
{
    uint32_t pal[256];
    const uint8_t *fb;
    uint64_t span;
    FILE *f;
    uint32_t x, y;
    int osd_en, i;

    {
        int loaded = 0;
        for (i = 0; i < 256; i++) {
            pal[i] = disp_yuv_to_rgb(io_get(m, R_DISP_GAM_OSD + (uint32_t)i * 4));
            if (i && pal[i]) loaded = 1;
        }
        /* if the firmware hasn't loaded the OSD palette RAM yet, fall back
         * to a visible per-index ramp so drawn content stays legible */
        if (!loaded)
            for (i = 0; i < 256; i++) {
                uint32_t gr = i ? (uint32_t)((i * 40 + 40) & 0xFF) : 0u;
                pal[i] = (gr << 16) | (gr << 8) | gr;
            }
    }

    osd_en = (io_get(m, R_DISP_OSD_SIZE) & DISP_OSD_EN) != 0;

    if (osd_base < 0x40000000u) return -1;
    span = (uint64_t)(h ? h - 1 : 0) * stride + w;
    if ((uint64_t)(osd_base - 0x40000000u) + span > MACH_DRAM_SIZE) return -1;
    fb = machine_dram_ptr(m, osd_base);
    if (!fb) return -1;

    f = fopen(ppm_path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    /* render the OSD plane content regardless of the hardware enable bit
     * (the firmware draws before flipping enable); enable state is still
     * reported via the return value */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t c = pal[fb[(uint64_t)y * stride + x]];
            fputc((int)((c >> 16) & 0xFF), f);
            fputc((int)((c >> 8) & 0xFF), f);
            fputc((int)(c & 0xFF), f);
        }
    fclose(f);
    return osd_en ? 0 : 1;
}
