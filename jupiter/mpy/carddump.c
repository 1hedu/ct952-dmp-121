/* CT952A card-dump AP -- no MicroPython, no display, no keyboard.
 *
 * The USB-keyboard REPL fights the still-running firmware RTOS for the UART
 * and OSD plane (the "crosstalk" saga, see DP700WD_HW_REFERENCE.md 10.52-54):
 * both need interrupts enabled (VSYNC for the OSD, timer ticks for the RTOS
 * scheduler), so the two coexist on the same core and stomp each other.
 *
 * This AP sidesteps all of that. It needs no interrupts at all: no vsync, no
 * timer, no USB IRQ -- just busy-polled SD host controller registers. It
 * reuses start_banner.S unchanged, which already sets PSR PIL=15 (all
 * interrupt levels masked) before jumping here, so the firmware RTOS never
 * runs again once we're in pyapp_main. There is nothing left to crosstalk
 * with.
 *
 * Because mm_file.c (the firmware's file layer) is read-only -- it can find
 * and read files but has no code path to create one or grow the FAT -- this
 * AP cannot create DUMP.BIN itself. It can only overwrite sectors already
 * belonging to a file the FAT already maps. The workflow is:
 *   1. From a PC, drop a placeholder DUMP.BIN onto the card (filler bytes,
 *      sized for the expected dump + slack -- see build_carddump.sh).
 *   2. Run this AP on the frame: it walks the card's own FAT (16 or 32,
 *      whichever the card actually uses) to find DUMP.BIN's data sectors,
 *      formats a register dump, and writes it over the placeholder bytes
 *      via CARD_WriteSector-equivalent direct SDC commands.
 *   3. Pull the card, read DUMP.BIN on the PC.
 */
#include <stdint.h>

/* No libc (-nostdlib -ffreestanding): GCC still emits calls to memset for
 * some array zero-fills/struct inits at -Os despite -fno-builtin, so supply
 * our own rather than fight the idiom-recognition pass. */
