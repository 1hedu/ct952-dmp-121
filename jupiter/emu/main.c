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
    const char *dram_path = NULL;
    const char *fb_path = NULL;
    const char *uartin_path = NULL;
    uint64_t max_instr = 200000000ull;
    uint32_t seed_entry = 0, seed_sp = 0;
    uint32_t fb_addr = 0x4005F000u;   /* DS_OSDFRAME_ST */
    uint32_t fb_w = 616, fb_h = 440;  /* firmware OSD region geometry */
    uint32_t fb_stride = 720;         /* OSD buffer row stride (720-aligned, not fb_w) */
    int rom_load = 0;
    int m_skip_panelcfg = 0;
    int m_build_panelcfg = 0;
    int m_jpeg_en = 0;
    uint32_t m_jpeg_src = 0x401dc000u;
    const char *jpeg_out_path = NULL;
    int gdb_port = 0;
    uint64_t run_to = 0;
    const char *snap_out = NULL, *snap_in = NULL;
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
        else if (!strcmp(argv[i], "--dump-dram") && i + 1 < argc)
            dram_path = argv[++i];
        else if (!strcmp(argv[i], "--uart-in") && i + 1 < argc)
            uartin_path = argv[++i];
        else if (!strcmp(argv[i], "--fb-out") && i + 1 < argc)
            fb_path = argv[++i];
        else if (!strcmp(argv[i], "--fb-addr") && i + 1 < argc)
            fb_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fb-wh") && i + 1 < argc) {
            char *xp; fb_w = (uint32_t)strtoul(argv[++i], &xp, 0);
            if (xp && (*xp == 'x' || *xp == 'X')) fb_h = (uint32_t)strtoul(xp + 1, NULL, 0);
        }
        else if (!strcmp(argv[i], "--fb-stride") && i + 1 < argc)
            fb_stride = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed-entry") && i + 1 < argc)
            seed_entry = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed-sp") && i + 1 < argc)
            seed_sp = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--rom-load"))
            rom_load = 1;
        else if (!strcmp(argv[i], "--skip-panelcfg"))
            m_skip_panelcfg = 1;
        else if (!strcmp(argv[i], "--build-panelcfg"))
            m_build_panelcfg = 1;
        else if (!strcmp(argv[i], "--decode-jpeg"))
            m_jpeg_en = 1;
        else if (!strcmp(argv[i], "--jpeg-src") && i + 1 < argc)
            m_jpeg_src = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--jpeg-out") && i + 1 < argc) {
            jpeg_out_path = argv[++i]; m_jpeg_en = 1;
        }
        else if (!strcmp(argv[i], "--gdb") && i + 1 < argc)
            gdb_port = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--run-to") && i + 1 < argc)
            run_to = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)
            snap_out = argv[++i];
        else if (!strcmp(argv[i], "--restore") && i + 1 < argc)
            snap_in = argv[++i];
        else if (!strcmp(argv[i], "--quiet"))
            uart_path = uart_path;   /* handled below via flag */
        else if (argv[i][0] != '-')
            rom_path = argv[i];
    }
    if (!rom_path) {
        fprintf(stderr, "usage: ct952emu <flash.rom> [--instr N] "
                        "[--uart FILE] [--iolog FILE] [--quiet]\n"
                        "       [--fb-out PPM] [--fb-addr ADDR] [--fb-wh WxH]\n"
                        "       [--uart-in FILE]\n");
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
    m->skip_panelcfg = m_skip_panelcfg;
    m->build_panelcfg = m_build_panelcfg;
    m->jpeg_decode_en = m_jpeg_en;
    m->jpeg_src = m_jpeg_src;
    m->jpeg_out = jpeg_out_path;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--quiet"))
            m->uart_echo = 0;
    if (uart_path) {
        m->uart_file = fopen(uart_path, "wb");
        if (!m->uart_file) { perror(uart_path); return 1; }
    }
    if (uartin_path) {
        FILE *rf = fopen(uartin_path, "rb");
        if (!rf) { perror(uartin_path); return 1; }
        {
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), rf)) > 0)
                machine_uart_feed(m, buf, (uint32_t)n);
        }
        fclose(rf);
        fprintf(stderr, "[ct952emu] queued %u UART RX bytes from %s\n",
                m->rx_len, uartin_path);
    }

    if (snap_in) {
        /* fast re-attach: skip rom-load/boot, load a prior machine snapshot */
        if (machine_restore(m, snap_in) != 0) {
            fprintf(stderr, "[ct952emu] restore FAILED from %s\n", snap_in);
            return 1;
        }
        fprintf(stderr, "[ct952emu] restored snapshot %s (icount=%llu pc=0x%08x)\n",
                snap_in, (unsigned long long)m->cpu.icount, m->cpu.pc);
    } else {
        if (rom_load) {
            uint32_t entry = machine_rom_load(m, stderr);
            if (!entry) {
                fprintf(stderr, "[ct952emu] rom-load FAILED\n");
                return 1;
            }
            /* Stage the boot trampoline the way the mask ROM does: entry =
             * decompressed ROMV reset vector, sp = top of DRAM. */
            if (!seed_entry) seed_entry = entry;
            if (!seed_sp)    seed_sp = 0x40780000u;
            fprintf(stderr, "[ct952emu] rom-load OK, reset vector @ 0x%08x\n",
                    entry);
        }

        if (seed_entry || seed_sp) {
            machine_seed_boot(m, seed_entry, seed_sp);
            fprintf(stderr, "[ct952emu] seeded boot entry=0x%08x sp=0x%08x\n",
                    seed_entry, seed_sp);
        }
    }

    /* Optional: fast-forward to an icount, then snapshot -- reach an interesting
     * boot point once and re-load it instantly for later --restore --gdb runs. */
    if (run_to) {
        fprintf(stderr, "[ct952emu] fast-forwarding to icount %llu ...\n",
                (unsigned long long)run_to);
        machine_run(m, run_to);
        fprintf(stderr, "[ct952emu] reached icount=%llu pc=0x%08x\n",
                (unsigned long long)m->cpu.icount, m->cpu.pc);
    }
    /* CT952_DUMPFLAGS: print the POWERONMENU / OSDSS screensaver gate flags
     * (DP700WD_HW_REFERENCE.md 10.44/11.7) so the CURRENT faithful boot's state
     * can be re-measured (the "=0" reads in 11.7 predate the EHCI/faithful-JPU
     * work -- verify against the running target, not a stale dump). */
    if (getenv("CT952_DUMPFLAGS")) {
        static const struct { uint32_t a; int sz; const char *n; } fl[] = {
            /* __fThreadInit (initial.h): per-subsystem thread-init done bits.
             * bit0x1=MPEG-dec, 0x2=JPEG-dec, 0x100=Parser, 0x200=InfoFilter,
             * 0x80000=USB-src. Bit0x1 stays 0 by design -- the DMP photo-frame
             * build's INITIAL_ThreadInit (flash 0x41b60) has NO case for
             * THREAD_MPEG_DECODER(id 3): it falls through to `b,a 0x41d50`
             * (bare ret), so the MPEG decoder thread is never created and its
             * done-bit is never posted. See DP700WD_HW_REFERENCE.md 12.47. */
            {0x40038f80u, 4, "__fThreadInit"},
            {0x40023a10u, 1, "__bPOWERONMENUInitial"},
            {0x400239b8u, 4, "__dwOSDSSCheckTime"},
            {0x400239c4u, 1, "_bOSDSSScreenSaverMode"},
            {0x400239c0u, 4, "OSDSS_activity_token"},
            {0x400239ccu, 1, "__bOSDSSPicIdx"},
            {0x40026ea4u, 4, "event_flag(0x80 bit)"},
            /* CC-event worker F_REQ (0x40026e9c): the flag the worker 0x6748
             * waits on; bit 0x80 -> dispatcher 0x6eec -> PostEvent 0x12f10 ->
             * re-post the CC/OSD event mbox -> CC loop cycles. NEVER set (§12.43,
             * §12.49). Its low byte 0x40026e98 is MediaPresentPost's last source
             * index (0x6130). */
            {0x40026e9cu, 4, "F_REQ(worker wake;bit0x80)"},
            /* Display-stop state flag (0x4002401c): the display-STOP routine
             * 0xa41f0 sets bit1 and masks VSYNC (P1_1ST bit0 MDIS) at ~9.8M; the
             * display-ENABLE block 0x3fac0 (which re-arms VSYNC, the natural
             * per-frame CC-loop wake) never re-runs, so bit1 stays set and the
             * boot is left display-STOPPED. See §12.49. */
            {0x4002401cu, 4, "disp_state(bit1=stopped)"},
        };
        unsigned k;
        fprintf(stderr, "[DUMPFLAGS] pc=0x%08x icount=%llu\n",
                m->cpu.pc, (unsigned long long)m->cpu.icount);
        for (k = 0; k < sizeof(fl)/sizeof(fl[0]); k++) {
            uint8_t *p = machine_dram_ptr(m, fl[k].a);
            if (!p) { fprintf(stderr, "  %-24s @%08x  <unmapped>\n", fl[k].n, fl[k].a); continue; }
            if (fl[k].sz == 1)
                fprintf(stderr, "  %-24s @%08x = 0x%02x\n", fl[k].n, fl[k].a, p[0]);
            else {
                uint32_t v = (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
                             (uint32_t)p[2]<<8 | p[3];
                fprintf(stderr, "  %-24s @%08x = 0x%08x\n", fl[k].n, fl[k].a, v);
            }
        }
        /* Interrupt state (VSYNC/timer delivery diagnosis, §12.41): is the
         * per-frame VSYNC line (PROC1-1st bit0 -> LEON 13) unmasked & delivered,
         * or is it stuck pending because the firmware never unmasked it? */
        fprintf(stderr, "  IRQ: LEON_MASK@090=%08x PENDING@094=%08x  "
                "P1_1ST mask@0b0=%08x pend@0b4=%08x  P1_2ND mask@0d0=%08x pend@0d4=%08x\n",
                machine_io_get(m, 0x090), machine_io_get(m, 0x094),
                machine_io_get(m, 0x0b0), machine_io_get(m, 0x0b4),
                machine_io_get(m, 0x0d0), machine_io_get(m, 0x0d4));
    }
    if (snap_out) {
        if (machine_snapshot(m, snap_out) != 0)
            fprintf(stderr, "[ct952emu] snapshot FAILED to %s\n", snap_out);
        else
            fprintf(stderr, "[ct952emu] snapshot written to %s\n", snap_out);
        if (!gdb_port) { machine_free(m); free(m); return 0; }
    }

    if (gdb_port) {
        fprintf(stderr, "[ct952emu] flash %ld bytes, gdb stub mode\n", sz);
        gdb_serve(m, gdb_port);
        ran = m->cpu.icount;
    } else {
    fprintf(stderr, "[ct952emu] flash %ld bytes, running %llu instrs\n",
            sz, (unsigned long long)max_instr);
    ran = machine_run(m, max_instr);
    }
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

    /* control-flow trail leading to the stop (last register-indirect
     * jumps) -- pinpoints the source of a wild jump */
    {
        int n = m->cpu.tr_i < 16 ? m->cpu.tr_i : 16;
        int k;
        fprintf(stderr, "[ct952emu] last %d indirect jumps (from -> to):\n", n);
        for (k = n; k > 0; k--) {
            int idx = (m->cpu.tr_i - k) & 31;
            fprintf(stderr, "    0x%08x -> 0x%08x\n",
                    m->cpu.tr_from[idx], m->cpu.tr_to[idx]);
        }
    }

    /* last 64 PCs executed -- pinpoints the exact hot loop body */
    {
        int k;
        fprintf(stderr, "[ct952emu] last 64 PCs (oldest first):\n   ");
        for (k = 0; k < 64; k++) {
            int idx = (m->cpu.pc_ri + k) & 63;
            fprintf(stderr, " %08x", m->cpu.pc_ring[idx]);
            if ((k & 7) == 7) fprintf(stderr, "\n   ");
        }
        fprintf(stderr, "\n");
    }

    if (iolog_path) {
        FILE *lf = fopen(iolog_path, "w");
        if (lf) { machine_dump_iolog(m, lf); fclose(lf); }
    } else {
        machine_dump_iolog(m, stderr);
    }

    if (dram_path) {
        FILE *df = fopen(dram_path, "wb");
        if (df) {
            fwrite(machine_dram_ptr(m, 0x40000000u), 1, MACH_DRAM_SIZE, df);
            fclose(df);
            fprintf(stderr, "[ct952emu] dumped DRAM (%u bytes) to %s\n",
                    MACH_DRAM_SIZE, dram_path);
        }
    }

    if (fb_path) {
        int r = machine_disp_scanout(m, fb_addr, fb_w, fb_h, fb_stride, fb_path);
        if (r < 0)
            fprintf(stderr, "[ct952emu] fb scanout FAILED\n");
        else
            fprintf(stderr, "[ct952emu] wrote %s (%ux%u, OSD %s @ 0x%08x)\n",
                    fb_path, fb_w, fb_h, r == 0 ? "enabled" : "DISABLED",
                    fb_addr);
    }

    if (m->uart_file) fclose(m->uart_file);
    machine_free(m);
    free(m);
    return 0;
}
