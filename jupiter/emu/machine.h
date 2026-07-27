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

    /* Live LCM / display timing-generator raster (DP700WD_HW_REFERENCE.md
     * §12.50). The CT909 DISP block (ctkav_disp.h) drives a scan-line counter
     * (REG_DISP_MEM_LINE 0x1A68), toggles the field-parity bit each VSYNC
     * period, and can raise a per-N-hsync line interrupt (REG_DISP_N_HSYNC_INT
     * 0x1A6C -> INT_PROC1_1ST_HSYNC bit1). We derive MEM_LINE at read time from
     * vsync_cnt/vsync_div and the timing-generator total, so no separate line
     * clock can drift from the VSYNC cadence. */
    uint32_t disp_field;        /* even/odd field parity, toggles each VSYNC */
    uint32_t disp_hsync_grp;    /* last N-hsync group that raised the line IRQ */

    /* Functional hardware-JPEG-decode model: the CT952 decodes the staged
     * JPEG (e.g. the power-on COBY logo at DRAM 0x401dc000) in a DMA/VLD block
     * we don't model gate-for-gate. When armed, the emulator decodes the JPEG
     * at jpeg_src in-host at the decode-wait, writes the result to jpeg_out,
     * and reports the decoder done. */
    int jpeg_decode_en, jpeg_done;
    uint32_t jpeg_src;
    const char *jpeg_out;
    /* decoded RGB888 raster, kept so the scan-out can composite it as the
     * video plane under the OSD (the panel = photo on video + UI on OSD) */
    uint8_t *jpeg_rgb;
    int jpeg_w, jpeg_h;
    uint32_t jpeg_sig;      /* signature of the last-decoded staged bitstream */
    int jpeg_count;         /* number of distinct frames decoded */

    /* PROC2 vdec stand-in: PROC1 writes a VDEC command to REG_SRAM_PLAYMODE
     * (0xb0000190); the real decoder microcode acks by overwriting it with a
     * completion state (comdec.h EN_VDEC_CMD). We deliver that ack after a
     * short read latency so the firmware's wait loops make progress. */
    uint8_t proc2_cmd;
    int proc2_ack_countdown;

    /* Halted-decoder STOP->STOPPED dwell: when PROC2 is held in reset (the
     * power-on menu case) the firmware's boot thread runs the decoder-stop
     * handshake and polls first for MODE_STOP(0x10) then MODE_STOPPED(0x11)
     * (INITIAL_PowerONStatus, DP700WD_HW_REFERENCE.md 10.12). A zero-dwell ack
     * collapses 0x10->0x11 before the boot poll can latch 0x10. We instead hold
     * the commanded intermediate state for a fixed number of cycles (a real
     * decoder holds "stopping" until acknowledged), then deliver the ack, so
     * both the 0x10 and the 0x11 poll windows exist. */
    uint64_t proc2_ack_cycle;   /* m->cycles at which to deliver the ack (0=idle) */
    uint32_t proc2_ack_dwell;   /* dwell length in cycles (env CT952_VDEC_DWELL) */

    /* Decoder-STOP visibility window. The boot thread's stop poll (0x61170,
     * from INITIAL_PowerONStatus / POWERONMENU CC_KeyCommand(KEY_STOP)) reads
     * the decoder state via getter 0x6f054, which returns the SOFTWARE MIRROR
     * (0x40039cd0) OR'd with a busy bit when the live reg 0xB0000190 is not in
     * {0,0x11}. The poll waits for state == MODE_STOP(0x10). The firmware's own
     * mirror-writer only ever stores STOPPED(0x11) (never 0x10) and, on an
     * idempotent repeat-stop, the decoder is already stopped -- so with PROC2
     * in reset nothing ever presents MODE_STOP, and the poll rides out a
     * ~4.5-billion-instruction stage-1+stage-2 timeout. Model PROC2's behaviour:
     * for a short window after a stop command, present the mirror read as
     * MODE_STOP(0x10) so the poll latches it, then let it settle to STOPPED. */
    uint64_t vdec_stop_until;   /* present mirror as MODE_STOP while cycles < this */
    int vdec_stopped;           /* a stop has been issued: after the 0x10 window the
                                 * software mirror 0x40039cd0 holds STOPPED(0x11), so
                                 * the boot's gate-3 poll passes without the MIRROR10
                                 * read-hack (faithful decoder-stop bookkeeping) */
    int vdec_frame_done;        /* a JPEG frame has been functionally decoded: the
                                 * decoder is now at MODE_STOP(0x10)=frame-done, so the
                                 * decode-status getter (0x375a0->0x6f054, action 0)
                                 * maps the mirror 0x40039cd0 to JPEG_STATUS_OK. Set
                                 * after the stop gates, so gate-3's 0x11 is unaffected
                                 * (DP700WD_HW_REFERENCE.md 10.40). */

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

    uint64_t jpu_active_until;   /* cycles: JPU decode recently kicked (diag gating) */

    /* MCU BIU bit-stream read-channel drained flag (faithful JPU pipeline,
     * DP700WD_HW_REFERENCE.md 10.8). The JPEG worker (0x4001fe90) programs the
     * bit-stream source into BCR08 (io 0x2a20), then polls BCR0A (io 0x2a28)
     * for bit 12 (0x1000) = "read channel drained / macroblock stream ready".
     * We set biu_drained when the JPU GPU_CTL0 kick has functionally decoded
     * the staged frame, so the poll retires and the worker advances to
     * HALJPEG_Display instead of spinning out the decode-wait timeout. */
    int biu_drained;

    /* No-media model: stand in for the USBSRC worker thread so the firmware's
     * media-detect loop resolves to "no removable media". (Crutch removed §11.3;
     * field retained for ABI/layout stability of snapshots.) */
    int nomedia;

    /* GPU 2-D engine op accounting + font-index queue */
    uint64_t gpu_ops, gpu_font_ops, gpu_mode_ops[8];
    uint16_t gpu_fontq[1024];
    int gpu_fontn;

    /* io access inventory */
    mach_logent_t log[MACH_LOG_MAX];
    int log_n;
    uint32_t unmapped_reads, unmapped_writes;

    /* EHCI USB host controller (base 0xa0000100). Stage 1: bring-up + empty
     * root hub (no device attached) so the retail USB enumeration completes and
     * the boot advances past "starting usb stack". */
    uint32_t ehci_usbcmd, ehci_usbsts, ehci_usbintr, ehci_frindex;
    uint32_t ehci_ctrldss, ehci_periodic, ehci_async, ehci_configflag;
    uint32_t ehci_portsc[4];

    /* SD Host Controller (standard SDHC spec, base 0xa0001100) + a FAT image as
     * the inserted card. Lets the firmware's SDC driver (card.a/sdc.o) init the
     * card and CMD18-DMA-read blocks, so the media manager enumerates the card
     * and the browse UI populates. sd_img is the raw card image (FAT). */
    uint8_t  *sd_img;
    uint32_t  sd_size;
    uint32_t  sdc_reg[64];        /* register file, word-indexed by offset>>2 */
    uint32_t  sdc_int_stat;       /* REG_SDC_INT_STAT accumulator (0x30) */
    uint32_t  sdc_resp[4];        /* command response registers */
    int       sdc_acmd;           /* previous command was CMD55 (APP_CMD) */
    uint32_t  sdc_rca;            /* card relative address */
    uint8_t   sdc_data[512];      /* PIO read buffer (small data: SCR/switch/status) */
    uint32_t  sdc_data_len;       /* valid bytes in sdc_data */
    uint32_t  sdc_data_pos;       /* DATA_PORT read cursor */

    /* info.a JPEG-parse engine (0x80000800 block, §12.98): src at +0x220,
     * status at +0x230 (bits[16:21] progress, firmware waits >0x1f), GO at
     * +0x23c. Modeled: on GO, mark done so the parse's completion poll exits. */
    uint32_t  eng_src;            /* 0x80000a20 source address */
    uint32_t  eng_done;           /* set when GO written; drives 0x80000a30 */

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

