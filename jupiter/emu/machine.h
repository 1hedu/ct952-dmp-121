/*
 * ct952emu -- CT909P machine model: memory map + core devices.
 *
 * Map (from ctkav_platform.h / DVD909.ld):
 *   0x00000000  flash (XIP, the ROM image; writes ignored+logged)
 *   0x40000000  DRAM (default 8 MB to cover all family configs)
 *   0x80000000  on-chip I/O page (LEON core block + AV blocks)
 *   0x80004000  GPIO page (CT909P)
 *   0x90000000  DSU (stub)
 *   0xA0000000  FCR/SDC/MSC/NFC (stub)
 *   0xC0000000  D-cache-bypass alias of DRAM
 *
 * Modeled with real behavior: timers 1/2/3 + prescaler + watchdog,
 * UART1/2 + DSU-UART (tx -> host stdout/capture), the LEON interrupt
 * controller (mask/pending/force/clear), and a PROC2 stand-in that
 * ACKs the boot handshake and the AM mailbox so PROC1 firmware doesn't
 * hang waiting for the audio DSP. Everything else in the I/O pages is
 * a store/readback register file with an access log -- the bring-up
 * inventory that tells us which device to model next.
 */
#ifndef CT952EMU_MACHINE_H
#define CT952EMU_MACHINE_H

#include "sparc.h"
#include <stdio.h>

#define MACH_DRAM_SIZE   (8u << 20)
#define MACH_FLASH_MAX   (4u << 20)
#define MACH_IO_SIZE     0x8000u        /* 0x80000000..0x80007FFF backed */
#define MACH_LOG_MAX     512

typedef struct {
    uint32_t addr;
    uint32_t reads, writes;
    uint32_t last_write;
} mach_logent_t;

typedef struct machine {
    sparc_bus_t bus;        /* must be first (container-of via cast) */
    sparc_t cpu;

    /* PROC2: the CT909's second SPARC V8 core (LEON2-class), running the
     * audio / JPEG-decoder microcode PROC1 loads to 0x40002000. Shares the
     * bus (DRAM, vdec SRAM 0xb0000000, JPU regs). Released from reset by
     * PROC1 via REG_PLAT_RESET_CONTROL_DISABLE / the DSU2 control. Gated by
     * CT952_PROC2 while under bring-up. */
    sparc_t cpu2;
    sparc_bus_t bus2;       /* cpu2's bus view (no PROC1 interrupts) */
    int proc2_enable;       /* feature gate (env CT952_PROC2) */
    int proc2_on;           /* released and running */

    uint8_t *flash;
    uint32_t flash_size;
    uint8_t *dram;
    uint8_t *bram;          /* 0xB0000000 scratch SRAM (firmware RW use) */
    uint32_t io[MACH_IO_SIZE / 4];

    /* timers */
    uint32_t presc_cnt;
    uint64_t t3_value;

    /* display VSYNC generation (secondary PROC1-1st IRQ, LEON line 13) */
    uint32_t vsync_cnt, vsync_div;

    /* Functional hardware-JPEG-decode model: the CT952 decodes the staged
     * JPEG (e.g. the power-on COBY logo at DRAM 0x401dc000) in a DMA/VLD block
     * we don't model gate-for-gate. When armed, the emulator decodes the JPEG
     * at jpeg_src in-host at the decode-wait, writes the result to jpeg_out,
     * and reports the decoder done. */
    int jpeg_decode_en, jpeg_done;
    uint32_t jpeg_src;
    const char *jpeg_out;

    /* PROC2 vdec stand-in: PROC1 writes a VDEC command to REG_SRAM_PLAYMODE
     * (0xb0000190); the real decoder microcode acks by overwriting it with a
     * completion state (comdec.h EN_VDEC_CMD). We deliver that ack after a
     * short read latency so the firmware's wait loops make progress. */
    uint8_t proc2_cmd;
    int proc2_ack_countdown;

    /* uart capture */
    FILE *uart_file;         /* optional capture file (may be NULL) */
    int uart_echo;           /* echo UART bytes to stdout */

    /* uart rx: host -> firmware, drained on UART1 DATA reads */
    uint8_t *rx_buf;
    uint32_t rx_len, rx_pos;

    /* bring-up aid: force the panel-config descriptor's validity word
     * (0x4002f770, desc+0x14) to read as -1, so the boot config-register
     * thunk (flash 0x3d564) takes its built-in "no override" skip path
     * instead of spinning on the never-initialised config arena. */
    int skip_panelcfg;

    /* faithful aid: just before the config thunk (0x3d564) first runs,
     * invoke the firmware's own descriptor builder (0x3ce60) via
     * machine_call so the descriptor is built from the real SETD settings
     * sector -- reproducing the default-init pass the eCos init-callback
     * list would have run before the apply callback. */
    int build_panelcfg, panelcfg_built;

    /* GPU 2-D engine op accounting + font-index queue */
    uint64_t gpu_ops, gpu_font_ops, gpu_mode_ops[8];
    uint16_t gpu_fontq[1024];
    int gpu_fontn;

    /* io access inventory */
    mach_logent_t log[MACH_LOG_MAX];
    int log_n;
    uint32_t unmapped_reads, unmapped_writes;

    uint64_t cycles;
    int watchdog_fired;
} machine_t;

