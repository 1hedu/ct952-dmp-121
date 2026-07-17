/*
 * ct952emu -- machine model implementation.
 * Register offsets from ctkav_platform.h (cited per block).
 */
#include "machine.h"
#include <stdlib.h>
#include <string.h>

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

#define TIMER_ENABLE   1u
#define TIMER_RELOAD   2u
#define TIMER_LOAD     4u

#define UART_STAT_READY 0x6u   /* TX shift + holding empty, no RX data */

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
    case R_UART2_STAT:
    case R_DSU_UART_STAT:
        return UART_STAT_READY;
    case R_UART1_DATA:
    case R_UART2_DATA:
    case R_DSU_UART_DATA:
        return 0;                     /* no RX modeled yet */
    case R_TIMER3_VAL:
        return (uint32_t)m->t3_value;
    case R_PRESC_CNT:
        return m->presc_cnt;
    case R_INT_PENDING:
        return io_get(m, R_INT_PENDING);
    default:
        log_access(m, 0x80000000u + off, 0, 0);
        return io_get(m, off);
    }
}

static void io_write(machine_t *m, uint32_t off, uint32_t v)
{
    switch (off) {
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
    case R_PARAM1:
        /* AM mailbox: PROC1 writes cmd with [31:30]=1 write / 2 read;
         * PROC2 acks by clearing [31:30] (hdecoder.c:1718-1727).
         * Stand-in DSP: ack immediately; reads return 0 via PARAM2. */
        if ((v >> 30) == 2)
            io_set(m, R_PARAM2, 0);
        io_set(m, R_PARAM1, v & 0x3FFFFFFFu);
        return;
    case R_AUDIO_CMD:
        /* PROC1 writes 0x10003 then polls [31:16] for the DSP boot
         * ack (hdecoder.c:714-737). Stand-in: ack instantly. */
        io_set(m, R_AUDIO_CMD, (v & 0xFFFFu) | 0x00010000u);
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

static uint32_t bus_read(sparc_bus_t *b, uint32_t addr, int size, int *fault)
{
    machine_t *m = M(b);
    *fault = 0;

    if (addr < MACH_FLASH_MAX) {
        if (addr + (uint32_t)size <= m->flash_size)
            return mem_read_raw(m->flash + addr, size);
        return 0xFFFFFFFFu;   /* erased flash */
    }
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE)
        return mem_read_raw(m->dram + (addr - 0x40000000u), size);
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
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return 0;                        /* DSU stub */
    if (addr >= 0xA0000000u && addr < 0xA0010000u) {
        log_access(m, addr & ~3u, 0, 0);
        return 0;                        /* FCR/SDC/NFC stub */
    }
    m->unmapped_reads++;
    return 0;
}

static void bus_write(sparc_bus_t *b, uint32_t addr, uint32_t val,
                      int size, int *fault)
{
    machine_t *m = M(b);
    *fault = 0;

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
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return;
    if (addr >= 0xA0000000u && addr < 0xA0010000u) {
        log_access(m, addr & ~3u, 1, val);
        return;
    }
    m->unmapped_writes++;
}

static int bus_irq_level(sparc_bus_t *b)
{
    machine_t *m = M(b);
    uint32_t eff = (io_get(m, R_INT_PENDING) | io_get(m, R_INT_FORCE)) &
                   io_get(m, R_INT_MASK);
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
    if (!m->flash || !m->dram)
        return -1;
    memset(m->flash, 0xFF, MACH_FLASH_MAX);
    memcpy(m->flash, flash, flash_size);
    memset(m->dram, 0, MACH_DRAM_SIZE);
    m->flash_size = flash_size;
    m->uart_echo = 1;

    m->bus.read = bus_read;
    m->bus.write = bus_write;
    m->bus.irq_level = bus_irq_level;
    m->bus.irq_ack = bus_irq_ack;
    sparc_reset(&m->cpu, &m->bus);
    return 0;
}

void machine_seed_boot(machine_t *m, uint32_t entry, uint32_t sp)
{
    if (entry) io_set(m, R_PROC2_START, entry);
    if (sp)    io_set(m, R_PROC2_SP, sp);
}

void machine_free(machine_t *m)
{
    free(m->flash);
    free(m->dram);
    m->flash = NULL;
    m->dram = NULL;
}

uint64_t machine_run(machine_t *m, uint64_t n)
{
    uint64_t done = 0;
    while (done < n && !m->cpu.halted && !m->watchdog_fired) {
        uint64_t chunk = n - done;
        uint64_t ran, i;
        if (chunk > 4096) chunk = 4096;
        ran = sparc_run(&m->cpu, chunk);
        done += ran;
        for (i = 0; i < ran; i++)
            machine_cycle(m);
        if (ran < chunk)
            break;
    }
    return done;
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
