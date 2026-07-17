/*
 * ct952emu -- CPU core verification.
 *
 * Loads the cross-compiled SPARC test image (tests/testprog.bin,
 * linked with a real trap table + window handlers) into the emulated
 * machine and runs it; the payload's testmain() result must equal the
 * SAME C code compiled natively into this harness. One number, two
 * compilers, two architectures: if the interpreter mis-executes any
 * instruction the results diverge.
 *
 * Also spot-checks trap plumbing (the payload flags unexpected traps
 * with 0xDEADDEAD) and that window traps actually fired (deep fib
 * recursion cannot fit in 8 windows).
 */
#include "machine.h"
#include "tests/testapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/testprog.bin";
    FILE *f;
    uint8_t *img;
    long sz;
    machine_t *m;
    uint32_t flag, result, expect;
    uint64_t ran;

    f = fopen(bin, "rb");
    if (!f) { perror(bin); return 2; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) {
        fprintf(stderr, "machine init failed\n");
        return 2;
    }
    free(img);
    m->uart_echo = 0;

    ran = machine_run(m, 400000000ull);

    flag = m->io[0x7FF4 / 4];
    result = m->io[0x7FF0 / 4];
    expect = testmain();

    printf("ran %llu instrs, flag=%08x result=%08x expect=%08x\n",
           (unsigned long long)ran, flag, result, expect);

    if (m->cpu.halted) {
        printf("FAIL: cpu halted: %s (pc=0x%08x)\n",
               m->cpu.halt_reason, m->cpu.halt_pc);
        return 1;
    }
    if (flag == 0xDEADDEADu) {
        printf("FAIL: payload took an unexpected trap (pc=0x%08x)\n",
               result);
        return 1;
    }
    if (flag != 0xC0DED00Du) {
        printf("FAIL: payload did not complete\n");
        return 1;
    }
    if (result != expect) {
        printf("FAIL: result mismatch\n");
        return 1;
    }

    printf("CRC cpu=%08x\n", result);
    printf("sparc core tests OK\n");
    machine_free(m);
    free(m);
    return 0;
}