void *memset(void *dst, int c, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

/* ---- on-screen status (OSD plane) ---------------------------------------
 * This AP has no serial console, and the status word at 0x401f4000 needs a
 * debug probe to read -- so a failure showed up only as "it booted but the
 * file is unchanged", with no way to tell WHICH step died. Render the outcome
 * to the OSD plane instead, reusing the banner AP's geometry, which is
 * MEASURED and confirmed on this panel (10.19): 4bpp plane at
 * DS_OSDFRAME_ST_AP, row pitch 292 bytes, the AP loader's live palette
 * (0 = transparent colour key, 1 = yellow, 2 = white). */
#include "font8x8.h"

#define OSD_BASE   0x40084000u
#define OSD_PITCH  292u
#define OSD_REGION 24576u
#define OSD_ROWS   56u
#define OSD_VIS_W  480
#define C_TXT      2

#define REG_CACHE    (*(volatile uint32_t *)0x80000014u)
#define REG_OSD_POS  (*(volatile uint32_t *)0x80001A50u)
#define REG_OSD_SIZE (*(volatile uint32_t *)0x80001A54u)
#define REG_SYSCFG1  (*(volatile uint32_t *)0x8000031Cu)

static volatile uint8_t *const FB = (volatile uint8_t *)OSD_BASE;

/* Flush/settle the cache. Needed BOTH to make OSD writes visible to the
 * scanout and to keep CPU and SD-controller DMA views of a buffer coherent:
 * without it a DMA read lands in DRAM while the CPU still sees stale cached
 * lines (and a CPU-filled buffer is still dirty in cache when DMA reads it). */
static void cache_flush(void)
{
    REG_CACHE &= ~0x00040000u;
    REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}

static void px(int x, int y, uint8_t c)
{
    uint32_t off;
    if (x < 0 || y < 0 || x >= OSD_VIS_W || (uint32_t)y >= OSD_ROWS) return;
    off = (uint32_t)y * OSD_PITCH + ((uint32_t)x >> 1);
    if (off >= OSD_REGION) return;
    if (x & 1) FB[off] = (uint8_t)((FB[off] & 0xF0) | (c & 0x0F));
    else       FB[off] = (uint8_t)((FB[off] & 0x0F) | ((c & 0x0F) << 4));
}

static void glyph(int x0, int y0, uint8_t ch)
{
    const uint8_t *g;
    int r, c;
    if (ch < 0x20 || ch > 0x7F) ch = 0x20;
    g = font8x8[ch - 0x20];
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            if (g[r] & (1u << c)) px(x0 + c, y0 + r, C_TXT);
}

static void osd_text(int row, const char *s)
{
    int i;
    for (i = 0; s[i]; i++) glyph(i * 8, row * 8, (uint8_t)s[i]);
}

static void osd_init(void)
{
    uint32_t sz, i;
    REG_SYSCFG1 &= ~0x10000000u;                 /* keep the watchdog dead */
    sz = REG_OSD_SIZE;
    REG_OSD_SIZE = (sz & ~0x0FFF0000u) | (OSD_ROWS << 16);
    REG_OSD_POS  = (78u << 16) | 102u;
    for (i = 0; i < OSD_REGION; i++) FB[i] = 0;  /* transparent */
}

/* ---- SD Host Controller (SDHC-standard), base 0xA0001100 (ctkav_sdc.h) --- */
#define SDC_BASE 0xA0001100u
#define SDC_R32(o) (*(volatile uint32_t *)(SDC_BASE + (o)))
#define SDC_R16(o) (*(volatile uint16_t *)(SDC_BASE + (o)))

#define SDC_DMA_ADDR   SDC_R32(0x00)
#define SDC_BLK_SIZE   SDC_R16(0x04)
#define SDC_BLK_COUNT  SDC_R16(0x06)
#define SDC_ARG        SDC_R32(0x08)
#define SDC_TRAN_MODE  SDC_R16(0x0c)
#define SDC_CMD        SDC_R16(0x0e)
#define SDC_RESP0      SDC_R32(0x10)
#define SDC_RESP1      SDC_R32(0x14)
#define SDC_RESP2      SDC_R32(0x18)
#define SDC_RESP3      SDC_R32(0x1c)
#define SDC_STAT       SDC_R32(0x24)
#define SDC_INT_STAT   SDC_R32(0x30)
#define SDC_INT_STAT_EN SDC_R32(0x34)
#define SDC_INT_EN      SDC_R32(0x38)
#define SDC_R8(o)      (*(volatile uint8_t *)(SDC_BASE + (o)))
#define SDC_PW_CTRL    SDC_R8(0x29)
#define SDC_CLK_CTRL   SDC_R16(0x2c)
#define SDC_TIMEOUT    SDC_R8(0x2e)
#define SDC_CPBLT0     SDC_R32(0x40)

#define CLK_INCLK_ENABLE (1u << 0)
#define CLK_INCLK_STABLE (1u << 1)
#define CLK_SDCLK_ENABLE (1u << 2)

#define STAT_CMD_INHIBIT_CMD (1u << 0)
#define STAT_CMD_INHIBIT_DAT (1u << 1)

#define INT_CMD_COMPLETE  (1u << 16)
#define INT_TRAN_COMPLETE (1u << 17)
#define INT_ERR           (1u << 31)

#define F_DATA_PRESENT (1u << 5)
#define F_RESP_LEN_0   (0u)
#define F_RESP_LEN_136 (1u)
#define F_RESP_LEN_48  (2u)
#define F_RESP_LEN_48B (3u)

#define TM_DMA  (1u << 0)
#define TM_READ (1u << 4)

/* Poll budget per wait. Deliberately small: every wait here is a busy loop over
 * an MMIO read, and this AP has no watchdog, no console and no way to be
 * interrupted -- an over-long budget is indistinguishable from a hang. A few
 * hundred thousand MMIO reads is already far longer than any healthy SD command
 * takes, so anything that exhausts it is broken, not slow. */
#define SDC_POLL 200000L

/* Issue one SD command and wait for completion (CMD, then DAT if present).
 * Returns 0 only on genuine completion; -1 on controller error OR timeout.
 * (Timeout MUST be an error: leaving it as success made a dead controller look
 * like a good read returning a zero-filled sector.) */
static int sdc_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t tran_mode,
                    uint32_t resp[4])
{
    uint32_t st = 0;
    long t;
    for (t = SDC_POLL; (SDC_STAT & STAT_CMD_INHIBIT_CMD) && t; t--) { }
    if (!t) return -1;                     /* controller never released the CMD line */
    if (tran_mode) {
        for (t = SDC_POLL; (SDC_STAT & STAT_CMD_INHIBIT_DAT) && t; t--) { }
        if (!t) return -1;
    }
    SDC_ARG = arg;
    SDC_TRAN_MODE = (uint16_t)tran_mode;
    SDC_CMD = (uint16_t)(((idx & 0x3fu) << 8) | flags);
    for (t = SDC_POLL; !((st = SDC_INT_STAT) & (INT_CMD_COMPLETE | INT_ERR)) && t; t--) { }
    if (!t) { SDC_INT_STAT = 0xFFFFFFFFu; return -1; }      /* no command completion */
    if (tran_mode && !(st & INT_ERR)) {
        for (t = SDC_POLL; !((st = SDC_INT_STAT) & (INT_TRAN_COMPLETE | INT_ERR)) && t; t--) { }
        if (!t) { SDC_INT_STAT = 0xFFFFFFFFu; return -1; }  /* no transfer completion */
    }
    SDC_INT_STAT = st;                     /* write-1-to-clear */
    if (resp) { resp[0] = SDC_RESP0; resp[1] = SDC_RESP1; resp[2] = SDC_RESP2; resp[3] = SDC_RESP3; }
    return (st & INT_ERR) ? -1 : 0;
}