/* gdb-stub support (gdbstub.c): step-with-breakpoints + side-effect-free
 * debug memory access. See the definitions in machine.c. */
int machine_step_bp(machine_t *m, const uint32_t *bps, int nbp,
                    uint64_t maxsteps, uint64_t *out_steps);
uint32_t machine_dbg_read(machine_t *m, uint32_t addr, int size);
void machine_dbg_write(machine_t *m, uint32_t addr, uint32_t val, int size);

/* Serve the GDB remote protocol on TCP `port`, driving this machine, until
 * the client detaches/kills. Returns 0 on a clean session, -1 on setup error. */
int gdb_serve(machine_t *m, int port);

/* Snapshot / restore the whole machine (registers + I/O + DRAM/BRAM/flash) to
 * `path`, so a boot to an interesting point is reached once and re-loaded
 * instantly. Returns 0 on success, -1 on I/O or format error. */
int machine_snapshot(machine_t *m, const char *path);
int machine_restore(machine_t *m, const char *path);

/* Direct DRAM helpers (addr in 0x40000000 space). */
uint32_t machine_dram_rd(machine_t *m, uint32_t addr, int size);
uint8_t *machine_dram_ptr(machine_t *m, uint32_t addr);
uint32_t machine_io_get(machine_t *m, uint32_t off);

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

/* De-tile the video/slideshow plane (macroblock-tiled YUV 4:2:0 at 0x40065000/
 * 0x400B3C00) straight from DRAM to an RGB PPM -- the photo the panel scans out.
 * Returns 0 on success, -1 on error. */
int machine_video_scanout(machine_t *m, uint32_t w, uint32_t h,
                          const char *ppm_path);

#endif /* CT952EMU_MACHINE_H */
