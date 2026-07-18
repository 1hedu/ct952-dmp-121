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

/* Serial (SPI) flash controller (ctkav_platform.h PROM block, CT909P
 * offsets; spflash.c drives it). Bulk data reads go through the
 * memory-mapped ASI-0x7 window (handled as flash in bus_read); only the
 * JEDEC/device-ID + status handshake goes through these registers. */
#define R_SPI_CMD      0x2A24   /* command byte written here */
#define R_SPI_OP       0x2A28   /* format write / status read */
#define R_SPI_RD       0x2A34   /* read-data result */
#define SPI_IDLE       0x0200u
#define SPI_IOR        0x0400u
#define SPI_IOW        0x0800u
#define SPI_WAITCMD    0x1000u
#define SPI_DONE       (SPI_IDLE | SPI_IOR | SPI_IOW | SPI_WAITCMD)
/* MX25L1605 -- a 2 MB serial flash; the device dump is 2 MB, and the
 * firmware matches (RD_REG & 0xffff) == 0xC214 via the 0x90 read-ID cmd
 * (spflash.c _SPF_ReadID, MX25L1605). Manufacturer 0xC2, device 0x14. */
#define SPI_ID_MXIC    0xC214u

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
    case R_IIC_CMD:
        /* trigger/busy bit self-clears: transaction done immediately */
        return io_get(m, R_IIC_CMD) & ~IIC_BUSY;
    case R_SPI_OP:
        /* every command completes instantly: all state bits ready */
        return io_get(m, R_SPI_OP) | SPI_DONE;
    case R_SPI_RD:
        return m->spi_rd;
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
    case R_SPI_CMD: {
        /* Latch the read-data result the firmware will pull from RD_REG.
         * cmd byte is the low 8 bits (ID/status reads); bulk data reads
         * don't come through here. spflash.c _SPF_ReadID tries 0x9F/0x90/
         * 0xAB; the 0x90 "read manuf/device ID" is the one that matches
         * (RD_REG & 0xffff == 0xC214 -> MX25L1605). Read-status (0x05)
         * returns 0 = not busy (WIP clear). */
        uint8_t cmd = (uint8_t)(v & 0xFF);
        switch (cmd) {
        case 0x90: m->spi_rd = SPI_ID_MXIC;       break; /* manuf+device */
        case 0x9F: m->spi_rd = SPI_ID_MXIC >> 8;  break; /* JEDEC 1st byte */
        case 0x05: m->spi_rd = 0x00;              break; /* status: ready */
        default:   m->spi_rd = 0x00;              break;
        }
        io_set(m, R_SPI_CMD, v);
        return;
    }
    case R_AUDIO_CMD:
        /* PROC1 writes 0x10003, then spins reading this word and shifting
         * right 16; it breaks when [31:16] == 0 (hdecoder.c:724-737).
         * PROC2 signals "audio boot OK" by clearing the high half. Our
         * stand-in DSP acks instantly: keep the low 16 (audio type),
         * clear [31:16]. */
        io_set(m, R_AUDIO_CMD, v & 0xFFFFu);
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
        /* FCR/SDC/NFC stub. absent_ff: float the bus like real silicon
         * with no card/media attached (many present-detect bits are
         * active-low), instead of reading as all-zeros. */
        return m->absent_ff ? 0xFFFFFFFFu : 0;
    }
    if (addr >= 0xB0000000u && addr < 0xB0010000u)
        return mem_read_raw(m->sram + (addr - 0xB0000000u), size);
    m->unmapped_reads++;
    log_access(m, addr & ~3u, 0, 0);   /* record so it names the blocker */
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
        if (m->watch_left > 0 && addr >= m->watch_lo && addr < m->watch_hi) {
            fprintf(stderr, "[watch] pc=0x%08x wrote [0x%08x] = 0x%08x (%dB)\n",
                    m->cpu.pc, addr, val, size);
            m->watch_left--;
        }
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
    if (addr >= 0xB0000000u && addr < 0xB0010000u) {
        mem_write_raw(m->sram + (addr - 0xB0000000u), val, size);
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
    m->sram = (uint8_t *)malloc(0x10000);   /* 0xB0000000 scratch SRAM */
    if (!m->flash || !m->dram || !m->sram)
        return -1;
    memset(m->flash, 0xFF, MACH_FLASH_MAX);
    memcpy(m->flash, flash, flash_size);
    memset(m->dram, 0, MACH_DRAM_SIZE);
    memset(m->sram, 0, 0x10000);
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
    free(m->sram);
    m->flash = NULL;
    m->dram = NULL;
    m->sram = NULL;
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

/* ---- Display-engine scanout ----
 * Reproduce what the CT952 OSD read-channel would put on screen, from
 * the real registers any code (firmware or an SDK demo) programs:
 *   REG_MCU_VCR20   0x80000D80  OSD read-channel base (DRAM byte addr)
 *   REG_DISP_OSD_SIZE 0x80001A54  bit28 = OSD enable
 *   GAM_OSD RAM     0x80001C00..  256 palette entries, [23:0] = YCbCr
 *                                 (Y<<16|Cb<<8|Cr, BT.601 studio range),
 *                                 bit24 = per-entry mix enable.
 * The 8bpp OSD plane at the base is resolved through that palette and
 * YCbCr->RGB converted (host-side float is fine). Returns 1 and fills
 * rgb (w*h*3, top-down RGB) if the OSD is enabled with a DRAM base,
 * else 0. pitch is the plane's row stride in bytes. */
#define R_MCU_VCR20     0x0D80
#define R_DISP_OSD_SIZE 0x1A54
#define R_GAM_OSD       0x1C00
#define DISP_OSD_ENABLE 0x10000000u

int machine_scanout(machine_t *m, int w, int h, int pitch, uint8_t *rgb)
{
    uint32_t base = io_get(m, R_MCU_VCR20);
    uint32_t size = io_get(m, R_DISP_OSD_SIZE);
    const uint8_t *fb;
    int x, y;

    if (!(size & DISP_OSD_ENABLE)) return 0;
    fb = machine_dram_ptr(m, base);
    if (!fb) return 0;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t idx = fb[(uint32_t)y * (uint32_t)pitch + (uint32_t)x];
            uint32_t e = io_get(m, R_GAM_OSD + (uint32_t)idx * 4);
            int Y = (int)((e >> 16) & 0xFF);
            int Cb = (int)((e >> 8) & 0xFF) - 128;
            int Cr = (int)(e & 0xFF) - 128;
            /* BT.601 studio-swing YCbCr -> full-range RGB */
            double yy = 1.164 * (double)(Y - 16);
            int r = (int)(yy + 1.596 * Cr + 0.5);
            int g = (int)(yy - 0.392 * Cb - 0.813 * Cr + 0.5);
            int b = (int)(yy + 2.017 * Cb + 0.5);
            uint8_t *o = rgb + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 3;
            o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
    return 1;
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