static uint32_t g_rca;

/* Put the controller into a state where POLLING actually observes completions.
 *
 * Per the SDHC spec, a bit in Normal/Error Interrupt STATUS (0x30) is only ever
 * set if the corresponding bit in Interrupt Status ENABLE (0x34) is set --
 * Interrupt SIGNAL Enable (0x38) separately controls whether the CPU IRQ line
 * asserts. The stock firmware drives this controller from its ISR and may leave
 * 0x34 configured for its own use, so a polled driver that never touches 0x34
 * can spin forever on CMD_COMPLETE bits the hardware is not permitted to set.
 * Enable all status bits, and leave the signal enables OFF so nothing tries to
 * interrupt a CPU running at PIL=15 with the firmware's handlers dormant.
 * (The emulator sets the status bits unconditionally, so this gap is invisible
 * there and only shows up on silicon.) */
static void sdc_prepare(void)
{
    SDC_INT_EN = 0;                  /* no CPU interrupt signalling */
    SDC_INT_STAT = 0xFFFFFFFFu;      /* clear anything stale (write-1-to-clear) */
    SDC_INT_STAT_EN = 0xFFFFFFFFu;   /* allow every status bit to latch */
}

/* Turn the SD clock back on.
 *
 * MEASURED on hardware: after the AP loader has finished reading the AP off the
 * card it leaves CLK_CTRL (0x2c) = 0x0000 -- both the internal clock and SDCLK
 * disabled -- while the controller itself stays alive (HOST_VER reads 0x1000)
 * and the card stays inserted and ready (STAT = 0x000F0000, both CMD_INHIBIT
 * bits clear). With no clock nothing can be shifted to the card, so every
 * command times out. That is what made the first read fail.
 *
 * Bring it up in the order the SDHC spec requires: set the divider together
 * with INCLK_ENABLE, wait for INCLK_STABLE, and only then gate SDCLK on.
 * freq_sel is the 8-bit divided-clock selector (base / (2*freq_sel)); a large
 * divider is deliberate here, since re-identification must happen at a low
 * clock and a dump of a few hundred sectors is not worth tuning for. */
