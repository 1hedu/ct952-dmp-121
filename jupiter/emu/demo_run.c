/*
 * ct952emu -- run the SDK splash demo on the emulated CT952 and snapshot
 * the OSD framebuffer it draws.
 *
 * Loads tests/demo_splash.bin (real JupiterSDK code cross-compiled for
 * SPARC V8), runs it inside the same CPU + machine model that boots the
 * stock ROM, then reads the 8bpp framebuffer and its ARGB palette back
 * out of emulated DRAM and writes a PPM -- a true picture produced by
 * the ported SDK executing on the emulated hardware.
 *
 *   demo_run <demo.bin> [out.ppm]
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W    616
#define FB_H    440
#define FB_ADDR   0x40200000u
#define PAL_ADDR  0x402C0000u

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/demo_splash.bin";
    const char *out = (argc > 2) ? argv[2] : "demo_splash.ppm";
    FILE *f;
    uint8_t *img, *fb;
    uint32_t pal[256];
    long sz;
    machine_t *m;
    uint64_t ran;
    int x, y, i;

    f = fopen(bin, "rb");
    if (!f) { perror(bin); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) {
        fprintf(stderr, "machine init failed\n"); return 2;
    }
    free(img);
    m->uart_echo = 0;

    ran = machine_run(m, 200000000ull);
    fprintf(stderr, "[demo_run] ran %llu instrs, pc=0x%08x %s\n",
            (unsigned long long)ran, m->cpu.pc,
            m->cpu.halted ? m->cpu.halt_reason : "(spin/done)");

    /* pull the framebuffer + palette out of emulated DRAM */
    fb = machine_dram_ptr(m, FB_ADDR);
    for (i = 0; i < 256; i++)
        pal[i] = machine_dram_rd(m, PAL_ADDR + (uint32_t)i * 4, 4);

    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            uint32_t c = pal[fb[y * FB_W + x]];
            fputc((int)((c >> 16) & 0xFF), f);
            fputc((int)((c >> 8) & 0xFF), f);
            fputc((int)(c & 0xFF), f);
        }
    fclose(f);
    fprintf(stderr, "[demo_run] wrote %s (%dx%d)\n", out, FB_W, FB_H);

    machine_free(m);
    free(m);
    return 0;
}