/* Create/reset the machine with a flash image (copied in). */
int machine_init(machine_t *m, const uint8_t *flash, uint32_t flash_size);

/* Queue bytes for the firmware to read from UART1 RX (host -> device).
 * Appends to any pending data; each byte is delivered once, in order,
 * and the UART1 status DATA_READY bit reflects whether any remain. */
void machine_uart_feed(machine_t *m, const uint8_t *data, uint32_t len);

/* Seed the boot-trampoline registers the earlier dsu_boot stage would
 * have written: GR22 (0x800007d8) = firmware entry, GR21 (0x800007d4)
 * = initial stack. The reset stub reads these and jumps. Call after
 * machine_init, before machine_run; 0 leaves them unset. */
void machine_seed_boot(machine_t *m, uint32_t entry, uint32_t sp);
void machine_free(machine_t *m);

/* Run n instructions (also advances timers 1 cycle per instruction). */
uint64_t machine_run(machine_t *m, uint64_t n);

/* Call firmware code inside the emulated machine: set %o0..%o2 = a0..a2,
 * %sp = sp, %o7 so the routine's `retl`/`ret` lands on an unmapped
 * sentinel, then run from `entry` until it returns (or budget/halt).
 * Runs with traps off and WIM=0 (window rotation only, no over/underflow
 * traps) -- fine for shallow leaf-ish routines like the decompressor.
 * Returns 0 on clean return, -1 if halted, -2 on budget. */
int machine_call(machine_t *m, uint32_t entry,
                 uint32_t a0, uint32_t a1, uint32_t a2,
                 uint32_t sp, uint64_t budget);

/* Direct DRAM helpers (addr in 0x40000000 space). */
uint32_t machine_dram_rd(machine_t *m, uint32_t addr, int size);
uint8_t *machine_dram_ptr(machine_t *m, uint32_t addr);

/* Parse the flash section table and stage every DRAM-resident section:
 * raw sections are copied, zip-flagged sections are decompressed by
 * invoking the firmware's own UZIP codec (flash 0x2000, wrapper +0xc50)
 * via machine_call -- exactly what the on-chip mask ROM does. Logs each
 * section to `log` (may be NULL). Returns the ROMV run address (the
 * reset entry) or 0 on failure. Leaves the CPU reset (pc=0) afterward. */
uint32_t machine_rom_load(machine_t *m, FILE *log);

/* Dump the I/O access inventory (sorted by address) to f. */
void machine_dump_iolog(machine_t *m, FILE *f);

/* Model the DISP display engine's OSD scan-out: composite the 8bpp
 * palette-indexed OSD plane at `osd_base` (w x h, `stride` bytes/row)
 * through the modelled DISP OSD palette RAM (GAM_OSD @ 0x80001C00,
 * BT.601 0x00YYUUVV) into an RGB PPM. Honours DISP_OSD_EN in
 * REG_DISP_OSD_SIZE. Returns 0 if OSD is enabled, 1 if disabled (still
 * writes the file), -1 on error. */
int machine_disp_scanout(machine_t *m, uint32_t osd_base,
                         uint32_t w, uint32_t h, uint32_t stride,
                         const char *ppm_path);

#endif /* CT952EMU_MACHINE_H */