static int sdc_clock_on(uint32_t freq_sel)
{
    long t;
    SDC_PW_CTRL = 0x0Fu;                 /* BUS_VOL_33V (7<<1) | BUS_PW_ON */
    SDC_CLK_CTRL = 0;                    /* stop the clock while the divider changes */
    SDC_CLK_CTRL = (uint16_t)(((freq_sel & 0xFFu) << 8) | CLK_INCLK_ENABLE);
    for (t = SDC_POLL; !(SDC_CLK_CTRL & CLK_INCLK_STABLE) && t; t--) { }
    if (!t) return -1;                   /* internal clock never stabilised */
    SDC_CLK_CTRL |= CLK_SDCLK_ENABLE;
    SDC_TIMEOUT = 0x0E;                  /* near-max data timeout; we are not tuning */
    return 0;
}

/* CMD0/8/ACMD41/2/3/7: the same init sequence the retail SDC driver uses
 * (see machine.c's SDC model comment), minus CSD/ACMD6/CMD6 -- we only need
 * to read/write blocks, not negotiate bus width or speed class. */
static int sdc_init(void)
{
    uint32_t resp[4] = {0, 0, 0, 0};
    long tries;
    /* Bail immediately if the very first command cannot even be issued -- there
     * is no point running an identification sequence against a controller that
     * is not responding, and grinding through it is what previously looked like
     * a hang (200000 retries x a multi-million-iteration poll each). */
    if (sdc_cmd(0, 0, F_RESP_LEN_0, 0, 0) < 0) return -1;    /* GO_IDLE_STATE */
    sdc_cmd(8, 0x1AAu, F_RESP_LEN_48, 0, resp);              /* SEND_IF_COND (may fail on v1) */
    for (tries = 0; tries < 512; tries++) {                  /* ACMD41 busy-wait, bounded */
        if (sdc_cmd(55, 0, F_RESP_LEN_48, 0, 0) < 0) return -1;   /* APP_CMD */
        if (sdc_cmd(41, 0x40FF8000u, F_RESP_LEN_48, 0, resp) < 0) return -1; /* HCS */
        if (resp[0] & 0x80000000u) break;                     /* card done, not busy */
    }
    if (!(resp[0] & 0x80000000u)) return -1;
    sdc_cmd(2, 0, F_RESP_LEN_136, 0, resp);                  /* ALL_SEND_CID */
    sdc_cmd(3, 0, F_RESP_LEN_48, 0, resp);                    /* SEND_RELATIVE_ADDR */
    g_rca = resp[0] >> 16;
    return sdc_cmd(7, g_rca << 16, F_RESP_LEN_48B, 0, 0);    /* SELECT_CARD */
}

static int sdc_read_block(uint32_t lba, void *buf)
{
    int rc;
    SDC_BLK_SIZE = 512;
    SDC_BLK_COUNT = 1;
    SDC_DMA_ADDR = (uint32_t)buf;
    cache_flush();          /* drop stale/dirty lines before the DMA lands */
    rc = sdc_cmd(17, lba, F_DATA_PRESENT | F_RESP_LEN_48, TM_DMA | TM_READ, 0);
    cache_flush();          /* make the DMA'd bytes visible to the CPU */
    return rc;
}

static int sdc_write_block(uint32_t lba, const void *buf)
{
    SDC_BLK_SIZE = 512;
    SDC_BLK_COUNT = 1;
    SDC_DMA_ADDR = (uint32_t)buf;
    cache_flush();          /* push the CPU-filled buffer to DRAM for the DMA */
    return sdc_cmd(24, lba, F_DATA_PRESENT | F_RESP_LEN_48, TM_DMA, 0);
}

/* ---- FAT16/FAT32 (real cards are almost always FAT32; the /tmp/card.img
 * emulator test fixture is FAT16 -- support both). ------------------------ */
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct {
    uint32_t bytes_per_sec, sec_per_clus, rsvd_sec, num_fats;
    uint32_t root_ent_cnt, fat_size, first_data_sec, root_clus;
    int is_fat32;
} fatinfo_t;

/* SDMA buffers are 512-ALIGNED, not merely word-aligned. The SDHC DMA-buffer
 * boundary field in BLK_SIZE defaults to 4 KB: if a 512-byte transfer straddles
 * a 4 KB boundary the controller raises a DMA interrupt and stops, expecting the
 * driver to reprogram the address mid-transfer. A 512-aligned 512-byte block can
 * never straddle a 4 KB page, so the single-shot transfers below always complete.
 * (The emulator ignores the boundary entirely, so this only bites on silicon.) */
