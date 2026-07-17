/*
 * ct952emu -- CLI: run a CT909 flash image (e.g. DVD909.rom).
 *
 *   ct952emu <flash.rom> [--instr N] [--uart FILE] [--iolog FILE]
 *            [--quiet]
 *
 * Runs N instructions (default 200M), echoing UART/DSU output, then
 * reports CPU state and dumps the unmodeled-I/O inventory -- the
 * bring-up worklist.
 */
#include "machine.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *uart_path = NULL, *iolog_path = NULL;
    uint64_t max_instr = 200000000ull;
    uint32_t seed_entry = 0, seed_sp = 0;
    machine_t *m;
    FILE *f;
    uint8_t *img;
    long sz;
    uint64_t ran;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instr") && i + 1 < argc)
            max_instr = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--uart") && i + 1 < argc)
            uart_path = argv[++i];
        else if (!strcmp(argv[i], "--iolog") && i + 1 < argc)
            iolog_path = argv[++i];
        else if (!strcmp(argv[i], "--seed-entry") && i + 1 < argc)
            seed_entry = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed-sp") && i + 1 < argc)
            seed_sp = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--quiet"))
            uart_path = uart_path;   /* handled below via flag */
        else if (argv[i][0] != '-')
            rom_path = argv[i];
    }
    if (!rom_path) {
        fprintf(stderr, "usage: ct952emu <flash.rom> [--instr N] "
                        "[--uart FILE] [--iolog FILE] [--quiet]\n");
        return 2;
    }

    f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n");
        return 1;
    }
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) {
        fprintf(stderr, "machine init failed (image %ld bytes)\n", sz);
        return 1;
    }
    free(img);

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--quiet"))
            m->uart_echo = 0;
    if (uart_path) {
        m->uart_file = fopen(uart_path, "wb");
        if (!m->uart_file) { perror(uart_path); return 1; }
    }

    if (seed_entry || seed_sp) {
        machine_seed_boot(m, seed_entry, seed_sp);
        fprintf(stderr, "[ct952emu] seeded boot entry=0x%08x sp=0x%08x\n",
                seed_entry, seed_sp);
    }

    fprintf(stderr, "[ct952emu] flash %ld bytes, running %llu instrs\n",
            sz, (unsigned long long)max_instr);
    ran = machine_run(m, max_instr);
    fprintf(stderr,
            "\n[ct952emu] stopped after %llu instrs: %s\n"
            "[ct952emu] pc=0x%08x npc=0x%08x psr=0x%08x tbr=0x%08x "
            "cwp=%u wim=0x%02x\n",
            (unsigned long long)ran,
            m->cpu.halted ? m->cpu.halt_reason
                          : (m->watchdog_fired ? "watchdog"
                                               : "instruction budget"),
            m->cpu.pc, m->cpu.npc, m->cpu.psr, m->cpu.tbr,
            (unsigned)(m->cpu.psr & PSR_CWP), m->cpu.wim);

    if (iolog_path) {
        FILE *lf = fopen(iolog_path, "w");
        if (lf) { machine_dump_iolog(m, lf); fclose(lf); }
    } else {
        machine_dump_iolog(m, stderr);
    }

    if (m->uart_file) fclose(m->uart_file);
    machine_free(m);
    free(m);
    return 0;
}
