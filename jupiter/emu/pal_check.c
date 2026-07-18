/*
 * ct952emu -- verify the Jupiter OSD-palette path on the emulated DISP.
 *
 * Runs tests/pal_test.bin (loads GAM_OSD via jup_argb_to_yuv, draws two
 * index strips), scans the OSD plane out through the emulator's DISP
 * model (machine_disp_scanout, the exact BT.601 inverse), and checks how
 * closely each colour round-trips. The FIXED bank must round-trip within
 * a few LSB (rounding); the BUGGY (double-converted) bank is shown to be
 * grossly wrong -- demonstrating why jvid_load_palette must tag its YUV.
 *
 *   pal_check <pal_test.bin>
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>

#define OSD_BASE 0x4005F000u
#define SW 10u
#define SH 24u
#define OW (16u*SW)
#define OH (2u*SH)
#define ARGB_OUT 0x40380000u
#define TOL 6              /* max per-channel round-trip error allowed */

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/pal_test.bin";
    FILE *f = fopen(bin, "rb");
    uint8_t *img, *ppm;
    long sz;
    machine_t *m;
    int i, worst_fix = 0, worst_bug = 0, fail = 0;

    if (!f) { perror(bin); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) return 2;
    free(img);
    m->uart_echo = 0;
    machine_run(m, 20000000ull);

    if (machine_disp_scanout(m, OSD_BASE, OW, OH, OW, "/tmp/pal_scan.ppm") < 0) {
        printf("FAIL: scanout error\n"); return 1;
    }
    /* reload the rendered RGB */
    { FILE *pf = fopen("/tmp/pal_scan.ppm", "rb");
      char hdr[32]; int hw, hh, hmx;
      if (!pf) { printf("FAIL: no ppm\n"); return 1; }
      if (fscanf(pf, "%2s %d %d %d", hdr, &hw, &hh, &hmx) != 4) return 1;
      fgetc(pf);
      ppm = (uint8_t *)malloc((size_t)hw * hh * 3);
      if (fread(ppm, 1, (size_t)hw * hh * 3, pf) != (size_t)hw*hh*3) return 1;
      fclose(pf);
    }

    printf("idx   ARGB     fixed(RGB)  err   buggy(RGB)  err\n");
    for (i = 0; i < 16; i++) {
        uint32_t argb = machine_dram_rd(m, ARGB_OUT + (uint32_t)i * 4, 4);
        int er = (int)((argb >> 16) & 0xFF);
        int eg = (int)((argb >> 8) & 0xFF);
        int eb = (int)(argb & 0xFF);
        uint32_t cx = i * SW + SW / 2;
        /* top strip (row SH/2) = fixed bank; bottom (row SH+SH/2) = buggy */
        const uint8_t *pf = ppm + ((SH/2) * OW + cx) * 3;
        const uint8_t *pb = ppm + ((SH + SH/2) * OW + cx) * 3;
        int df = 0, db = 0, k;
        int fr[3] = {pf[0],pf[1],pf[2]}, br[3] = {pb[0],pb[1],pb[2]};
        int ex[3] = {er,eg,eb};
        for (k = 0; k < 3; k++) {
            int a = fr[k]-ex[k]; if (a<0)a=-a; if (a>df) df=a;
            int b = br[k]-ex[k]; if (b<0)b=-b; if (b>db) db=b;
        }
        if (df > worst_fix) worst_fix = df;
        if (db > worst_bug) worst_bug = db;
        if (df > TOL) fail = 1;
        printf("%2d  %06x  %02x%02x%02x     %2d   %02x%02x%02x    %3d\n",
               i, argb & 0xFFFFFF, fr[0],fr[1],fr[2], df,
               br[0],br[1],br[2], db);
    }
    printf("\nFIXED  path: worst per-channel round-trip error = %d (tol %d)\n",
           worst_fix, TOL);
    printf("BUGGY  path: worst per-channel error = %d (double conversion)\n",
           worst_bug);
    if (fail) { printf("FAIL: fixed palette path exceeds tolerance\n"); return 1; }
    printf("OSD palette path OK: jup_argb_to_yuv -> GAM_OSD -> DISP scanout "
           "round-trips within %d LSB.\n", TOL);
    machine_free(m); free(m);
    return 0;
}