static uint8_t g_sector[512] __attribute__((aligned(512)));
static uint8_t g_fatbuf[512] __attribute__((aligned(512)));
static uint8_t g_wrbuf[512] __attribute__((aligned(512)));

static int fat_mount(fatinfo_t *fi)
{
    uint32_t fatsz16, fatsz32, root_dir_sectors;
    if (sdc_read_block(0, g_sector) < 0) return -1;
    if (g_sector[0x1fe] != 0x55 || g_sector[0x1ff] != 0xAA) return -1;
    fi->bytes_per_sec = le16(g_sector + 0x0b);
    fi->sec_per_clus  = g_sector[0x0d];
    fi->rsvd_sec      = le16(g_sector + 0x0e);
    fi->num_fats      = g_sector[0x10];
    fi->root_ent_cnt  = le16(g_sector + 0x11);
    fatsz16           = le16(g_sector + 0x16);
    fatsz32           = le32(g_sector + 0x24);
    fi->fat_size      = fatsz16 ? fatsz16 : fatsz32;
    fi->is_fat32      = (fatsz16 == 0);
    fi->root_clus     = fi->is_fat32 ? le32(g_sector + 0x2c) : 0;
    if (fi->bytes_per_sec != 512 || !fi->sec_per_clus || !fi->num_fats || !fi->fat_size)
        return -1;
    root_dir_sectors = fi->is_fat32 ? 0 :
        ((fi->root_ent_cnt * 32) + (fi->bytes_per_sec - 1)) / fi->bytes_per_sec;
    fi->first_data_sec = fi->rsvd_sec + fi->num_fats * fi->fat_size + root_dir_sectors;
    return 0;
}

static uint32_t clus_to_sec(const fatinfo_t *fi, uint32_t clus)
{ return fi->first_data_sec + (clus - 2u) * fi->sec_per_clus; }

/* Next cluster in the chain, or 0 at end-of-chain / on error. */
static uint32_t fat_next(const fatinfo_t *fi, uint32_t clus)
{
    uint32_t off, sec, v;
    if (fi->is_fat32) {
        off = clus * 4u;
        sec = fi->rsvd_sec + (off / fi->bytes_per_sec);
        if (sdc_read_block(sec, g_fatbuf) < 0) return 0;
        v = le32(g_fatbuf + (off % fi->bytes_per_sec)) & 0x0FFFFFFFu;
        return (v < 2 || v >= 0x0FFFFFF8u) ? 0 : v;
    } else {
        off = clus * 2u;
        sec = fi->rsvd_sec + (off / fi->bytes_per_sec);
        if (sdc_read_block(sec, g_fatbuf) < 0) return 0;
        v = le16(g_fatbuf + (off % fi->bytes_per_sec));
        return (v < 2 || v >= 0xFFF8u) ? 0 : v;
    }
}

/* Scan one directory sector's 16 entries for an exact 8.3 name match. */
static int scan_dir_sector(const uint8_t *name83, uint32_t *out_clus, uint32_t *out_size)
{
    uint32_t i;
    for (i = 0; i < 16; i++) {
        const uint8_t *e = g_sector + i * 32;
        int k, ok;
        if (e[0] == 0x00) return -1;                /* end of directory: stop */
        if (e[0] == 0xE5 || (e[0x0b] & 0x18) || e[0x0b] == 0x0F) continue;
        ok = 1;
        for (k = 0; k < 11; k++) if (e[k] != name83[k]) { ok = 0; break; }
        if (!ok) continue;
        *out_clus = ((uint32_t)le16(e + 0x14) << 16) | le16(e + 0x1a);
        *out_size = le32(e + 0x1c);
        return 1;
    }
    return 0;                                        /* not found in this sector, keep going */
}

