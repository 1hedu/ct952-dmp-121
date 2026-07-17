/*
 * ct952emu -- run the animated Mode 7 demo on the emulated CT952 and dump
 * the frames + palette for GIF assembly.
 *
 * Loads tests/demo_anim.bin, runs it (the emulated SPARC renders every
 * frame of the spinning racetrack into successive DRAM slots), then reads
 * the 8bpp frames back out along with the GAM_OSD palette resolved to
 * RGB, and writes a flat file:
 *
 *   [u32 w][u32 h][u32 nframes][256*3 RGB palette][nframes * w*h indices]
 *
 * tools/mkgif.py turns that into an animated GIF.
 *
 *   anim_run <demo_anim.bin> <out.bin>
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AW   256
#define AH   224
#define ANIM_FRAMES 24
#define ANIM_BASE   0x40300000u
#define ANIM_STRIDE (AW * AH)
#define R_GAM_OSD   0x1C00u

static void put_u32(FILE *f, uint32_t v)
{ fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); fputc((v>>16)&0xFF,f); fputc((v>>24)&0xFF,f); }

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/demo_anim.bin";
    const char *out = (argc > 2) ? argv[2] : "anim.bin";
    FILE *f;
    uint8_t *img;
    long sz;
    machine_t *m;
    int i, fr;

    f = fopen(bin, "rb");
    if (!f) { perror(bin); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) return 2;
    free(img);
    m->uart_echo = 0;
    machine_run(m, 300000000ull);
    fprintf(stderr, "[anim_run] rendered, pc=0x%08x\n", m->cpu.pc);

    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    put_u32(f, AW); put_u32(f, AH); put_u32(f, ANIM_FRAMES);

    /* GAM_OSD palette (YCbCr, BT.601 studio) -> RGB */
    for (i = 0; i < 256; i++) {
        uint32_t e = machine_dram_rd(m, 0x80001C00u + (uint32_t)i*4, 4);
        int Y=(int)((e>>16)&0xFF), Cb=(int)((e>>8)&0xFF)-128, Cr=(int)(e&0xFF)-128;
        double yy = 1.164*(double)(Y-16);
        int r=(int)(yy+1.596*Cr+0.5), g=(int)(yy-0.392*Cb-0.813*Cr+0.5),
            b=(int)(yy+2.017*Cb+0.5);
        fputc(r<0?0:r>255?255:r, f);
        fputc(g<0?0:g>255?255:g, f);
        fputc(b<0?0:b>255?255:b, f);
    }
    /* frames: raw 8bpp indices straight from DRAM */
    for (fr = 0; fr < ANIM_FRAMES; fr++) {
        uint8_t *fb = machine_dram_ptr(m, ANIM_BASE + (uint32_t)fr * ANIM_STRIDE);
        if (!fb) { fprintf(stderr, "[anim_run] frame %d out of DRAM\n", fr); break; }
        fwrite(fb, 1, (size_t)AW * AH, f);
    }
    fclose(f);
    fprintf(stderr, "[anim_run] wrote %s (%d frames %dx%d)\n",
            out, ANIM_FRAMES, AW, AH);
    machine_free(m); free(m);
    return 0;
}
