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
    uint32_t fb_w = 480, fb_h = 240;  /* OSD framebuffer geometry (see stride) */
    uint32_t fb_stride = 480;         /* OSD row stride = 480, VERIFIED by byte-level
                                       * autocorrelation of the live OSD plane (§12.91):
                                       * the 720 guess sheared the content diagonally. */
    const char *video_path = NULL;    /* --video-out: de-tiled slideshow plane */
    uint32_t vid_w = 640, vid_h = 360;/* slideshow video plane native size */
    int rom_load = 0;
    int m_skip_panelcfg = 0;
    int m_build_panelcfg = 0;
    int m_jpeg_en = 0;
    uint32_t m_jpeg_src = 0x401dc000u;
    const char *jpeg_out_path = NULL;
    int gdb_port = 0;
    uint64_t run_to = 0;
    const char *snap_out = NULL, *snap_in = NULL;
    const char *aprun_path = NULL;    /* --aprun: run a UPG952A.AP body as a self-flasher */
    const char *flashout_path = NULL; /* --flash-out: dump the (possibly reflashed) flash */
    int spitest = 0;                  /* --spitest: drive the REAL flash driver via the SPI ctrl model */
    uint32_t spi_addr = 0x1a0000u, spi_size = 0x1000u;
    uint64_t spi_boot = 20000000ull;   /* enough to run flash-init (PROM_Config...Ok) */
    const char *apflash_path = NULL;  /* --apflash: full self-flashing AP via the gate-level ctrl */
    const char *apload_path = NULL;   /* --apload: section-table AP through the REAL loader */
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
        else if (!strcmp(argv[i], "--video-out") && i + 1 < argc)
            video_path = argv[++i];
        else if (!strcmp(argv[i], "--video-wh") && i + 1 < argc) {
            char *xp; vid_w = (uint32_t)strtoul(argv[++i], &xp, 0);
            if (xp && (*xp == 'x' || *xp == 'X')) vid_h = (uint32_t)strtoul(xp + 1, NULL, 0);
        }
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
        else if (!strcmp(argv[i], "--aprun") && i + 1 < argc)
            aprun_path = argv[++i];
        else if (!strcmp(argv[i], "--flash-out") && i + 1 < argc)
            flashout_path = argv[++i];
        else if (!strcmp(argv[i], "--spitest") && i + 1 < argc) {
            char *cp; spi_addr = (uint32_t)strtoul(argv[++i], &cp, 0);
            if (cp && *cp == ':') spi_size = (uint32_t)strtoul(cp + 1, NULL, 0);
            spitest = 1;
        }
        else if (!strcmp(argv[i], "--spitest-boot") && i + 1 < argc)
            spi_boot = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--apflash") && i + 1 < argc)
            apflash_path = argv[++i];
        else if (!strcmp(argv[i], "--apload") && i + 1 < argc)
            apload_path = argv[++i];
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

    /* --aprun FILE: run a UPG952A.AP body as a self-flasher, exactly as the
     * firmware's AP loader (0x3e48) does after header validation -- copy the AP
     * body (file bytes [0x200 .. size]) to DRAM 0x4009a000 and jump into it at
     * the header's entry field (0x30). The body then loops the modeled WriteSPF
     * (enable with CT952_FLASHWRITE) over its carried image and reboots. This
     * isolates the *body* under test: it needs no firmware services, so we start
     * from a fresh machine and jump straight in. (Header validation itself is
     * checked independently by `ctkap.py apinfo`; this proves the body's flashing
     * logic + that the low boot region is preserved.) §12.88. */
    if (aprun_path) {
        FILE *af = fopen(aprun_path, "rb");
        long asz;
        uint8_t *ap;
        if (!af) { perror(aprun_path); return 1; }
        fseek(af, 0, SEEK_END); asz = ftell(af); fseek(af, 0, SEEK_SET);
        ap = (uint8_t *)malloc((size_t)asz);
        if (!ap || fread(ap, 1, (size_t)asz, af) != (size_t)asz) {
            fprintf(stderr, "[aprun] read failed\n"); return 1; }
        fclose(af);
        {
            uint32_t apsize = (uint32_t)ap[0x0c]<<24 | (uint32_t)ap[0x0d]<<16 |
                              (uint32_t)ap[0x0e]<<8  | ap[0x0f];
            uint32_t entry  = (uint32_t)ap[0x30]<<24 | (uint32_t)ap[0x31]<<16 |
                              (uint32_t)ap[0x32]<<8  | ap[0x33];
            uint32_t bodylen = (apsize > 0x200 && apsize <= (uint32_t)asz)
                                 ? apsize - 0x200u : (uint32_t)asz - 0x200u;
            uint8_t *d = machine_dram_ptr(m, 0x4009a000u);
            if (!entry) entry = 0x4009a000u;
            if (!d) { fprintf(stderr, "[aprun] DRAM 0x4009a000 unmapped\n"); return 1; }
            memcpy(d, ap + 0x200, bodylen);
            m->cpu.pc  = entry;
            m->cpu.npc = entry + 4;
            sparc_set_reg(&m->cpu, 14, 0x40780000u);     /* %sp */
            sparc_set_reg(&m->cpu, 30, 0);               /* %fp */
            m->cpu.psr = 0xA0000000u | PSR_S | 0x00000F00u; /* S=1, PIL=15 */
            m->cpu.halted = 0;
            setenv("CT952_FLASHWRITE", "1", 0);          /* enable the write model */
            fprintf(stderr, "[aprun] loaded AP body (%u B) -> DRAM 0x4009a000, "
                    "entry=0x%08x, flash-write model ON\n", bodylen, entry);
        }
        free(ap);
    }

    /* --spitest ADDR:SIZE: prove the gate-level SPI controller model by running
     * the firmware's OWN DRAM-resident flash driver against it. Boot normally
     * (populating the driver's config + decompressing TEXT/DATA into DRAM), arm
     * the controller model, stage a sentinel in DRAM, then CALL the real WriteSPF
     * (0x3d0fc) -- which runs the real SE/PP helpers, issuing real SPI commands
     * that the model services against m->flash. --flash-out dumps the result.
     * §12.89. Needs --rom-load so the driver is actually resident. */
    if (spitest) {
        uint8_t *src;
        int rc;
        fprintf(stderr, "[spitest] booting %llu instrs to populate the flash driver...\n",
                (unsigned long long)spi_boot);
        machine_run(m, spi_boot);
        fprintf(stderr, "[spitest] boot pc=0x%08x; arming SPI controller model\n", m->cpu.pc);
        m->spi_ctrl_on = 1;
        m->spi_erases = m->spi_programs = 0;
        src = machine_dram_ptr(m, 0x40700000u);      /* scratch source buffer */
        if (!src) { fprintf(stderr, "[spitest] scratch DRAM unmapped\n"); return 1; }
        {
            uint32_t k; static const char pat[] = "CT952A-SPICTRL-REAL-DRIVER-";
            for (k = 0; k < spi_size; k++) src[k] = pat[k % (sizeof(pat) - 1)];
        }
        fprintf(stderr, "[spitest] calling real WriteSPF(0x3d0fc)(flash=0x%06x, "
                "src=0x40700000, size=0x%x)\n", spi_addr, spi_size);
        rc = machine_call(m, 0x3d0fcu, spi_addr, 0x40700000u, spi_size,
                          0x40760000u, 20000000ull);
        fprintf(stderr, "[spitest] WriteSPF returned rc=%d (%%o0=0x%x); SPI ops: "
                "%llu erase, %llu program\n", rc, sparc_get_reg(&m->cpu, 8),
                (unsigned long long)m->spi_erases, (unsigned long long)m->spi_programs);
        /* verify: flash[spi_addr..+size] should now equal the staged sentinel */
        {
            uint32_t k, bad = 0;
            for (k = 0; k < spi_size; k++)
                if (m->flash[spi_addr + k] != src[k]) bad++;
            fprintf(stderr, "[spitest] flash vs sentinel: %s (%u mismatched byte(s))\n",
                    bad ? "MISMATCH" : "MATCH", bad);
        }
        if (flashout_path) {
            FILE *ff = fopen(flashout_path, "wb");
            if (ff) { fwrite(m->flash, 1, m->flash_size, ff); fclose(ff);
                fprintf(stderr, "[spitest] dumped flash to %s\n", flashout_path); }
        }
        machine_free(m); free(m);
        return 0;
    }

    /* --apflash FILE: the FULL self-flashing update path on firmware code. Boot
     * normally (driver resident + config populated), arm the gate-level SPI
     * controller model, load the AP body to DRAM 0x4009a000 and jump into it as
     * the loader (0x3e48) does -- then the body calls the REAL WriteSPF (XIP
     * 0x3d0fc), whose real erase/program helpers issue real SPI commands the
     * controller model services against m->flash. Every layer is firmware code
     * except the modeled controller. §12.90. (The body must target a sector that
     * does NOT hold the XIP WriteSPF trampoline; a hardware body runs a DRAM copy
     * of the driver to rewrite even that -- see CT952A_FLASH_FORMAT.md.) */
    if (apflash_path) {
        FILE *af = fopen(apflash_path, "rb");
        long asz; uint8_t *ap;
        if (!af) { perror(apflash_path); return 1; }
        fseek(af, 0, SEEK_END); asz = ftell(af); fseek(af, 0, SEEK_SET);
        ap = (uint8_t *)malloc((size_t)asz);
        if (!ap || fread(ap, 1, (size_t)asz, af) != (size_t)asz) {
            fprintf(stderr, "[apflash] read failed\n"); return 1; }
        fclose(af);
        fprintf(stderr, "[apflash] booting %llu instrs to populate the flash driver...\n",
                (unsigned long long)spi_boot);
        machine_run(m, spi_boot);
        m->spi_ctrl_on = 1;
        m->spi_erases = m->spi_programs = 0;
        {
            uint32_t apsize = (uint32_t)ap[0x0c]<<24 | (uint32_t)ap[0x0d]<<16 |
                              (uint32_t)ap[0x0e]<<8  | ap[0x0f];
            uint32_t entry  = (uint32_t)ap[0x30]<<24 | (uint32_t)ap[0x31]<<16 |
                              (uint32_t)ap[0x32]<<8  | ap[0x33];
            uint32_t bodylen = (apsize > 0x200 && apsize <= (uint32_t)asz)
                                 ? apsize - 0x200u : (uint32_t)asz - 0x200u;
            uint8_t *d = machine_dram_ptr(m, 0x4009a000u);
            uint64_t ran;
            if (!entry) entry = 0x4009a000u;
            if (!d) { fprintf(stderr, "[apflash] DRAM 0x4009a000 unmapped\n"); return 1; }
            memcpy(d, ap + 0x200, bodylen);
            fprintf(stderr, "[apflash] boot pc=0x%08x; controller armed; AP body (%u B) "
                    "-> DRAM 0x4009a000, jumping to entry 0x%08x\n", m->cpu.pc, bodylen, entry);
            m->cpu.pc = entry; m->cpu.npc = entry + 4;
            sparc_set_reg(&m->cpu, 14, 0x40760000u);
            sparc_set_reg(&m->cpu, 30, 0);
            /* Trap-free window environment (as machine_call uses to run WriteSPF):
             * S=1, PIL=15, ET=0, WIM=0 so nested save/restore just rotate and the
             * body's terminating `ta 0` cleanly halts (error mode). */
            m->cpu.psr = 0xA0000000u | PSR_S | 0x00000F00u;
            m->cpu.wim = 0;
            m->cpu.halted = 0; m->watchdog_fired = 0;
            ran = machine_run(m, 100000000ull);
            fprintf(stderr, "[apflash] body ran %llu instrs, stopped: %s (pc=0x%08x); "
                    "SPI ops: %llu erase, %llu program\n", (unsigned long long)ran,
                    m->cpu.halted ? m->cpu.halt_reason : "budget", m->cpu.pc,
                    (unsigned long long)m->spi_erases, (unsigned long long)m->spi_programs);
        }
        free(ap);
        if (flashout_path) {
            FILE *ff = fopen(flashout_path, "wb");
            if (ff) { fwrite(m->flash, 1, m->flash_size, ff); fclose(ff);
                fprintf(stderr, "[apflash] dumped flash to %s\n", flashout_path); }
        }
        machine_free(m); free(m);
        return 0;
    }

    /* --apload FILE: run a LOADER-COMPATIBLE section-table AP through the firmware's
     * REAL loader. Boot, arm the gate-level SPI controller, stage the whole AP at
     * DS_AP_CODE_AREA (0x4009a000), replicate ROMLD_MoveSectionTable (copy the 32
     * SECTION_ENTRYs from body+0x210 to AP_TABLE_ADDRESS 0x40000800, adding
     * src-dest to each dwRMA -- exactly romld.c), then CALL the binary's
     * ROMLD_BOOT_LoadSectionAndRun (@0x4bc): it loads the Load-flagged sections to
     * their LMAs, checksum-verifies them, and jumps to the Load|ProgEntry section.
     * That flasher app (apstub_sec.S) then reflashes via the resident DRAM driver
     * -> gate-level controller -> m->flash. Every layer is firmware code except the
     * modeled controller. §12.92. */
    if (apload_path) {
        FILE *af = fopen(apload_path, "rb");
        long asz; uint8_t *ap; uint8_t *body;
        if (!af) { perror(apload_path); return 1; }
        fseek(af, 0, SEEK_END); asz = ftell(af); fseek(af, 0, SEEK_SET);
        ap = (uint8_t *)malloc((size_t)asz);
        if (!ap || fread(ap, 1, (size_t)asz, af) != (size_t)asz) {
            fprintf(stderr, "[apload] read failed\n"); return 1; }
        fclose(af);
        fprintf(stderr, "[apload] booting %llu instrs to populate the flash driver...\n",
                (unsigned long long)spi_boot);
        machine_run(m, spi_boot);
        m->spi_ctrl_on = 1;
        m->spi_erases = m->spi_programs = 0;
        body = machine_dram_ptr(m, 0x4009a000u);        /* DS_AP_CODE_AREA */
        if (!body) { fprintf(stderr, "[apload] DRAM 0x4009a000 unmapped\n"); return 1; }
        memcpy(body, ap, (size_t)asz);                  /* stage the whole AP */
        {
            uint32_t unzip = (uint32_t)ap[0x34]<<24 | (uint32_t)ap[0x35]<<16 |
                             (uint32_t)ap[0x36]<<8  | ap[0x37];   /* dwAP_UNZIP_BUF */
            uint32_t ap_sp = (uint32_t)ap[0x30]<<24 | (uint32_t)ap[0x31]<<16 |
                             (uint32_t)ap[0x32]<<8  | ap[0x33];   /* dwAP_SP */
            uint32_t dwoff = (0x4009a000u + 0x210u) - 0x40000800u; /* MoveSectionTable */
            uint8_t *src = machine_dram_ptr(m, 0x4009a210u);
            uint8_t *dst = machine_dram_ptr(m, 0x40000800u);
            int k; uint64_t ran;
            for (k = 0; k < 32; k++) {                  /* ROMLD_MoveSectionTable */
                uint8_t *se = src + k*24, *de = dst + k*24;
                uint32_t rma;
                memcpy(de, se, 24);
                rma = (uint32_t)de[8]<<24 | (uint32_t)de[9]<<16 | (uint32_t)de[10]<<8 | de[11];
                rma += dwoff;
                de[8]=(uint8_t)(rma>>24); de[9]=(uint8_t)(rma>>16);
                de[10]=(uint8_t)(rma>>8); de[11]=(uint8_t)rma;
            }
            fprintf(stderr, "[apload] boot pc=0x%08x; controller armed; AP staged @0x4009a000 "
                    "(%ld B); section table moved to 0x40000800\n", m->cpu.pc, asz);
            fprintf(stderr, "[apload] calling REAL ROMLD_BOOT_LoadSectionAndRun(0x4bc)"
                    "(tbl=0x40000800, unzip=0x%08x, sp=0x%08x)\n", unzip, ap_sp);
            ran = (uint64_t)machine_call(m, 0x4bcu, 0x40000800u, unzip, ap_sp,
                                         ap_sp, 10000000ull);
            fprintf(stderr, "[apload] returned (rc as icount unused); stopped: %s (pc=0x%08x); "
                    "SPI ops: %llu erase, %llu program\n",
                    m->cpu.halted ? m->cpu.halt_reason : "budget/return", m->cpu.pc,
                    (unsigned long long)m->spi_erases, (unsigned long long)m->spi_programs);
            {   /* diagnostics: the moved FLSH entry + whether it loaded to its LMA */
                uint8_t *e = machine_dram_ptr(m, 0x40000800u);
                uint8_t *l = machine_dram_ptr(m, 0x40500000u);
                if (e) fprintf(stderr, "[apload] tbl@0x40000800 entry0: name=%02x%02x%02x%02x "
                        "lma=%02x%02x%02x%02x rma=%02x%02x%02x%02x lsz=%02x%02x%02x%02x "
                        "flags=%02x%02x%02x%02x\n", e[0],e[1],e[2],e[3], e[4],e[5],e[6],e[7],
                        e[8],e[9],e[10],e[11], e[12],e[13],e[14],e[15], e[20],e[21],e[22],e[23]);
                if (l) fprintf(stderr, "[apload] LMA@0x40500000: %02x%02x%02x%02x %02x%02x%02x%02x "
                        "(want 21101400 e2042100 = flasher _start)\n",
                        l[0],l[1],l[2],l[3], l[4],l[5],l[6],l[7]);
            }
            (void)ran;
        }
        free(ap);
        if (flashout_path) {
            FILE *ff = fopen(flashout_path, "wb");
            if (ff) { fwrite(m->flash, 1, m->flash_size, ff); fclose(ff);
                fprintf(stderr, "[apload] dumped flash to %s\n", flashout_path); }
        }
        machine_free(m); free(m);
        return 0;
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

    if (flashout_path) {
        FILE *ff = fopen(flashout_path, "wb");
        if (ff) {
            fwrite(m->flash, 1, m->flash_size, ff);
            fclose(ff);
            fprintf(stderr, "[ct952emu] dumped flash (%u bytes) to %s\n",
                    m->flash_size, flashout_path);
        } else perror(flashout_path);
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

    if (video_path) {
        int r = machine_video_scanout(m, vid_w, vid_h, video_path);
        if (r < 0)
            fprintf(stderr, "[ct952emu] video scanout FAILED\n");
        else
            fprintf(stderr, "[ct952emu] wrote %s (%ux%u de-tiled slideshow plane @ 0x40065000)\n",
                    video_path, vid_w, vid_h);
    }

    if (m->uart_file) fclose(m->uart_file);
    machine_free(m);
    free(m);
    return 0;
}