static int fat_find(const fatinfo_t *fi, const uint8_t name83[11],
                     uint32_t *out_clus, uint32_t *out_size)
{
    if (!fi->is_fat32) {
        uint32_t root_sec = fi->rsvd_sec + fi->num_fats * fi->fat_size;
        uint32_t root_dir_sectors =
            ((fi->root_ent_cnt * 32) + (fi->bytes_per_sec - 1)) / fi->bytes_per_sec;
        uint32_t s;
        for (s = 0; s < root_dir_sectors; s++) {
            int r;
            if (sdc_read_block(root_sec + s, g_sector) < 0) return 0;
            r = scan_dir_sector(name83, out_clus, out_size);
            if (r > 0) return 1;
            if (r < 0) return 0;
        }
        return 0;
    }

    /* FAT32: the root directory is a cluster chain like any other file. */
    {
        uint32_t clus = fi->root_clus;
        while (clus) {
            uint32_t s;
            for (s = 0; s < fi->sec_per_clus; s++) {
                int r;
                if (sdc_read_block(clus_to_sec(fi, clus) + s, g_sector) < 0) return 0;
                r = scan_dir_sector(name83, out_clus, out_size);
                if (r > 0) return 1;
                if (r < 0) return 0;
            }
            clus = fat_next(fi, clus);
        }
        return 0;
    }
}

/* Write `len` bytes of `data` across the file's own allocated clusters,
 * stopping at the end of the chain (never touches another file's sectors). */
static int fat_write_file(const fatinfo_t *fi, uint32_t first_clus,
                           const uint8_t *data, uint32_t len)
{
    uint32_t clus = first_clus, remaining = len;
    while (clus && remaining) {
        uint32_t s;
        for (s = 0; s < fi->sec_per_clus && remaining; s++) {
            uint32_t chunk = remaining > 512 ? 512 : remaining, i;
            for (i = 0; i < 512; i++) g_wrbuf[i] = (i < chunk) ? data[i] : 0;
            if (sdc_write_block(clus_to_sec(fi, clus) + s, g_wrbuf) < 0) return -1;
            data += chunk;
            remaining -= chunk;
        }
        if (remaining) clus = fat_next(fi, clus);
    }
    return (remaining == 0) ? 0 : -1;     /* -1: ran out of preseeded space, wrote nothing past it */
}

