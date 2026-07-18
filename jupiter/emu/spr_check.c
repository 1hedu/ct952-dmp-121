/*
 * ct952emu -- run the jspr hardware-verification image on the emulated
 * CT952 2-D GPU and report the result. The image drives the Jupiter
 * sprite API (jspr: clip + flip + build op) with two executors -- the
 * real emulated engine and jgpu's software model -- and testmain()
 * returns the count of bytes where they disagree (0 = jspr drives the
 * real engine exactly as its model intends, across clipped/flipped/
 * opaque sprites). start.S stores the return at 0x80007FF0 and the done
 * flag 0xC0DED00D at 0x80007FF4.
 *
 *   spr_check <jspr_hw_test.bin>
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/jspr_hw_test.bin";
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
               "and jspr's model across the sprite set\n", result);
        return 1;
    }
    printf("jspr on emulated CT952 GPU: sprite output == jgpu model, "
           "byte-exact across clipped/flipped/opaque sprites. OK\n");
    machine_free(m); free(m);
    return 0;
}
