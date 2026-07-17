/*
 * ct952emu -- UZIP decompressor probe.
 *
 * Loads a full flash dump, then invokes the firmware's own UZIP
 * decompressor (the section at flash 0x2000) INSIDE the emulator to
 * decompress the ROMV (vectors) section -- the codec that was the
 * documented boot blocker. If the emulated call returns and the output
 * disassembles as a plausible SPARC vector table, the blocker is
 * cracked with zero reverse-engineering of the compression format.
 *
 *   probe_unzip <flash.bin>
 */
#include "machine.h"
#include <stdlib.h>
#include <string.h>

/* Decode the flash section table (entries: name, LMA, RMA, Lsize,
 * Rsize, crc @ 24 bytes each, starting at 0x10). */
typedef struct { char name[5]; uint32_t lma, rma, lsz, rsz, crc; } sec_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int find_sec(const uint8_t *d, uint32_t n, const char *nm, sec_t *out)
{
    uint32_t off = 0x10;
    while (off + 24 <= n) {
        int ok = 1, i;
        for (i = 0; i < 4; i++)
            if (d[off + i] < 32 || d[off + i] > 126) ok = 0;
        if (!ok) break;
        if (!memcmp(d + off, nm, 4)) {
            memcpy(out->name, d + off, 4); out->name[4] = 0;
            out->lma = be32(d + off + 4);  out->rma = be32(d + off + 8);
            out->lsz = be32(d + off + 12); out->rsz = be32(d + off + 16);
            out->crc = be32(d + off + 20);
            return 0;
        }
        off += 24;
    }
    return -1;
}

/* UZIP blob lives at flash 0x2000; its main entry (save-frame function
 * reading a header from %i0) is at +0xb4. */
#define UZIP_BASE   0x00002000u
#define WORKMEM     0x40300000u   /* scratch in DRAM */
#define CALL_SP     0x40700000u

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
        : "/root/.claude/uploads/b51ee6c4-3ef7-53a6-9569-6759a3c68d0d/"
          "fea55828-dp700wd.bin";
    FILE *f = fopen(path, "rb");
    uint8_t *img;
    long sz;
    machine_t *m;
    sec_t romv;
    int rc, i;

    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc((size_t)sz);
    if (fread(img, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    if (find_sec(img, (uint32_t)sz, "ROMV", &romv) != 0) {
        printf("FAIL: no ROMV section\n"); return 1;
    }
    printf("ROMV: flash 0x%08x (%u bytes compressed) -> 0x%08x "
           "(%u bytes)\n", romv.rma, romv.rsz, romv.lma, romv.lsz);

    m = malloc(sizeof(*m));
    if (machine_init(m, img, (uint32_t)sz) != 0) {
        printf("FAIL: machine init (flash %ld)\n", sz); return 1;
    }
    free(img);

    /* Entry offset within the UZIP blob (argv[2], default 0xb4). */
    {
        uint32_t eoff = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0)
                                   : 0xB4u;
        printf("calling UZIP+0x%x (src=%08x dst=%08x wm=%08x)\n",
               eoff, romv.rma, romv.lma, WORKMEM);
        rc = machine_call(m, UZIP_BASE + eoff, romv.rma, romv.lma, WORKMEM,
                          CALL_SP, 50000000ull);
    }
    printf("machine_call rc=%d  (pc=0x%08x icount=%llu %s)\n",
           rc, m->cpu.pc, (unsigned long long)m->cpu.icount,
           m->cpu.halted ? m->cpu.halt_reason : "");

    if (rc == 0) {
        uint8_t *out = machine_dram_ptr(m, romv.lma);
        printf("decompressed ROMV, first 16 words @ 0x%08x:\n", romv.lma);
        for (i = 0; i < 16; i++)
            printf("  +%02x: %08x\n", i * 4,
                   machine_dram_rd(m, romv.lma + (uint32_t)i * 4, 4));
        /* dump to file for objdump sanity */
        {
            FILE *o = fopen("romv_out.bin", "wb");
            if (o && out) { fwrite(out, 1, romv.lsz, o); fclose(o); }
            printf("wrote romv_out.bin (%u bytes)\n", romv.lsz);
        }
    }
    machine_free(m); free(m);
    return rc == 0 ? 0 : 1;
}