/* ---- register dump text ---------------------------------------------- */
static char *pstr(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *phex(char *p, uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    int i;
    for (i = 28; i >= 0; i -= 4) *p++ = h[(v >> i) & 0xF];
    return p;
}

static char *preg(char *p, const char *name, uint32_t addr)
{
    p = pstr(p, name);
    *p++ = '=';
    p = phex(p, *(volatile uint32_t *)addr);
    *p++ = '\n';
    return p;
}

static uint8_t g_dump[8192] __attribute__((aligned(4)));

static uint32_t build_dump(void)
{
    char *p = (char *)g_dump;
    p = pstr(p, "CT952 register dump\n====================\n");
    p = pstr(p, "-- clock / system --\n");
    p = preg(p, "TIMER1_CONTROL      0x80000048", 0x80000048u);
    p = preg(p, "INT_MASK_PRIORITY   0x80000090", 0x80000090u);
    p = preg(p, "P1_1ST_MASK_ENABLE  0x800000B0", 0x800000B0u);
    p = preg(p, "P1_1ST_PENDING      0x800000B4", 0x800000B4u);
    p = preg(p, "P1_1ST_CLEAR        0x800000B8", 0x800000B8u);
    p = preg(p, "P1_2ND_MASK_ENABLE  0x800000D0", 0x800000D0u);
    p = preg(p, "P1_2ND_PENDING      0x800000D4", 0x800000D4u);
    p = preg(p, "P1_2ND_CLEAR        0x800000D8", 0x800000D8u);
    p = preg(p, "CLK_GATE            0x80000300", 0x80000300u);
    p = preg(p, "SYSCFG1             0x8000031C", 0x8000031Cu);
    p = preg(p, "REG_CACHE           0x80000014", 0x80000014u);
    p = pstr(p, "-- display --\n");
    p = preg(p, "OSD_POS             0x80001A50", 0x80001A50u);
    p = preg(p, "OSD_SIZE            0x80001A54", 0x80001A54u);
    p = preg(p, "LB_CR1              0x80001A28", 0x80001A28u);
    p = pstr(p, "-- USB (ChipIdea) --\n");
    p = preg(p, "USB_PORTSC          0xA0000184", 0xA0000184u);
    p = preg(p, "USB_OTGSC           0xA00001A4", 0xA00001A4u);
    p = preg(p, "USB_USBMODE         0xA00001A8", 0xA00001A8u);
    p = pstr(p, "-- SD host controller --\n");
    p = preg(p, "SDC_STAT            0xA0001124", 0xA0001124u);
    p = preg(p, "SDC_INT_STAT        0xA0001130", 0xA0001130u);
    p = pstr(p, "====================\n(end)\n");
    return (uint32_t)((uint8_t *)p - g_dump);
}

/* 8.3 directory-entry name for the preseeded placeholder: DUMP.BIN.
 *
 * The extension matters. The firmware's AP-discovery gate (ROM 0x254a4) only
 * looks for UPG952A.AP if its card scan counts ZERO media files, and its
 * extension table (ROM 0xe6a80, 40 entries) classifies .TXT as a SUBTITLE
 * format -- it sits alongside PSB/SMI/SUB/ASS/SSA/SRT in the subtitle matcher
 * at 0x1229c. So a .TXT placeholder counts as media, the scan returns nonzero,
 * and the AP is never loaded at all. An extension absent from that table is
 * invisible to the scan -- which is exactly why UPG952A.AP does not block
 * itself. .BIN is absent, so it is safe. Do NOT use .TXT/.DAT/.JPG/.LOG-like
 * names that appear in the table. §10.56. */
static const uint8_t DUMP_NAME[11] = {
    'D','U','M','P',' ',' ',' ',' ','B','I','N'
};

/* In-bounds scratch word on the real 2 MB frame (top of DRAM, below the AP
 * unzip buffer) recording how far we got -- readable with a JTAG/debug probe
 * even though this AP has no screen or serial console of its own. */
#define STATUS_WORD (*(volatile uint32_t *)0x401f4000u)

/* Small fixed-width formatting helpers for the status screen. */
static char g_line[64];

static void ln_clear(void) { int i; for (i = 0; i < 64; i++) g_line[i] = 0; }

static char *ln_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *ln_hex(char *p, uint32_t v, int digits)
{
    static const char h[] = "0123456789ABCDEF";
    int i;
    for (i = (digits - 1) * 4; i >= 0; i -= 4) *p++ = h[(v >> i) & 0xF];
    return p;
}

/* Name the stage in progress on row 3 and push it to the panel immediately.
 * Whatever is showing when the AP stops IS the stage that failed. */
static void stage(const char *s)
{
    int x, y;
    for (y = 3 * 8; y < 3 * 8 + 8; y++)      /* clear pixels: a space GLYPH draws nothing */
        for (x = 0; x < OSD_VIS_W; x++) px(x, y, 0);
    osd_text(3, s);
    cache_flush();
}

int pyapp_main(void)
{
    fatinfo_t fi;
    uint32_t clus = 0, fsize = 0, len = 0;
    uint32_t sig = 0, s0hi = 0, s0lo = 0, ist = 0;
    int step = 0;              /* last step reached: 1 read, 2 mount, 3 find, 4 write */
    int reinit = 0;            /* did we have to re-run card identification? */
    int clkrc = 0;             /* did the SD clock come up? */
    int ok;
    char *p;

    STATUS_WORD = 0x00000001u;
    osd_init();
    osd_text(0, "CT952 CARD DUMP AP");

    /* Show the controller's raw state BEFORE issuing any command, and flush so
     * it is on the panel even if everything after this stalls. If HV reads as
     * 0x00000000 or 0xFFFFFFFF the controller is not even addressable (gated
     * clock / powered down) and no amount of SD protocol will help. */
    ln_clear(); p = g_line;
    p = ln_str(p, "HV=");  p = ln_hex(p, SDC_R32(0xfc), 8);
    p = ln_str(p, " CK="); p = ln_hex(p, SDC_R32(0x2c), 8);
    p = ln_str(p, " CP="); p = ln_hex(p, SDC_CPBLT0, 8);
    osd_text(1, g_line);

    ln_clear(); p = g_line;
    p = ln_str(p, "ST=");  p = ln_hex(p, SDC_STAT, 8);
    p = ln_str(p, " IS="); p = ln_hex(p, SDC_INT_STAT, 8);
    osd_text(2, g_line);
    cache_flush();

    /* From here on, name the stage on screen BEFORE entering it and flush, so
     * that if a stage stalls the last line standing says exactly where. */
    stage("PREPARE");
    sdc_prepare();

    /* The loader leaves the SD clock off (see sdc_clock_on): without this every
     * command below times out, which is exactly what "FAIL STEP 0" was. */
    stage("SD CLOCK ON");
    clkrc = sdc_clock_on(0x40);          /* base/128: safe for re-identification */

    /* The firmware already identified and read this card (it loaded us from
     * it), so try it in the state it was left in first. A blind CMD0 would
     * knock a working card back to idle and demand a full re-identification
     * at a clock rate we never programmed -- so only re-init if the plain
     * read actually fails. */
    stage("READ SEC0");
    ok = (sdc_read_block(0, g_sector) == 0 &&
          g_sector[0x1fe] == 0x55 && g_sector[0x1ff] == 0xAA);
    if (!ok) {
        reinit = 1;
        stage("CARD INIT");
        if (sdc_init() == 0) {
            stage("READ SEC0 #2");
            ok = (sdc_read_block(0, g_sector) == 0 &&
                  g_sector[0x1fe] == 0x55 && g_sector[0x1ff] == 0xAA);
        }
    }
    stage("PARSE FAT");

    /* Capture what sector 0 actually looks like -- enough to tell a dead DMA
     * (zeros/0xFF) from a byte-order problem (recognisable bytes in the wrong
     * order) from a genuine FAT issue. */
    s0hi = ((uint32_t)g_sector[0] << 24) | ((uint32_t)g_sector[1] << 16) |
           ((uint32_t)g_sector[2] << 8)  |  g_sector[3];
    s0lo = ((uint32_t)g_sector[4] << 24) | ((uint32_t)g_sector[5] << 16) |
           ((uint32_t)g_sector[6] << 8)  |  g_sector[7];
    sig  = ((uint32_t)g_sector[0x1fe] << 8) | g_sector[0x1ff];
    ist  = SDC_INT_STAT;

    if (ok) {
        step = 1;
        if (fat_mount(&fi) == 0) {
            step = 2;
            if (fat_find(&fi, DUMP_NAME, &clus, &fsize)) {
                step = 3;
                len = build_dump();
                if (fat_write_file(&fi, clus, g_dump, len) == 0)
                    step = 4;
            }
        }
    }

    STATUS_WORD = (step == 4) ? (0x600D0000u | (len & 0xFFFFu))
                              : (0xBAD00000u | (uint32_t)step);

    stage(step == 4 ? "DONE" : "STOPPED");

    ln_clear(); p = g_line;
    p = ln_str(p, step == 4 ? "OK WROTE " : "FAIL STEP ");
    p = ln_hex(p, step == 4 ? len : (uint32_t)step, step == 4 ? 4 : 1);
    p = ln_str(p, reinit ? " REINIT" : " ASIS");
    p = ln_str(p, " (0RD 1MNT 2FIND 3WR 4OK)");
    osd_text(4, g_line);

    ln_clear(); p = g_line;
    p = ln_str(p, "SEC0=");  p = ln_hex(p, s0hi, 8);
    *p++ = ' ';              p = ln_hex(p, s0lo, 8);
    p = ln_str(p, " IS=");   p = ln_hex(p, ist, 8);
    osd_text(5, g_line);

    ln_clear(); p = g_line;
    p = ln_str(p, "SIG=");   p = ln_hex(p, sig, 4);
    p = ln_str(p, " CL=");   p = ln_hex(p, clus, 4);
    p = ln_str(p, " SZ=");   p = ln_hex(p, fsize, 8);
    p = ln_str(p, " CK2=");  p = ln_hex(p, SDC_CLK_CTRL, 4);
    p = ln_str(p, clkrc == 0 ? " CLKOK" : " CLKBAD");
    osd_text(6, g_line);

    cache_flush();
    for (;;) { }
    return 0;
}
