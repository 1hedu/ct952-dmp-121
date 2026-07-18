/*
 * ct952emu -- run the jgpu hardware-verification image on the emulated
 * CT952 2-D GPU and report the result. testmain() returns the count of
 * bytes where the emulated engine's output differs from jgpu's software
 * model (0 = the Jupiter blitter driver programs the real engine exactly
 * as intended). start.S stores the return at 0x80007FF0 and the done
 * flag 0xC0DED00D at 0x80007FF4.
 *
 *   jgpu_check <jgpu_hw_test.bin>
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/jgpu_hw_test.bin";
    FILE *f = fopen(bin, "rb");
    uint8_t *img;
    long sz;
    machine_t *m;
    uint32_t flag, result;

    if (!f) { perror(bin); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) return 2;
    free(img);
    m->uart_echo = 0;
    machine_run(m, 50000000ull);

    flag   = m->io[0x7FF4 / 4];
    result = m->io[0x7FF0 / 4];

    if (flag != 0xC0DED00Du) {
        printf("FAIL: image did not complete (flag=%08x)\n", flag);
        return 1;
    }
    if (result != 0) {
        printf("FAIL: %u byte(s) differ between the emulated GPU engine "
               "and jgpu's model\n", result);
        return 1;
    }
    printf("jgpu on emulated CT952 GPU: engine output == jgpu model, "
           "byte-exact across all fills. OK\n");
    machine_free(m); free(m);
    return 0;
}
