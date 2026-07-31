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
 * AP cannot create DUMP.TXT itself. It can only overwrite sectors already
 * belonging to a file the FAT already maps. The workflow is:
 *   1. From a PC, drop a placeholder DUMP.TXT onto the card (filler bytes,
 *      sized for the expected dump + slack -- see build_carddump.sh).
 *   2. Run this AP on the frame: it walks the card's own FAT (16 or 32,
 *      whichever the card actually uses) to find DUMP.TXT's data sectors,
 *      formats a register dump, and writes it over the placeholder bytes
 *      via CARD_WriteSector-equivalent direct SDC commands.
 *   3. Pull the card, read DUMP.TXT on the PC.
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

/* Issue one SD command and wait for completion (CMD, then DAT if present). */
static int sdc_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t tran_mode,
                    uint32_t resp[4])
{
    uint32_t st = 0;
    long t;
    for (t = 2000000; (SDC_STAT & STAT_CMD_INHIBIT_CMD) && t; t--) { }
    if (tran_mode)
        for (t = 2000000; (SDC_STAT & STAT_CMD_INHIBIT_DAT) && t; t--) { }
    SDC_ARG = arg;
    SDC_TRAN_MODE = (uint16_t)tran_mode;
    SDC_CMD = (uint16_t)(((idx & 0x3fu) << 8) | flags);
    for (t = 4000000; !((st = SDC_INT_STAT) & (INT_CMD_COMPLETE | INT_ERR)) && t; t--) { }
    if (tran_mode && !(st & INT_ERR))
        for (t = 8000000; !((st = SDC_INT_STAT) & (INT_TRAN_COMPLETE | INT_ERR)) && t; t--) { }
    SDC_INT_STAT = st;                     /* write-1-to-clear */
    if (resp) { resp[0] = SDC_RESP0; resp[1] = SDC_RESP1; resp[2] = SDC_RESP2; resp[3] = SDC_RESP3; }
    return (st & INT_ERR) ? -1 : 0;
}

static uint32_t g_rca;

/* CMD0/8/ACMD41/2/3/7: the same init sequence the retail SDC driver uses
 * (see machine.c's SDC model comment), minus CSD/ACMD6/CMD6 -- we only need
 * to read/write blocks, not negotiate bus width or speed class. */
static int sdc_init(void)
{
    uint32_t resp[4] = {0, 0, 0, 0};
    long tries;
    sdc_cmd(0, 0, F_RESP_LEN_0, 0, 0);                      /* GO_IDLE_STATE */
    sdc_cmd(8, 0x1AAu, F_RESP_LEN_48, 0, resp);              /* SEND_IF_COND */
    for (tries = 0; tries < 200000; tries++) {
        sdc_cmd(55, 0, F_RESP_LEN_48, 0, 0);                 /* APP_CMD */
        sdc_cmd(41, 0x40FF8000u, F_RESP_LEN_48, 0, resp);     /* SD_SEND_OP_COND, HCS */
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
    SDC_BLK_SIZE = 512;
    SDC_BLK_COUNT = 1;
    SDC_DMA_ADDR = (uint32_t)buf;
    return sdc_cmd(17, lba, F_DATA_PRESENT | F_RESP_LEN_48, TM_DMA | TM_READ, 0);
}

static int sdc_write_block(uint32_t lba, const void *buf)
{
    SDC_BLK_SIZE = 512;
    SDC_BLK_COUNT = 1;
    SDC_DMA_ADDR = (uint32_t)buf;
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

static uint8_t g_sector[512] __attribute__((aligned(4)));
static uint8_t g_fatbuf[512] __attribute__((aligned(4)));

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
    uint8_t buf[512] __attribute__((aligned(4)));
    uint32_t clus = first_clus, remaining = len;
    while (clus && remaining) {
        uint32_t s;
        for (s = 0; s < fi->sec_per_clus && remaining; s++) {
            uint32_t chunk = remaining > 512 ? 512 : remaining, i;
            for (i = 0; i < 512; i++) buf[i] = (i < chunk) ? data[i] : 0;
            if (sdc_write_block(clus_to_sec(fi, clus) + s, buf) < 0) return -1;
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

/* "DUMP    TXT" -- 8.3 directory-entry name for the preseeded DUMP.TXT. */
static const uint8_t DUMP_NAME[11] = {
    'D','U','M','P',' ',' ',' ',' ','T','X','T'
};

/* In-bounds scratch word on the real 2 MB frame (top of DRAM, below the AP
 * unzip buffer) recording how far we got -- readable with a JTAG/debug probe
 * even though this AP has no screen or serial console of its own. */
#define STATUS_WORD (*(volatile uint32_t *)0x401f4000u)

int pyapp_main(void)
{
    fatinfo_t fi;
    uint32_t clus, fsize, len;

    STATUS_WORD = 0x00000001u;                          /* started */

    if (sdc_init() < 0)                             { STATUS_WORD = 0xBAD00001u; goto halt; }
    if (fat_mount(&fi) < 0)                         { STATUS_WORD = 0xBAD00002u; goto halt; }
    if (!fat_find(&fi, DUMP_NAME, &clus, &fsize))   { STATUS_WORD = 0xBAD00003u; goto halt; }

    len = build_dump();
    if (fat_write_file(&fi, clus, g_dump, len) < 0) { STATUS_WORD = 0xBAD00004u; goto halt; }

    STATUS_WORD = 0x600D0000u | (len & 0xFFFFu);        /* success: low 16 bits = bytes written */

halt:
    for (;;) { }
    return 0;
}
