/*
 * ct952emu -- machine model implementation.
 * Register offsets from ctkav_platform.h (cited per block).
 */
#include "machine.h"
#include "emujpeg.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
static long g_irq13_asserted, g_irq13_taken;
static long g_proc2_reset_writes;   /* writes to REG_PLAT_RESET_CONTROL_ENABLE (0x80000324) */
static long g_irq_taken[16];        /* per-level interrupt-take counts */
static int  g_pm_trace = -1, g_pm_n, g_pm_wn; /* decoder-playmode focused trace */

/* Cheap signature of the staged bitstream (a few sampled bytes) so we only
 * re-decode when the firmware has staged a *different* JPEG. */
static uint32_t jpeg_stage_sig(const uint8_t *p, uint32_t avail)
{
    static const uint32_t off[] = { 0, 3, 0x50, 0x200, 0x800, 0x2000, 0x8000 };
    uint32_t s = 0x811c9dc5u;
    unsigned i;
    for (i = 0; i < sizeof(off) / sizeof(off[0]); i++)
        if (off[i] < avail) s = (s ^ p[off[i]]) * 16777619u;
    return s;
}

/* Functional hardware-JPEG decode: the CT952 decodes photos/logos in a
 * DMA/VLD/JPU block we don't model gate-for-gate; the firmware stages the JPEG
 * bitstream in the MM video buffer (DS_VDBUF_ST_MM = 0x401dc000) and polls the
 * decoder's progress word at 0x80000c10. When armed, we decode whatever JPEG is
 * currently staged, keep the raster for the scan-out video-plane composite, and
 * report the decoder done. Re-arms whenever a *new* bitstream is staged, so the
 * firmware's logo and every slideshow frame each decode in turn. */
static void machine_maybe_jpeg_decode(machine_t *m)
{
    uint8_t *src, *rgb = NULL;
    uint32_t avail, sig;
    int w = 0, h = 0;

    m->jpeg_done = 1;                 /* decoder reports ready once armed */
    /* CT952_NODECODE: skip the (slow) functional picojpeg decode but keep the
     * decode-done semantics. For live-tracing after --restore, the repeated
     * JPU_GO writes in the park cycle would otherwise re-run a ~100ms 640x360
     * decode each time and crawl the run; we don't need pixels to single-step. */
    if (getenv("CT952_NODECODE")) { m->vdec_frame_done = 1; m->biu_drained = 1; return; }
    /* Built-in-photo staging (CT952_STAGE_PHOTO="<flash_off>"): copy a built-in
     * JPEG from flash (photo album section "0001" @0x160000/0x170000/0x180000)
     * into the staging buffer so the firmware's own working decode+display path
     * renders a real photo -- demonstrating the slideshow through the pipeline. */
    {
        const char *sp = getenv("CT952_STAGE_PHOTO");
        if (sp && m->flash) {
            uint32_t faddr = (uint32_t)strtoul(sp, NULL, 0);
            uint8_t *dst = machine_dram_ptr(m, m->jpeg_src);
            if (dst && (uint64_t)faddr + 0x10000u <= m->flash_size)
                memcpy(dst, m->flash + faddr, 0x10000u);
        }
    }
    if (m->jpeg_src < 0x40000000u ||
        m->jpeg_src >= 0x40000000u + MACH_DRAM_SIZE)
        return;
    src = m->dram + (m->jpeg_src - 0x40000000u);
    avail = (0x40000000u + MACH_DRAM_SIZE) - m->jpeg_src;
    if (getenv("CT952_JPEGTRACE")) {
        static int jt; if (jt < 40) { jt++;
            fprintf(stderr, "[JPGTRY] src=%08x SOI=%02x%02x icount=%llu\n",
                    m->jpeg_src, src[0], src[1], (unsigned long long)m->cpu.icount); }
    }
    if (src[0] != 0xFF || src[1] != 0xD8)   /* need a JPEG SOI staged */
        return;
    sig = jpeg_stage_sig(src, avail);
    if (sig == m->jpeg_sig)           /* same frame as last time -> done */
        return;
    if (emu_jpeg_decode(src, avail, &rgb, &w, &h) != 0) {
        if (getenv("CT952_JPEGTRACE"))
            fprintf(stderr, "[JPGTRY] decode FAILED src=%08x icount=%llu\n",
                    m->jpeg_src, (unsigned long long)m->cpu.icount);
        return;
    }
    m->jpeg_sig = sig;
    m->jpeg_count++;
    /* Faithful decode-completion: the decoder has produced a frame and settled
     * to MODE_STOP(0x10). The decode-status getter (0x6f054 action 0) maps that
     * to JPEG_STATUS_OK, so HALJPEG_Status(DECODE)=OK -> HALJPEG_Display (10.40). */
    m->vdec_frame_done = 1;
    if (m->jpeg_out) {
        /* first frame -> jpeg_out as given; later frames -> name.NN.ppm */
        char path[512];
        FILE *f;
        if (m->jpeg_count == 1)
            snprintf(path, sizeof(path), "%s", m->jpeg_out);
        else
            snprintf(path, sizeof(path), "%s.%02d.ppm", m->jpeg_out,
                     m->jpeg_count);
        if ((f = fopen(path, "wb"))) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            fwrite(rgb, 1, (size_t)w * h * 3, f);
            fclose(f);
        }
    }
    fprintf(stderr, "[ct952emu] JPEG decode #%d: %dx%d from 0x%08x\n",
            m->jpeg_count, w, h, m->jpeg_src);
    if (getenv("CT952_DECODE_STACK")) {   /* caller chain of this decode (play path) */
        int k;
        uint32_t fp;
        fprintf(stderr, "  [decode#%d caller PCs] o7=%08x i7=%08x recent:",
                m->jpeg_count, sparc_get_reg(&m->cpu, 15), sparc_get_reg(&m->cpu, 31));
        for (k = 40; k < 64; k++)
            fprintf(stderr, " %08x", m->cpu.pc_ring[(m->cpu.pc_ri + k) & 63]);
        fprintf(stderr, "\n");
        /* Register-window backtrace: saved %i7 at [fp+0x3c], saved %fp at
         * [fp+0x38] -- names the firmware call chain that kicked the decode
         * (OSDSS_Entry -> _OSDSS_PictureUpdate -> UTL_ShowJPEG_Slide -> ...). */
        (void)fp;
        {
            uint32_t bt[32];
            int nb = sparc_win_backtrace(&m->cpu, bt, 32), bi;
            fprintf(stderr, "  [decode#%d winframes]", m->jpeg_count);
            for (bi = 0; bi < nb; bi++) fprintf(stderr, " %08x", bt[bi]);
            fprintf(stderr, "\n");
        }
    }
    /* Pic-index finder (CT952_PICIDX): at each decode, diff the low data region
     * against the previous decode and report bytes that INCREMENTED by 1 with a
     * small value -- the OSDSS slideshow's picture-index counter (__bOSDSSPicIdx)
     * advances +1 per photo, so this pins its real address from our own binary. */
    if (getenv("CT952_PICIDX")) {
        static const uint32_t cand[] = {0x40022fa3u,0x40031ad3u,0x40039935u,0x400399b7u};
        int ci;
        fprintf(stderr, "  [decode#%d cand]", m->jpeg_count);
        for (ci = 0; ci < 4; ci++) {
            uint8_t *p = machine_dram_ptr(m, cand[ci]);
            fprintf(stderr, " %08x=%d", cand[ci], p ? *p : -1);
        }
        fprintf(stderr, "\n");
        static uint8_t *prev = NULL;
        const uint32_t LO = 0x40020000u, HI = 0x40040000u;
        uint8_t *base = machine_dram_ptr(m, LO);
        if (base) {
            if (!prev) { prev = malloc(HI - LO); if (prev) memcpy(prev, base, HI - LO); }
            else {
                uint32_t i;
                fprintf(stderr, "  [decode#%d +1 bytes]", m->jpeg_count);
                for (i = 0; i < HI - LO; i++)
                    if (base[i] == (uint8_t)(prev[i] + 1) && base[i] <= 32)
                        fprintf(stderr, " %08x:%u->%u", LO + i, prev[i], base[i]);
                fprintf(stderr, "\n");
                memcpy(prev, base, HI - LO);
            }
        }
    }
    /* keep the raster for the scan-out video-plane composite */
    free(m->jpeg_rgb);
    m->jpeg_rgb = rgb;
    m->jpeg_w = w;
    m->jpeg_h = h;

    /* Faithful JPU MCU-BIU output (§10.8/§10.2/§10.10): write the decoded frame
     * into the firmware's video frame buffer as macroblock-tiled YUV 4:2:0, so
     * the hardware decode the driver kicked "produces" real pixels in the buffer
     * the scan-out reads. This is the MCU-BIU write-back the real block would do
     * as it drained the reconstructed macroblocks; §10.33 showed the Y buffer
     * (0x40065000) was all-zero without it. Y at DS_FRAMEBUF_ST_SLIDESHOW
     * (0x40065000), C at +0x4EC00 (0x400B3C00); strip=0x2D00 (720-wide buffer).
     * Formerly gated behind CT952_LOGODECODE (crutch); now unconditional so the
     * firmware's own decode path renders real pixels. */
    {
        const uint32_t YBASE = 0x40065000u, CBASE = 0x400B3C00u, strip = 0x2D00u;
        uint8_t *yb = machine_dram_ptr(m, YBASE);
        uint8_t *cb = machine_dram_ptr(m, CBASE);
        int x, y;
        if (yb && cb) {
            for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
                const uint8_t *px = rgb + ((size_t)y * w + x) * 3;
                int R = px[0], G = px[1], B = px[2];
                int Y = (299*R + 587*G + 114*B) / 1000;
                uint32_t yo = (uint32_t)(y>>4)*strip + (uint32_t)(x>>2)*64u
                            + (uint32_t)(y&15)*4u + (uint32_t)(x&3);
                yb[yo] = (uint8_t)(Y < 0 ? 0 : Y > 255 ? 255 : Y);
                if (!(x & 1) && !(y & 1)) {
                    int U = (-169*R - 331*G + 500*B) / 1000 + 128;
                    int V = ( 500*R - 419*G -  81*B) / 1000 + 128;
                    uint32_t cx = x>>1, cy = y>>1;
                    uint32_t co = (cy>>4)*strip + (cx>>3)*256u + ((cx&7)>>2)*64u
                                + (cy&15)*4u + (cx&3);
                    cb[co]       = (uint8_t)(U < 0 ? 0 : U > 255 ? 255 : U);
                    cb[co + 128] = (uint8_t)(V < 0 ? 0 : V > 255 ? 255 : V);
                }
            }
            fprintf(stderr, "[ct952emu] JPU MCU-BIU wrote %dx%d tiled YUV to 0x%08x/0x%08x\n",
                    w, h, YBASE, CBASE);
        }
    }
}

/* LEON core block offsets (ctkav_platform.h:19-97) */
#define R_TIMER1_CNT   0x040
#define R_TIMER1_RLD   0x044
#define R_TIMER1_CTL   0x048
#define R_WATCHDOG     0x04C
#define R_TIMER2_CNT   0x050
#define R_TIMER2_RLD   0x054
#define R_TIMER2_CTL   0x058
#define R_PRESC_CNT    0x060
#define R_PRESC_RLD    0x064
#define R_TIMER3_CTL   0x068
#define R_TIMER3_VAL   0x06C
#define R_UART1_DATA   0x070
#define R_UART1_STAT   0x074
#define R_UART2_DATA   0x080
#define R_UART2_STAT   0x084
#define R_INT_MASK     0x090
#define R_INT_PENDING  0x094
#define R_INT_FORCE    0x098
#define R_INT_CLEAR    0x09C
/* Secondary "PROC1 1st" interrupt controller (ctkav_platform.h:58-64):
 * cascades into LEON interrupt line 13 (INT_NO_PROC1_1ST). Bit0 = VSYNC,
 * the display-timing tick that drives the firmware's display/slideshow
 * state machine (interrupt.c: INT_Proc1_1st_isr -> ISR_DISPSaveClearStatus). */
#define R_P1_1ST_MASK  0x0B0        /* MASK_ENABLE: direct RW enable mask */
#define R_P1_1ST_PEND  0x0B4        /* PENDING: RW; VSYNC source ORs bit0 */
#define R_P1_1ST_STCL  0x0B8        /* STATUS(R) / CLEAR(W1C) */
#define R_P1_1ST_MDIS  0x0BC        /* MASK_DISABLE (W1C into MASK) */
#define INT_NO_PROC1_1ST 13
#define IRQ_P1_1ST_VSYNC 0x1u
#define IRQ_P1_1ST_HSYNC 0x2u       /* INT_PROC1_1ST_HSYNC: N-hsync line int */
/* LCM / display timing-generator registers (ctkav_disp.h, CT909P_IC_SYSTEM).
 * REG_DISP_BASE = 0x80001A00; offsets are from 0x80000000. */
#define R_DISP_TGEN_TOTAL 0x1A38    /* (Vtotal<<16)|Htotal; bit28 DISP_TGEN_EN */
#define R_DISP_SYNC_WH    0x1A3C    /* (Hwidth<<16)|Vheight; bit28 DISP_PSCAN_EN */
#define R_DISP_MEM_LINE   0x1A68    /* live read-frame-buffer line position */
#define R_DISP_N_HSYNC    0x1A6C    /* interrupt for each N hsyncs */
#define R_DISP_F0Y_ADDR   0x1AC0    /* main frame buffer 0 Y start address */
#define DISP_TGEN_EN      0x10000000u  /* TGEN_TOTAL: timing generator enable */
#define DISP_PSCAN_EN     0x10000000u  /* SYNC_WH: progressive-scan enable */
#define DISP_EVEN_FIELD   0x10000000u  /* MEM_LINE: even-field indicator */
/* Display DMA current-scan-address registers (0x80000E00 block): E04/E08 hold
 * the DMA start/end DRAM pointers, E0C/E10 the live current-read pointers the
 * display-enable safe-scan check (flash 0x3fa40) reads to avoid reconfiguring
 * mid-active-line. See DP700WD_HW_REFERENCE.md §12.50. */
#define R_DISP_DMA_START  0xE04
#define R_DISP_DMA_END    0xE08
#define R_DISP_DMA_CUR0   0xE0C
#define R_DISP_DMA_CUR1   0xE10
/* Secondary "PROC1 2nd" interrupt controller (ctkav_platform.h:70-76):
 * cascades into LEON interrupt line 10 (INT_NO_PROC1_2ND). Sources: USB/SERVO/
 * IR/BIU/MCU/VPU. We model the IR source (bit2) so a modeled remote keypress
 * drives the firmware's own INT_Proc1_2nd_isr -> ISR_IRSaveClearStatus ->
 * DSR_IR -> INPUT_RemoteScan -> __bISRKey: the faithful key-input path that
 * feeds the "Loading" event queue (DP700WD_HW_REFERENCE.md 10.37/10.38). */
#define R_P1_2ND_MASK  0x0D0        /* MASK_ENABLE: direct RW enable mask */
#define R_P1_2ND_PEND  0x0D4        /* PENDING: RW */
#define R_P1_2ND_STCL  0x0D8        /* STATUS(R) / CLEAR(W1C) */
#define R_P1_2ND_MDIS  0x0DC        /* MASK_DISABLE (W1C into MASK) */
#define INT_NO_PROC1_2ND 10
#define INT_P1_2ND_IR  0x4u         /* INT_PROC1_2ND_IR (interrupt.c) */
/* Platform IR receiver block (ctkav_platform.h:497-506), HW-NEC decoder.
 * IR_DATA[7:0]=scancode, [8]=repeat, [10]=invalid data; IR_RAW_CODE[31:24]=
 * customer, [23:16]=customer1. INPUT_RemoteScan reads the ISR-saved copies. */
#define R_IR_DATA      0x390
#define R_IR_RAWCODE   0x394
#define R_DSU_UART_DATA 0x0C0
#define R_DSU_UART_STAT 0x0C4
/* Command block (ctkav_platform.h:474-481): PARAMETER1 = 0x364 */
#define R_PARAM1       0x364
#define R_PARAM2       0x368
/* AIU GR bank: PROC2_SP GR21 0x7D4, PROC2_STARTADR GR22 0x7D8,
 * audio start/ACK word 0x7E4 (hdecoder.c:702-737) */
#define R_PROC2_SP     0x7D4
#define R_PROC2_START  0x7D8
#define R_AUDIO_CMD    0x7E4

/* Hardware IIC/EEPROM master (undocumented in the headers; the firmware
 * pokes it directly at 0x80004200). The boot config/EEPROM read routine
 * (flash 0x62538) writes a command to 0x4210 -- clears bit0, then sets
 * 0x24 (bit2 = start/trigger, bit5 = mode) -- and spins on bit2 until the
 * hardware clears it, with a ~1000-tick timeout. On real silicon bit2 is
 * a self-clearing "transaction in progress" flag; the store/readback
 * register file would leave it stuck set forever, so we clear it on read
 * (the transaction completes instantly in the model). 0x4204 is the
 * status/result word; a companion routine (flash 0x625b0) reads it and
 * defaults to the 0xAA55 EEPROM signature. */
#define R_IIC_STAT     0x4204
#define R_IIC_CMD      0x4210
#define R_IIC_DATA     0x4214
#define IIC_BUSY       0x4u

#define TIMER_ENABLE   1u
#define TIMER_RELOAD   2u
#define TIMER_LOAD     4u

/* VLD entropy-decoder status (ctkav_vdec.h): the JPEG decoder thread polls
 * these for per-picture completion. Reported done so HALJPEG_Decode's thread
 * finishes (the real pixels come from the functional decode). */
#define R_VLD_STATUS   0x2208        /* VLD_MB_RDY = bit26 (0x4000000) */
#define R_VLD_MBINT    0x21C0        /* HDR/RL/MC done + JPEG_ST bits */
#define VLD_MB_RDY     0x4000000u
#define VLD_DONE_BITS  0x80Bu        /* JPEG_ST|MC_DONE|RL_DONE|HDR_DONE */
/* MCU audio bitstream-buffer remainder (ctkav_mcu.h:185): the audio-hang
 * monitor treats a frozen value as a dead DSP and resets PROC2. */
#define R_MCU_A0REM    0x2F10
#define R_MCU_A1REM    0x2F14

#define UART_STAT_READY 0x6u   /* TX shift + holding empty, no RX data */
#define UART_STAT_DATA_READY 0x1u   /* RX byte available (ctkav_platform.h) */

static machine_t *M(sparc_bus_t *b) { return (machine_t *)b; }

static uint32_t io_get(machine_t *m, uint32_t off)
{
    return m->io[off / 4];
}

static void io_set(machine_t *m, uint32_t off, uint32_t v)
{
    m->io[off / 4] = v;
}

/* ---- LCM / display timing-generator raster (DP700WD_HW_REFERENCE.md §12.50) ----
 *
 * The CT909 DISP block runs a timing generator programmed by TGEN_TOTAL 0x1A38
 * = (Vtotal<<16)|Htotal (bit28 DISP_TGEN_EN) and SYNC_WH 0x1A3C (bit28
 * DISP_PSCAN_EN = progressive). It scans out one field per VSYNC period; the
 * live scan-line position is exposed as REG_DISP_MEM_LINE 0x1A68 (masked &0x7FF
 * for the line, bit28 DISP_EVEN_FIELD for parity). The firmware polls MEM_LINE
 * to sync raster-sensitive ops (spflash _VSYNCPolling waits MEM_LINE==0 at the
 * top of frame; gdi GDI_CHECK_DISP_MEM_LINE waits for the raster to leave the
 * rectangle being drawn). We derive the line from the same vsync_cnt/vsync_div
 * field clock that raises VSYNC (machine_cycle), so MEM_LINE==0 coincides
 * exactly with the VSYNC-pending assertion at the top of frame. */
static uint32_t disp_field_lines(machine_t *m)
{
    uint32_t tgen = io_get(m, R_DISP_TGEN_TOTAL);
    uint32_t vtot = (tgen >> 16) & 0x0FFFu;
    if (!vtot) vtot = 525u;                     /* NTSC frame total fallback */
    /* SYNC_WH DISP_PSCAN_EN clear => interlaced: one VSYNC = one field ~ Vtotal/2
     * lines; progressive => the field spans the whole Vtotal. */
    if (io_get(m, R_DISP_SYNC_WH) & DISP_PSCAN_EN)
        return vtot;
    vtot >>= 1;
    return vtot ? vtot : 1u;
}

/* Current scan line within the field (0 .. field_lines-1). */
static uint32_t disp_cur_line(machine_t *m)
{
    uint32_t flines = disp_field_lines(m);
    uint32_t div = m->vsync_div ? m->vsync_div : 1u;
    uint32_t line = (uint32_t)(((uint64_t)m->vsync_cnt * flines) / div);
    if (line >= flines) line = flines - 1u;
    return line;
}

static void log_access(machine_t *m, uint32_t addr, int is_write,
                       uint32_t val)
{
    int i;
    for (i = 0; i < m->log_n; i++)
        if (m->log[i].addr == addr) {
            if (is_write) { m->log[i].writes++; m->log[i].last_write = val; }
            else m->log[i].reads++;
            return;
        }
    if (m->log_n < MACH_LOG_MAX) {
        m->log[m->log_n].addr = addr;
        m->log[m->log_n].reads = is_write ? 0 : 1;
        m->log[m->log_n].writes = is_write ? 1 : 0;
        m->log[m->log_n].last_write = is_write ? val : 0;
        m->log_n++;
    }
}

static void uart_tx(machine_t *m, int ch, uint32_t v)
{
    (void)ch;
    if (m->uart_echo) {
        fputc((int)(v & 0xFF), stdout);
        fflush(stdout);
    }
    if (m->uart_file)
        fputc((int)(v & 0xFF), m->uart_file);
}

/* ---- I/O page ---- */

static uint32_t io_read(machine_t *m, uint32_t off)
{
    switch (off) {
    case R_UART1_STAT:
        /* TX always ready; RX ready iff bytes are queued (host -> device) */
        return UART_STAT_READY |
               ((m->rx_pos < m->rx_len) ? UART_STAT_DATA_READY : 0u);
    case R_UART2_STAT:
    case R_DSU_UART_STAT:
        return UART_STAT_READY;
    case R_UART1_DATA:
        /* pop one queued RX byte (or 0 if none) */
        if (m->rx_pos < m->rx_len)
            return m->rx_buf[m->rx_pos++];
        return 0;
    case R_UART2_DATA:
    case R_DSU_UART_DATA:
        return 0;                     /* RX not modeled on these ports */
    case R_TIMER3_VAL:
        return (uint32_t)m->t3_value;
    case R_PRESC_CNT:
        return m->presc_cnt;
    case R_INT_PENDING:
        return io_get(m, R_INT_PENDING);
    case R_P1_1ST_STCL:
        /* STATUS read: the live secondary pending register */
        return io_get(m, R_P1_1ST_PEND);
    case R_P1_2ND_STCL:
        return io_get(m, R_P1_2ND_PEND);
    case R_IIC_CMD:
        /* trigger/busy bit self-clears: transaction done immediately */
        return io_get(m, R_IIC_CMD) & ~IIC_BUSY;
    case 0x420c:
        /* IIC data-ready/done status (0x8000420c): the firmware's config-read
         * poll (flash 0x624f4) waits up to 999 ticks for bit2 (=0x4) to signal
         * the transfer completed before reading the data. Our IIC model completes
         * transactions instantly (R_IIC_CMD busy self-clears), so present bit2 set
         * -- otherwise every EEPROM/config read times out and the late boot stalls
         * in a 999-tick poll per byte (§12.72). Faithful: done immediately. */
        return io_get(m, 0x420c) | 0x4u;
    case R_VLD_STATUS:
        /* VLD entropy decode: report macroblock-ready so the JPEG decoder
         * thread's completion poll advances (the real pixels come from the
         * functional decode). */
        return io_get(m, R_VLD_STATUS) | VLD_MB_RDY;
    case R_VLD_MBINT:
        /* per-stage done bits (header/run-length/motion-comp/JPEG) */
        return io_get(m, R_VLD_MBINT) | VLD_DONE_BITS;
    case R_MCU_A0REM:
    case R_MCU_A1REM:
        /* Audio bitstream-buffer remainder: the audio-hang monitor treats a
         * frozen value as a dead DSP. Present a live, changing value so audio
         * looks alive. (Not the current menu blocker -- that is a config-apply
         * callback loop -- but correct modelling regardless.) */
        return (uint32_t)((~(m->cycles >> 6)) & 0x00FFFFFCu) | 4u;
    case 0x2a28:
        /* MCU BIU bit-stream read-channel status (BCR0A, §10.8). The JPEG worker
         * (0x4001fe90) polls bit 12 (0x1000) = "read channel drained / macroblock
         * stream consumed" before advancing to HALJPEG_Display. The JPU decode
         * kick (R_GPU_CTL0 JPU op) sets m->biu_drained once the frame has been
         * functionally decoded; present that as bit 12 so the poll retires
         * instead of spinning out the decode-wait timeout. */
        return io_get(m, 0x2a28) | (m->biu_drained ? 0x1000u : 0u);
    case 0xc10:
        /* Decoder progress/state word. The firmware's wait loop (flash
         * 0x72810) polls bits[20:16] for >=7. This is driven by the hardware
         * JPEG decoder consuming the staged bitstream; once the functional
         * decode has run (biu_drained) or the legacy arm is set, report >=7. */
        if (m->jpeg_decode_en || m->biu_drained) {
            machine_maybe_jpeg_decode(m);
            return (io_get(m, 0xc10) & ~0x001f0000u) | 0x00070000u;
        }
        log_access(m, 0x80000c10u, 0, 0);
        return io_get(m, 0xc10);
    case 0x407C: {
        /* ADCGLB: analog key-ladder ADC (panel.c PANEL_KeyScan reads bits
         * [31:24] as the key voltage). No key pressed => high rail (~0xFF).
         * CT952_ADC overrides the raw value for both reads. CT952_PANELKEY=
         * "<r0>,<r1>" is CHANNEL-AWARE: PANEL_KeyScan selects the ladder line by
         * writing bits [23:16] of ADCGLB (0x84 => RDATA0 line, 0xC4/0xE4 =>
         * RDATA1 line) before each read, so a real single keypress reads as a
         * voltage on ONE line and the idle rail on the other. Return r0<<24 when
         * the 0x84 line is selected and r1<<24 when the 0xC4/0xE4 line is,
         * defaulting to the 0xFF idle rail otherwise. */
        const char *pk = getenv("CT952_PANELKEY");
        if (pk) {
            /* "<r0>,<r1>[@<at>[,<len>]]": press the key (voltage r0 on the 0x84
             * ladder line, r1 on 0xC4) as a real EDGE -- idle before <at>, held for
             * <len> instructions, then released back to the 0xFF idle rail. The
             * input debounce (0x59c8c) needs an idle->press transition and the key
             * held consistently across a few 100ms scans, then a release, to latch
             * __bISRKey and fire the action -- a constant level never edges. */
            static int pk_init = 0; static uint32_t r0 = 0xFF, r1 = 0xFF;
            static uint64_t at = 0, len = 0;
            if (!pk_init) { pk_init = 1; char b[80]; strncpy(b, pk, 79); b[79] = 0;
                char *a = strchr(b, '@');
                if (a) { *a = 0; char *l = strchr(a + 1, ',');
                    if (l) { *l = 0; len = strtoull(l + 1, NULL, 0); }
                    at = strtoull(a + 1, NULL, 0); }
                char *c = strchr(b, ','); if (c) { *c = 0; r1 = (uint32_t)strtoul(c + 1, NULL, 0); }
                r0 = (uint32_t)strtoul(b, NULL, 0);
                if (at && !len) len = 10000000ull;   /* default ~press duration */
            }
            int pressed = !at || (m->cpu.icount >= at &&
                                  (!len || m->cpu.icount < at + len));
            if (pressed) {
                uint32_t chan = (io_get(m, 0x407Cu) >> 16) & 0xFF;
                if (chan == 0x84u) return (r0 & 0xFF) << 24;
                if (chan == 0xC4u || chan == 0xE4u) return (r1 & 0xFF) << 24;
            }
            return 0xFF000000u;   /* released / idle rail */
        }
        const char *e = getenv("CT952_ADC");
        return e ? (uint32_t)strtoul(e, NULL, 0) : 0xFF000000u;
    }
    case R_DISP_MEM_LINE: {
        /* Live main-display read-frame-buffer line position (§12.50). When the
         * timing generator is disabled the block is quiescent -- return the
         * last-written value (the firmware's own poll idioms then see the
         * static 0). When enabled, sweep 0..field_lines-1 each VSYNC period and
         * flag even/odd field parity, so any wait-for-line / wait-for-vblank
         * sees real motion and MEM_LINE==0 lines up with the top-of-frame VSYNC. */
        uint32_t en = io_get(m, R_DISP_TGEN_TOTAL) & DISP_TGEN_EN;
        uint32_t val = en ? ((disp_cur_line(m) & 0x7FFu) |
                             (m->disp_field ? DISP_EVEN_FIELD : 0u))
                          : io_get(m, R_DISP_MEM_LINE);
        if (getenv("CT952_MLTRACE")) {
            static int mt; if (mt < 80) { fprintf(stderr,
                "[MLrd] MEM_LINE=%08x en=%u tgen=%08x pc=%08x icount=%llu\n",
                val, en?1u:0u, io_get(m, R_DISP_TGEN_TOTAL), m->cpu.pc,
                (unsigned long long)m->cpu.icount); mt++; }
        }
        return val;
    }
    case R_DISP_DMA_CUR0:
    case R_DISP_DMA_CUR1: {
        /* Live display-DMA current-scan address (§12.50). The display-enable
         * safe-scan check (flash 0x3fa40) reads these to confirm the scan-out is
         * not mid-active before reconfiguring. Reflect a live pointer that walks
         * from the DMA start toward the end in step with the raster line, but
         * only while the timing generator is enabled AND a real DRAM start
         * pointer is programmed -- otherwise return the last-written value so the
         * shared 0x80000E00 DMA-descriptor block (also used by non-display DMA)
         * is left untouched. */
        if (!(io_get(m, R_DISP_TGEN_TOTAL) & DISP_TGEN_EN))
            return io_get(m, off);
        uint32_t start = io_get(m, R_DISP_DMA_START);
        uint32_t end   = io_get(m, R_DISP_DMA_END);
        if (start < 0x40000000u || end <= start)
            return io_get(m, off);
        uint32_t flines = disp_field_lines(m);
        uint32_t span   = end - start;
        uint32_t cur    = start +
            (uint32_t)(((uint64_t)disp_cur_line(m) * span) / (flines ? flines : 1u));
        return cur & ~3u;
    }
    default:
        log_access(m, 0x80000000u + off, 0, 0);
        return io_get(m, off);
    }
}

/* ---- GPU 2-D engine (ctkav_gpu.h offsets; programming per gdi.c) ---- */
#define R_GPU_CTL0     0x2880
#define R_GPU_CTL1     0x2884
#define R_GPU_COL_NDX  0x2888
#define R_GPU_OP_SIZE  0x288C
#define R_GPU_AG_OFF   0x2890
#define R_GPU_SRC_ADDR 0x2894
#define R_GPU_DEST     0x2898
#define R_GPU_FONT_ADR 0x289C
#define R_GPU_FONT_CFG 0x28A0
#define R_GPU_FONT_IDX 0x28A8
#define GPU_START_BIT  0x2u
#define GPU_STATUS_BIT 0x200u        /* CTL0[9] busy */
#define GPU_FONT_1BIT  0x20u         /* CTL0[5] */
/* The 0x2880 block is shared JPU/GPU (ctkav_jpu.h): JPU_GPU_OP (CTL[28])
 * selects which register set is live. JPU ops signal completion by clearing
 * JPU_BUSY (CTL[0]); the JPEG decoder thread spins on `while (CTRL & 1)`. */
#define JPU_GPU_OP     0x10000000u   /* CTL[28]: 1=GPU op, 0=JPU op */
#define JPU_GO_BIT     0x2u          /* CTL[1] */
#define JPU_BUSY_BIT   0x1u          /* CTL[0] */

static uint8_t *dram_rw(machine_t *m, uint32_t addr, uint32_t span)
{
    uint32_t off;
    if (addr < 0x40000000u) return NULL;
    off = addr - 0x40000000u;
    if ((uint64_t)off + span > MACH_DRAM_SIZE) return NULL;
    return m->dram + off;
}

/* Execute one GPU op triggered by a CTL0 write with GPU_START. Fill and
 * 1-bit font expansion into the 8bpp OSD plane; the firmware's UI drawing
 * (gdi.c) programs these. Row stride comes from AG_OFF (CT909P encoding:
 * ((ag_width<<8)+ag_offset)<<16, both in 8-byte units, so bytes/row =
 * (ag_width+ag_offset-1)*8). */
static void gpu_exec(machine_t *m, uint32_t ctl0)
{
    uint32_t sz = io_get(m, R_GPU_OP_SIZE);
    uint32_t w = sz & 0xFFFF, h = (sz >> 16) & 0x7FF;
    uint32_t dest = io_get(m, R_GPU_DEST);
    /* Row pitch of the destination plane. gdi.c GDI_SetGpuAddr derives
     *   ag_width  = (op_width + (addr&3) + 3) >> 2
     *   ag_offset = ((OSD_width+3)>>2) - ag_width + 1     (REG_GPU_AG_OFF>>16)
     * so that ag_width+ag_offset-1 == (OSD_width+3)>>2 is a CONSTANT plane
     * pitch regardless of the op's own width. Reconstruct it the same way:
     * take ag_offset from the register and ag_width from OP_SIZE.w, so a
     * width-set draw (w!=0) lands on the same physical plane pitch as a
     * full-width (w==0) draw instead of shearing. */
    uint32_t ago = (io_get(m, R_GPU_AG_OFF) >> 16) & 0xFFFF;
    uint32_t agw = (w + (dest & 3) + 3) >> 2;
    uint32_t stride = (agw + ago > 1) ? (agw + ago - 1) * 8u : 616u;
    uint32_t opmode = (ctl0 >> 2) & 0x7;

    if (m->gpu_ops == 0 && getenv("CT952_TRACE"))
        fprintf(stderr, "[GPU-FIRST] first GPU op at icount=%llu pc=%08x "
                "(UI drawing started -> menu reached)\n",
                (unsigned long long)m->cpu.icount, m->cpu.pc);
    m->gpu_ops++;
    if (ctl0 & GPU_FONT_1BIT) m->gpu_font_ops++; else m->gpu_mode_ops[opmode]++;

    if (ctl0 & GPU_FONT_1BIT) {
        /* 1-bit font expansion into the 8bpp OSD plane. Glyph table at
         * FONT_ADDR, each glyph = capacity DWs (glyph_DW*4 bytes/row,
         * MSB-first 1-bit rows). FONT_CONFIG = width_DW<<24 | len<<16 |
         * capacity. COL_NDX low byte = fg index, next byte = bg. Height
         * from OP_SIZE[26:16]; glyph advance = glyph_DW*8 pixels. */
        uint32_t cfg = io_get(m, R_GPU_FONT_CFG);
        uint32_t fbase = io_get(m, R_GPU_FONT_ADR);
        uint32_t wdw = (cfg >> 24) & 0xFF; if (!wdw) wdw = 1;
        uint32_t cap = cfg & 0xFFF;       /* DW per glyph */
        uint32_t gh = h ? h : (cap / wdw);   /* glyph height in rows */
        uint32_t adv = wdw * 8;              /* pixel advance per glyph */
        uint8_t fg = io_get(m, R_GPU_COL_NDX) & 0xFF;
        uint8_t bg = (io_get(m, R_GPU_COL_NDX) >> 8) & 0xFF;
        uint32_t xoff = 0;
        int gi;
        if (!gh) gh = 16;
        for (gi = 0; gi < m->gpu_fontn; gi++) {
            uint32_t gnum = m->gpu_fontq[gi] & 0x1FF;
            uint32_t gaddr = fbase + gnum * cap * 4u;
            const uint8_t *gp = dram_rw(m, gaddr, cap * 4u);
            uint32_t row, col;
            if (!gp) { xoff += adv; continue; }
            for (row = 0; row < gh; row++) {
                for (col = 0; col < adv; col++) {
                    uint32_t bytei = (col >> 3);
                    uint8_t rb = gp[row * wdw * 4u + bytei];
                    uint8_t bit = (rb >> (7 - (col & 7))) & 1;
                    uint32_t px = dest + row * stride + xoff + col;
                    uint8_t *d = dram_rw(m, px, 1);
                    if (d) *d = bit ? fg : bg;
                }
            }
            xoff += adv;
        }
        m->gpu_fontn = 0;
        return;
    }

    if (opmode == 6 && !(ctl0 & GPU_FONT_1BIT)) {   /* GPU_FILLRECTANGLE */
        uint8_t color = (io_get(m, R_GPU_CTL1) >> 24) & 0xFF;
        uint8_t *fb;
        uint32_t r, c;
        if (!w || !h) return;
        fb = dram_rw(m, dest, (h - 1) * stride + w);
        if (!fb) return;
        for (r = 0; r < h; r++)
            for (c = 0; c < w; c++)
                fb[r * stride + c] = color;
    }
    m->gpu_fontn = 0;   /* consume the font-index queue */
}

static void io_write(machine_t *m, uint32_t off, uint32_t v)
{
    /* Display bring-up trace (CT952_DISPTRACE): log writes to the OSD
     * size/enable register (0x1a54, bit28=OSD_EN), the OSD region regs
     * (0x1a40..0x1a5c), and the GAM_OSD palette (0x1c00..0x1cff) with PC +
     * icount + value -- shows whether the firmware ever ATTEMPTS OSD-enable /
     * palette-load and where the display bring-up stalls (§12.1). */
    /* Palette-write trace (CT952_PALTRACE): every write to the GAM_OSD window
     * 0x1c00..0x1ffc, with the OSD_CR access-mode bit and icount -- to see how
     * the OSD palette RAM is actually loaded (address-mapped vs auto-increment
     * data port) and separate palette from gamma. */
    if (getenv("CT952_PALTRACE") && off >= 0x1c00u && off <= 0x1ffcu) {
        static int pt; if (pt < 1024) { pt++;
            fprintf(stderr, "[PAL] +%03x <- %08x  osdcr=%08x pc=%08x icount=%llu\n",
                    off, v, io_get(m, 0x1a58u), m->cpu.pc,
                    (unsigned long long)m->cpu.icount);
        }
    }
    if (getenv("CT952_DISPTRACE") &&
        ((off >= 0x1a40u && off <= 0x1a5cu) || (off >= 0x1c00u && off <= 0x1cffu))) {
        static int dt; if (dt < 120) {
            fprintf(stderr, "[DISP] +%03x <- %08x  %s pc=%08x icount=%llu\n",
                    off, v, (off == 0x1a54u && (v & 0x10000000u)) ? "OSD_EN!" :
                            (off >= 0x1c00u ? "pal" : ""),
                    m->cpu.pc, (unsigned long long)m->cpu.icount);
            dt++;
        }
    }
    switch (off) {
    case R_GPU_CTL0:
        if (v & JPU_GPU_OP) {
            /* GPU 2-D op (font/fill): run it, clear the GPU busy bit (CTL[9]) */
            if (v & GPU_START_BIT) gpu_exec(m, v);
            io_set(m, off, v & ~GPU_STATUS_BIT);
        } else {
            /* JPU op (scale/decode/fill): the actual pixels are produced by the
             * functional JPEG decode; here just complete the handshake so the
             * decoder thread's `while (REG_JPU_CTRL & 1)` exits. */
            io_set(m, off, v & ~JPU_BUSY_BIT);
            m->jpu_active_until = m->cycles + 300000u;   /* decode-active window */
            /* Faithful decode-completion: a JPU decode op means the hardware
             * decoder is processing/finishing a frame -> it reaches MODE_STOP(0x10)
             * = frame-done. The decode-status getter (0x375a0 action 0) maps 0x10
             * to JPEG_STATUS_OK. JPU decode ops run only once the firmware is past
             * the boot decoder-stop handshake (which runs before any decode), so
             * gate-3's 0x11 is unaffected. */
            m->vdec_frame_done = 1;
            /* Faithful JPU decode (§10.8): on the JPU decode kick, functionally
             * decode the staged JPEG and emit the tiled-YUV frame the real MCU-BIU
             * write-channel would produce. Guarded by jpeg_sig so it decodes
             * once/frame. Then mark the MCU-BIU read channel drained so the
             * worker's BCR0A poll (io 0x2a28 bit 12) retires and it advances to
             * HALJPEG_Display. Formerly gated behind CT952_LOGODECODE (crutch). */
            { uint32_t jc_before = m->jpeg_count;
              machine_maybe_jpeg_decode(m);
              m->biu_drained = 1;
            /* EXPERIMENT (CT952_DECDONE): on an ACTUAL new-frame completion
             * (jpeg_count advanced), raise the PROC1-1st DECODE-DONE interrupt
             * -- RL_DONE(0x20)/MC_DONE(0x40)/INT_16L(0x80) the MCU-BIU write-
             * channel asserts as it drains reconstructed macroblocks. §12.29
             * tested only PROC1-2nd (line 10) BIU/MCU sources -- NEGATIVE; this
             * is the untested faithful variant on line 13 (which §12.41 proved
             * is unmasked at the park). Never touches VSYNC (bit0), which the
             * firmware masks deliberately (§12.41). Probes whether the display/
             * decode ISR's DSR posts the CC-event completion the boot starves on. */
              if (getenv("CT952_DECDONE") && m->jpeg_count != jc_before) {
                uint32_t bit = 0xE0u;   /* RL_DONE|MC_DONE|INT_16L */
                io_set(m, R_P1_1ST_MASK, io_get(m, R_P1_1ST_MASK) | bit);
                io_set(m, R_P1_1ST_PEND, io_get(m, R_P1_1ST_PEND) | bit);
                if (getenv("CT952_DECIRQ_TRACE"))
                    fprintf(stderr, "[DECDONE] frame#%d -> P1_1ST bit 0x%x (mask %08x) icount=%llu\n",
                            m->jpeg_count, bit, io_get(m, R_P1_1ST_MASK),
                            (unsigned long long)m->cpu.icount);
              } }
            /* EXPERIMENT (CT952_DECIRQ=<P1_2ND bit mask>): raise a PROC1-2nd
             * decoder/BIU interrupt (BIU=0x10, MCU_BSRD=0x20, ...) on JPU
             * completion + unmask it, to run the firmware's decoder DSR.
             * RESULT (§12.29): NEGATIVE -- bits 0x10..0x1f0 delivered 68x each
             * do NOT advance the boot. The power-on stage does not gate on a
             * decode IRQ; it gates on a decode-STATUS POLL (0x36ff0->0x375a0
             * action 3) over the decode handle 0x40022f88, which is NULL (its
             * setup at ~0x55bd0 never runs). Kept env-gated as a documented
             * negative probe. */
            { const char *e = getenv("CT952_DECIRQ");
              if (e) { uint32_t bit = (uint32_t)strtoul(e, NULL, 0);
                io_set(m, R_P1_2ND_MASK, io_get(m, R_P1_2ND_MASK) | bit);
                io_set(m, R_P1_2ND_PEND, io_get(m, R_P1_2ND_PEND) | bit);
                if (getenv("CT952_DECIRQ_TRACE"))
                    fprintf(stderr, "[DECIRQ] raised P1_2ND bit 0x%x (mask now %08x) icount=%llu\n",
                            bit, io_get(m, R_P1_2ND_MASK), (unsigned long long)m->cpu.icount);
              } }
        }
        return;
    case 0x2a20:
        /* MCU BIU bit-stream read-channel source (BCR08, §10.8). The JPEG worker
         * (0x4001fe90) programs the bit-stream base here before kicking the JPU.
         * Wire it to the functional decoder's source so the decode reads exactly
         * the bitstream the firmware staged, and reset the drained flag: a new
         * source means a new fill is in flight, so BCR0A bit 12 must read 0 until
         * the JPU kick drains it. Only DRAM pointers (0x4xxxxxxx) are meaningful
         * as a bit-stream base; ignore other writes (control/config aliases). */
        if ((v & 0xF0000000u) == 0x40000000u) m->jpeg_src = v;
        m->biu_drained = 0;
        /* JPU-source selection trace (CT952_JSRCBT): who programs the display
         * photo pointer, and its caller chain -- to find the "current photo"
         * selection that picks 0x401dc000 (demo) but never the card 0x401ec000. */
        if (getenv("CT952_JSRCBT")) {
            uint32_t bt[32]; int nb = sparc_win_backtrace(&m->cpu, bt, 32), bi;
            static int jn; if (jn < 30) {
                fprintf(stderr, "[JSRCBT] 0x2a20 <- %08x pc=%08x winframes:", v, m->cpu.pc);
                for (bi = 0; bi < nb; bi++) fprintf(stderr, " %08x", bt[bi]);
                fprintf(stderr, " icount=%llu\n", (unsigned long long)m->cpu.icount); jn++; }
        }
        io_set(m, off, v);
        return;
    case R_GPU_FONT_IDX:
        if (m->gpu_fontn < 1024) m->gpu_fontq[m->gpu_fontn++] = (uint16_t)v;
        io_set(m, off, v);
        return;
    case R_UART1_DATA: uart_tx(m, 1, v); return;
    case R_UART2_DATA: uart_tx(m, 2, v); return;
    case R_DSU_UART_DATA: uart_tx(m, 3, v); return;
    case R_TIMER1_CNT:
    case R_TIMER1_RLD: {
        /* Time compression (CT952_TICK_MULT): the eCos system tick is TIMER1
         * (IRQ bit 0x100 = L8). Many boot-thread decoder-state polls with PROC2
         * held in reset are timeout-bound (2999/30015 ticks ~ billions of
         * instructions). Scaling the TIMER1 reload/count down by N makes the
         * tick advance N x faster, so those timeouts fire in 1/N the
         * instructions and the boot progresses through the chain in a runnable
         * budget -- relative firmware timing is preserved. */
        static int mult = -1;
        if (mult < 0) { const char *e = getenv("CT952_TICK_MULT");
                        mult = e ? atoi(e) : 1; if (mult < 1) mult = 1; }
        if (mult > 1) { v = v / (uint32_t)mult; if (!v) v = 1; }
        io_set(m, off, v);
        return;
    }
    case R_TIMER1_CTL:
    case R_TIMER2_CTL: {
        uint32_t cnt_off = (off == R_TIMER1_CTL) ? R_TIMER1_CNT : R_TIMER2_CNT;
        uint32_t rld_off = (off == R_TIMER1_CTL) ? R_TIMER1_RLD : R_TIMER2_RLD;
        io_set(m, off, v);
        if (v & TIMER_LOAD)
            io_set(m, cnt_off, io_get(m, rld_off));
        return;
    }
    case R_INT_CLEAR:
        /* write-1-to-clear pending + force (LEON) */
        io_set(m, R_INT_PENDING, io_get(m, R_INT_PENDING) & ~v);
        io_set(m, R_INT_FORCE, io_get(m, R_INT_FORCE) & ~v);
        return;
    case R_P1_1ST_STCL:
        /* secondary CLEAR: write-1-to-clear pending bits */
        io_set(m, R_P1_1ST_PEND, io_get(m, R_P1_1ST_PEND) & ~v);
        return;
    case R_P1_1ST_MDIS:
        /* secondary MASK_DISABLE: clear the named enable bits */
        if (getenv("CT952_VSMTRACE"))
            fprintf(stderr, "[VSM] MDIS clear %08x  mask %08x->%08x  vsync(bit0)%s  pc=%08x icount=%llu\n",
                    v, io_get(m, R_P1_1ST_MASK), io_get(m, R_P1_1ST_MASK) & ~v,
                    (v & 1) ? " CLEARED" : "", m->cpu.pc, (unsigned long long)m->cpu.icount);
        io_set(m, R_P1_1ST_MASK, io_get(m, R_P1_1ST_MASK) & ~v);
        return;
    case R_P1_1ST_MASK:
        /* direct MASK_ENABLE write (VSYNC-mask trace, §12.41) */
        if (getenv("CT952_VSMTRACE"))
            fprintf(stderr, "[VSM] MASK<-%08x  vsync(bit0)%s  pc=%08x icount=%llu\n",
                    v, (v & 1) ? " ENABLED" : " off", m->cpu.pc,
                    (unsigned long long)m->cpu.icount);
        io_set(m, R_P1_1ST_MASK, v);
        return;
    case R_P1_2ND_STCL:
        /* PROC1-2nd CLEAR: write-1-to-clear pending bits */
        io_set(m, R_P1_2ND_PEND, io_get(m, R_P1_2ND_PEND) & ~v);
        return;
    case R_P1_2ND_MDIS:
        io_set(m, R_P1_2ND_MASK, io_get(m, R_P1_2ND_MASK) & ~v);
        return;
    case R_PARAM1:
        /* AM mailbox: PROC1 writes cmd with [31:30]=1 write / 2 read;
         * PROC2 acks by clearing [31:30] (hdecoder.c:1718-1727).
         * Stand-in DSP: ack immediately; reads return 0 via PARAM2.
         * Skipped once the real PROC2 core is running (it acks for real). */
        if (!m->proc2_on) {
            if ((v >> 30) == 2)
                io_set(m, R_PARAM2, 0);
            io_set(m, R_PARAM1, v & 0x3FFFFFFFu);
            return;
        }
        io_set(m, R_PARAM1, v);
        return;
    case R_AUDIO_CMD:
        /* PROC1 writes 0x10003, then spins reading this word and shifting
         * right 16; it breaks when [31:16] == 0 (hdecoder.c:724-737).
         * PROC2 signals "audio boot OK" by clearing the high half. The
         * stand-in acks instantly; the real PROC2 core does it itself. */
        io_set(m, R_AUDIO_CMD, m->proc2_on ? v : (v & 0xFFFFu));
        return;
    default:
        log_access(m, 0x80000000u + off, 1, v);
        io_set(m, off, v);
        return;
    }
}

/* ---- bus ---- */

static uint32_t mem_read_raw(const uint8_t *p, int size)
{
    /* big-endian memory image */
    if (size == 1) return p[0];
    if (size == 2) return ((uint32_t)p[0] << 8) | p[1];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void mem_write_raw(uint8_t *p, uint32_t v, int size)
{
    if (size == 1) { p[0] = (uint8_t)v; return; }
    if (size == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* PROC2 vdec command -> completion-state ack (comdec.h EN_VDEC_CMD).
 * The decoder microcode overwrites REG_SRAM_PLAYMODE with these once it
 * has consumed the command; PROC1 wait loops poll for them. */
static uint8_t proc2_ack_of(uint8_t cmd)
{
    switch (cmd) {
    case 0x00: return 0x10;   /* reset/NONE     -> decoder idle=STOP */
    case 0x10: return 0x11;   /* MODE_STOP      -> MODE_STOPPED      */
    case 0x40: return 0x12;   /* MODE_SCAN      -> MODE_SCAN_DONE    */
    case 0x80: return 0x13;   /* MODE_PREDECODE -> MODE_PREDEC_DONE  */
    default:   return cmd;    /* PLAY/...: state stays as set        */
    }
}

/* ---- EHCI USB host controller model (base 0xa0000100) ------------------------
 * Stage 1 goal: let the retail Jungo/BSD EHCI driver's init + root-hub scan
 * COMPLETE with zero devices attached, so the boot advances past USB init.
 * Layout: capability regs packed at base (CAPLENGTH byte0 + HCIVERSION bytes2-3
 * in word 0; HCSPARAMS word 1); operational regs at base+CAPLENGTH(0x10). The
 * fw's byte/half accessor extracts LE lanes from word 0, so word 0 must read
 * back as 0x01000010 (CAPLENGTH=0x10, HCIVERSION=0x0100). Word regs are raw.
 * Offsets below are relative to EHCI_BASE. */
#define EHCI_BASE     0xA0000100u
#define EHCI_END      0xA0000200u
#define EHCI_NPORTS   1u

static uint32_t ehci_read(machine_t *m, uint32_t off)
{
    switch (off) {
    case 0x00: return 0x01000010u;              /* HCIVERSION<<16 | rsvd | CAPLENGTH */
    case 0x04: return EHCI_NPORTS;              /* HCSPARAMS: N_PORTS in [3:0]        */
    case 0x08: return 0x00000000u;              /* HCCPARAMS: 32-bit, no EECP         */
    case 0x0C: return 0x00000000u;              /* HCSP_PORTROUTE                     */
    /* operational registers at base+CAPLENGTH (0x10) */
    case 0x10: return m->ehci_usbcmd & ~0x2u;   /* USBCMD (HCRESET self-clears)       */
    case 0x14:                                  /* USBSTS: HCHalted(0x1000)=!running  */
        return (m->ehci_usbcmd & 1u) ? (m->ehci_usbsts & ~0x1000u)
                                     : (m->ehci_usbsts | 0x1000u);
    case 0x18: return m->ehci_usbintr;          /* USBINTR                            */
    case 0x1C: return (uint32_t)((m->cycles >> 10) & 0x3FFFu); /* FRINDEX advancing   */
    case 0x20: return m->ehci_ctrldss;          /* CTRLDSSEGMENT                      */
    case 0x24: return m->ehci_periodic;         /* PERIODICLISTBASE                   */
    case 0x28: return m->ehci_async;            /* ASYNCLISTADDR                      */
    case 0x50: return m->ehci_configflag;       /* CONFIGFLAG                         */
    case 0x54: return m->ehci_portsc[0];        /* PORTSC[0]: no connect -> 0         */
    default:   return 0;
    }
}

static void ehci_write(machine_t *m, uint32_t off, uint32_t val)
{
    switch (off) {
    case 0x10: m->ehci_usbcmd = val & ~0x2u; break;      /* reset completes instantly */
    case 0x14: m->ehci_usbsts &= ~(val & 0x3Fu); break;  /* W1C interrupt bits         */
    case 0x18: m->ehci_usbintr = val; break;
    case 0x1C: m->ehci_frindex = val; break;
    case 0x20: m->ehci_ctrldss = val; break;
    case 0x24: m->ehci_periodic = val; break;
    case 0x28: m->ehci_async = val; break;
    case 0x50: m->ehci_configflag = val; break;
    case 0x54: m->ehci_portsc[0] = val & ~0x2Au; break;  /* drop W1C change bits; no dev */
    default: break;
    }
}

/* ---- SD Host Controller model (standard SDHC spec, base 0xa0001100) ---------
 * Presents m->sd_img (a FAT card image) as an inserted SDHC card. The retail
 * SDC driver (card.a/sdc.o) does CMD0/8/ACMD41/2/3/9/7/ACMD6/CMD6 init then
 * CMD18 multi-block DMA reads into DRAM; we serve blocks from sd_img and drive
 * the INT_STAT CMD_COMPLETE/TRAN_COMPLETE handshake. Register offsets are from
 * ctkav_sdc.h; SDC_BASE low 8 bits below (0x00..0xfc). §12.94. */
#define SDC_BASE_ADDR 0xA0001100u
/* INT_STAT bits */
#define SDCI_CMD_COMPLETE   (1u<<16)
#define SDCI_TRAN_COMPLETE  (1u<<17)
#define SDCI_DMA_INT        (1u<<19)
#define SDCI_BUF_RD_RDY     (1u<<21)
#define SDCI_CARD_INS       (1u<<22)
/* STAT bits */
#define SDCS_CARD_INS       (1u<<16)
#define SDCS_STABLE         (1u<<17)
#define SDCS_CD_PIN         (1u<<18)

/* fill sdc_resp[0..3] from a 16-byte CID/CSD (big-endian, [0]=bits127:120);
 * the SDHC R2 response register holds CID/CSD[127:8] (CRC byte dropped). */
static void sdc_resp_r2(machine_t *m, const uint8_t *b)
{
    m->sdc_resp[0] = ((uint32_t)b[11]<<24)|((uint32_t)b[12]<<16)|((uint32_t)b[13]<<8)|b[14];
    m->sdc_resp[1] = ((uint32_t)b[7]<<24)|((uint32_t)b[8]<<16)|((uint32_t)b[9]<<8)|b[10];
    m->sdc_resp[2] = ((uint32_t)b[3]<<24)|((uint32_t)b[4]<<16)|((uint32_t)b[5]<<8)|b[6];
    m->sdc_resp[3] = ((uint32_t)b[0]<<16)|((uint32_t)b[1]<<8)|b[2];
}

/* copy `len` bytes from the card image at byte offset `src` to DRAM at `dst`. */
static void sdc_dma_to_dram(machine_t *m, uint32_t dst, uint32_t src, uint32_t len)
{
    uint8_t *d = machine_dram_ptr(m, dst);
    uint32_t i;
    if (!d) return;
    for (i = 0; i < len; i++)
        d[i] = (m->sd_img && src + i < m->sd_size) ? m->sd_img[src + i] : 0;
}

/* Deliver a small read-data block for the command: DMA to DRAM if TRAN_MODE
 * selects DMA, else stage it for PIO DATA_PORT reads (BUFF_READ_RDY). */
static void sdc_data_block(machine_t *m, const uint8_t *buf, uint32_t len,
                           uint32_t tran_mode)
{
    if (tran_mode & 1) {                 /* DMA */
        uint8_t *d = machine_dram_ptr(m, m->sdc_reg[0]);
        if (d) { uint32_t i; for (i = 0; i < len; i++) d[i] = buf[i]; }
        m->sdc_int_stat |= SDCI_TRAN_COMPLETE;
    } else {                             /* PIO via DATA_PORT */
        if (len > sizeof m->sdc_data) len = sizeof m->sdc_data;
        memcpy(m->sdc_data, buf, len);
        m->sdc_data_len = len; m->sdc_data_pos = 0;
        m->sdc_int_stat |= SDCI_BUF_RD_RDY;
    }
}

/* Execute an SD command written to REG_SDC_CMD. */
static void sdc_do_cmd(machine_t *m, uint32_t cmd_reg, uint32_t tran_mode)
{
    uint32_t idx = (cmd_reg >> 8) & 0x3f;
    uint32_t arg = m->sdc_reg[2];                 /* 0x08 ARG */
    int acmd = m->sdc_acmd;
    m->sdc_acmd = 0;
    m->sdc_resp[0] = 0x00000900;                  /* default R1: card status ready(tran) */
    m->sdc_int_stat |= SDCI_CMD_COMPLETE;

    if (acmd) {
        switch (idx) {
        case 41: /* ACMD41 SD_SEND_OP_COND: OCR, ready + CCS=1 (SDHC/block addr) */
            m->sdc_resp[0] = 0xC0FF8000u; return;
        case 6:  /* ACMD6 SET_BUS_WIDTH */ return;
        case 51: { /* ACMD51 SEND_SCR: 8-byte SCR (SD spec v2, 1+4-bit bus) */
            static const uint8_t scr[8] = {0x02,0x35,0x80,0x00,0x00,0x00,0x00,0x00};
            sdc_data_block(m, scr, 8, tran_mode); return; }
        case 13: { /* ACMD13 SD_STATUS: 64 bytes, zero-filled */
            uint8_t z[64]; memset(z, 0, sizeof z);
            sdc_data_block(m, z, 64, tran_mode); return; }
        default: return;
        }
    }

    switch (idx) {
    case 0:  /* GO_IDLE */ break;
    case 8:  /* SEND_IF_COND: R7 echoes voltage(0x1)+check pattern(0xAA) */
        m->sdc_resp[0] = arg & 0xFFF; break;
    case 55: /* APP_CMD */ m->sdc_acmd = 1; break;
    case 2: { /* ALL_SEND_CID: R2 */
        static const uint8_t cid[16] = {0x03,'S','D','E','M','U','0','0',
                                        0x10,0x12,0x34,0x56,0x78,0x01,0x40,0x01};
        sdc_resp_r2(m, cid); break; }
    case 3:  /* SEND_RELATIVE_ADDR: R6 = (RCA<<16)|status */
        m->sdc_rca = 1; m->sdc_resp[0] = (m->sdc_rca << 16) | 0x0500; break;
    case 9: { /* SEND_CSD: R2, CSD v2 (SDHC), C_SIZE=0x1FFF => 4GB */
        static const uint8_t csd[16] = {0x40,0x0E,0x00,0x32,0x5B,0x59,0x00,0x00,
                                        0x1F,0xFF,0x7F,0x80,0x0A,0x40,0x00,0x01};
        sdc_resp_r2(m, csd); break; }
    case 7:  /* SELECT_CARD: R1b */ break;
    case 16: /* SET_BLOCKLEN */ break;
    case 12: /* STOP_TRANSMISSION: R1b */ break;
    case 13: /* SEND_STATUS: R1 */ break;
    case 6: { /* SWITCH_FUNC: R1 + 64-byte switch-status block */
        uint8_t sw[64]; memset(sw, 0, sizeof sw);
        sw[13] = 0x01;                    /* function group 1 = high-speed supported */
        sw[16] = 0x01;                    /* selected function group 1 = 1 */
        sdc_data_block(m, sw, 64, tran_mode); break; }
    case 17: case 18: { /* READ_SINGLE/MULTIPLE_BLOCK: DMA from card image */
        uint32_t blkcnt = (idx == 17) ? 1u : (m->sdc_reg[1] & 0xffff);   /* 0x06 BLK_COUNT */
        uint32_t blksz  = (m->sdc_reg[1] >> 16) & 0xfff;                 /* 0x04 BLK_SIZE low 12b */
        uint32_t dma    = m->sdc_reg[0];                                 /* 0x00 DMA_ADDR */
        if (!blksz) blksz = 512;
        if (!blkcnt) blkcnt = 1;
        /* CCS=1 (SDHC) => ARG is a block number */
        sdc_dma_to_dram(m, dma, arg * 512u, blkcnt * blksz);
        m->sdc_int_stat |= SDCI_TRAN_COMPLETE;
        if (getenv("CT952_SDCTRACE"))
            fprintf(stderr, "[SDC] READ%d sector=%u count=%u -> dram=%08x pc=%08x icount=%llu\n",
                    idx, arg, blkcnt, dma, m->cpu.pc, (unsigned long long)m->cpu.icount);
        { const char *bt_e = getenv("CT952_SDCBT");
          if (bt_e && dma == (uint32_t)strtoul(bt_e, NULL, 0)) {
            uint32_t bt[32]; int nb = sparc_win_backtrace(&m->cpu, bt, 32), bi;
            fprintf(stderr, "[SDCBT] READ%d sector=%u count=%u dram=%08x winframes:",
                    idx, arg, blkcnt, dma);
            for (bi = 0; bi < nb; bi++) fprintf(stderr, " %08x", bt[bi]);
            fprintf(stderr, " icount=%llu\n", (unsigned long long)m->cpu.icount);
            { int k; fprintf(stderr, "[SDCBT] recent-PC ring:");
              for (k = 24; k < 64; k++)
                  fprintf(stderr, " %08x", m->cpu.pc_ring[(m->cpu.pc_ri + k) & 63]);
              fprintf(stderr, "\n"); }
          } }
        break; }
    default: break;
    }
}

static uint32_t sdc_read(machine_t *m, uint32_t off)
{
    switch (off & 0xfc) {
    case 0x10: return m->sdc_resp[0];
    case 0x14: return m->sdc_resp[1];
    case 0x18: return m->sdc_resp[2];
    case 0x1c: return m->sdc_resp[3];
    case 0x20: { /* DATA_PORT: serve staged PIO read data, word at a time */
        uint32_t v = 0, i;
        for (i = 0; i < 4; i++)
            if (m->sdc_data_pos < m->sdc_data_len)
                v |= (uint32_t)m->sdc_data[m->sdc_data_pos++] << (24 - i * 8);
        if (m->sdc_data_pos >= m->sdc_data_len) {  /* buffer drained: transfer done */
            m->sdc_int_stat &= ~SDCI_BUF_RD_RDY;
            m->sdc_int_stat |= SDCI_TRAN_COMPLETE;
        }
        return v; }
    case 0x24: { /* STAT: card inserted, stable, CD pin low(present); not busy */
        /* Delayed-insert (CT952_SDCARD_AT=<icount>): model the user inserting the
         * card AFTER power-on -- report "no card" until <icount>, giving a clean
         * insert EDGE once the media subsystem is running (tests whether a
         * present-from-boot card is missed because its edge is processed too
         * early). */
        static long ins_at = -2;
        if (ins_at == -2) { const char *e = getenv("CT952_SDCARD_AT");
                            ins_at = e ? (long)strtoull(e, NULL, 0) : -1; }
        if (!m->sd_img) return 0;
        if (ins_at >= 0 && m->cpu.icount < (uint64_t)ins_at) return 0;
        return (SDCS_CARD_INS | SDCS_STABLE | SDCS_CD_PIN); }
    case 0x2c: { /* CLK_CTRL(0x2c)/TIMEOUT(0x2e)/SW_RESET(0x2f) */
        uint32_t w = m->sdc_reg[0x2c >> 2];
        uint32_t clk = (w >> 16) & 0xffff;      /* halfword at 0x2c = bits[31:16] */
        if (clk & 0x1) clk |= 0x2;              /* INCLK_ENABLE -> INCLK_STABLE */
        /* SW_RESET (byte 0x2f = bits[7:0]) is self-clearing: report reset done */
        return (clk << 16) | (w & 0x0000ff00u); }
    case 0x30: return m->sdc_int_stat;
    case 0xfc: return 0x00000001u;              /* HOST_VER (0xfe) small nonzero */
    default: return m->sdc_reg[(off & 0xfc) >> 2];
    }
}

static void sdc_write(machine_t *m, uint32_t off, uint32_t val, int size)
{
    uint32_t widx = (off & 0xfc) >> 2, w = m->sdc_reg[widx];
    if (size == 4) w = val;
    else if (size == 2) { if (off & 2) w = (w & 0xffff0000u) | (val & 0xffff);
                          else         w = (w & 0x0000ffffu) | ((val & 0xffff) << 16); }
    else { int sh = (3 - (off & 3)) * 8; w = (w & ~(0xffu << sh)) | ((val & 0xffu) << sh); }
    m->sdc_reg[widx] = w;

    if ((off & 0xfc) == 0x30)                    /* INT_STAT: write-1-to-clear */
        { m->sdc_int_stat &= ~val; m->sdc_reg[widx] = 0; return; }
    /* CMD register is the halfword at 0x0e (low 16b of word 0x0c). A write that
     * touches 0x0e (halfword there, or a word write to 0x0c) issues the command. */
    if (((off == 0x0e) && size == 2) || ((off & 0xfc) == 0x0c && size == 4)) {
        uint32_t word = m->sdc_reg[0x0c >> 2];
        sdc_do_cmd(m, word & 0xffff /*CMD @0x0e*/, (word >> 16) & 0xffff /*TRAN_MODE @0x0c*/);
    }
}

static uint32_t bus_rd(machine_t *m, uint32_t addr, int size, int *fault)
{
    *fault = 0;

    /* CT952_OSDUITRACE: OSD_ChangeUI (0xafd8) reads the active-UI latch at 0xafe0
     * with the mode arg in i0; catch that read to log every ChangeUI(mode) call +
     * whether it's declined (activeUI != 0). Per-access, so it fires mid-chunk. */
    { static int on = -1; if (on < 0) on = getenv("CT952_OSDUITRACE") ? 1 : 0;
      if (on && addr == 0x40020ec8u && m->cpu.pc >= 0xafd8u && m->cpu.pc <= 0xaff8u) {
        uint8_t *p = machine_dram_ptr(m, 0x40020ec8u);
        uint32_t au = p ? ((uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]) : 0;
        fprintf(stderr, "[OSDUI] ChangeUI(mode=%u) activeUI=%08x %s caller=%08x icount=%llu\n",
                sparc_get_reg(&m->cpu, 24) & 0xff, au,        /* i0 = reg 24 = mode */
                au ? "DECLINED" : "enters", sparc_get_reg(&m->cpu, 31),
                (unsigned long long)m->cpu.icount);
      }
    }

    /* DRAM data-region read-frequency histogram (CT952_DRAMHIST=<icount>):
     * counts reads to the BSS/data window past a threshold and dumps the
     * hottest addresses at exit -- pinpoints hot status vars like JPEG_Status
     * that a busy-poll hammers (item 3, find the decode-completion variable). */
    {
        static uint32_t *hc = 0; static uint32_t *ha = 0; static int on = -1;
        static uint64_t thr = 0;
        if (on < 0) { const char *e = getenv("CT952_DRAMHIST");
            on = e ? 1 : 0; if (e) { thr = strtoull(e, NULL, 0);
                hc = calloc(4096, 4); ha = calloc(4096, 4); } }
        if (on && hc && addr >= 0x40028000u && addr < 0x40100000u &&
            m->cpu.icount > thr && m->cycles < m->jpu_active_until) {
            uint32_t k = ((addr >> 2) * 2654435761u) & 4095u, i;
            for (i = 0; i < 4096; i++) {
                uint32_t s = (k + i) & 4095u;
                if (!hc[s]) { ha[s] = addr; hc[s] = 1; break; }
                if (ha[s] == addr) { hc[s]++; break; }
            }
            static uint64_t last = 0;
            if (m->cpu.icount - last > 60000000ull) {
                last = m->cpu.icount; int j, t;
                fprintf(stderr, "[DRAMHIST @%lluM] top reads:\n",
                        (unsigned long long)(m->cpu.icount/1000000));
                for (t = 0; t < 8; t++) { int b=-1; uint32_t bc=0;
                    for (j=0;j<4096;j++) if (hc[j]>bc){bc=hc[j];b=j;}
                    if (b<0||!bc) break;
                    fprintf(stderr, "   %08x  %u\n", ha[b], bc); hc[b]=0; }
            }
        }
    }

    /* DSU2 block (0x98000000): PROC1 reads PROC2's live PC here to monitor
     * it (REG_PLAT_DSU2_PC = 0x98080010). Back the PC/nPC; rest reads 0. */
    if (addr >= 0x98000000u && addr < 0x98100000u) {
        /* REG_PLAT_DSU2_PC/NPC: PROC1's monitor reads PROC2's live PC and
         * resets PROC2 if it looks hung / out of the firmware DRAM range.
         * When the real core isn't running, report an advancing PC inside
         * PROC2's code range (DS_PROC2_STARTADDR 0x40002000 .. SP 0x4001cf00)
         * so the monitor sees the audio DSP as alive and stops resetting it. */
        if (addr == 0x98080010u)
            return m->proc2_on ? m->cpu2.pc
                   : 0x40002000u + (uint32_t)((m->cycles >> 4) & 0x3FFFu) * 4u;
        if (addr == 0x98080014u)
            return m->proc2_on ? m->cpu2.npc
                   : 0x40002000u + (uint32_t)((m->cycles >> 4) & 0x3FFFu) * 4u + 4u;
        return 0;
    }

    /* Capture the boot-thread poll loop stuck after the logo times out:
     * whenever OS_GetSysTimer reads the eCos tick backing store (0x4002e328),
     * dump the recent PC ring (the caller chain) once we're past the logo
     * (icount>60M). CT952_POLLTRACE. */
    if (getenv("CT952_POLLTRACE") && addr == 0x4002e328u &&
        m->cpu.icount > 60000000ull) {
        /* Dedupe by stack pointer so we see EACH thread's poll loop, not just
         * the hottest one repeated. Each distinct sp is a distinct thread. */
        static uint32_t seen[32]; static int nseen;
        uint32_t sp = sparc_get_reg(&m->cpu, 14);
        int k, dup = 0;
        for (k = 0; k < nseen; k++) if (seen[k] == sp) { dup = 1; break; }
        if (!dup && nseen < 32) {
            seen[nseen++] = sp;
            fprintf(stderr, "[THREAD sp=%08x i7=%08x] recent PCs:", sp,
                    sparc_get_reg(&m->cpu, 31));
            for (k = 52; k < 64; k++)
                fprintf(stderr, " %08x", m->cpu.pc_ring[(m->cpu.pc_ri + k) & 63]);
            fprintf(stderr, "\n");
        }
    }

    /* Capture the PC that polls the BIU bit-stream read channel (0x80002a28
     * / 0x80002a34), so we can disassemble the poll loop and learn the exact
     * "drained/ready" value the JPEG worker wants. CT952_BIUTRACE. */
    if (getenv("CT952_BIUTRACE") &&
        (addr == 0x80002a28u || addr == 0x80002a34u || addr == 0x80002a30u)) {
        static int bn; if (bn < 12) {
            fprintf(stderr, "[BIU rd] %08x pc=%08x\n", addr, m->cpu.pc); bn++; }
    }

    /* EXPERIMENT (CT952_JPEG_DMA): the JPEG worker thread (DRAM 0x4001fe90)
     * spins on `while ((*0x80002a28 & mask)==0) ...` waiting for the BIU
     * bit-stream read-channel DMA to signal ready/drained -- a DMA we don't
     * model, so it times out and JPEG_Status stays UNFINISH. Present the
     * channel-status registers as "ready" so the worker can finish and reach
     * HALJPEG_Display. */
    {
        static int jd = -1;
        if (jd < 0) jd = getenv("CT952_JPEG_DMA") ? 1 : 0;
        if (jd && (addr == 0x80002a28u || addr == 0x80002a30u ||
                   addr == 0x80002a34u))
            return 0xFFFFFFFFu;
    }

    /* PROC2 vdec stand-in: deliver the command ack after a dwell.
     * Skipped once the real PROC2 core is running -- it drives PLAYMODE.
     * The commanded (intermediate) state stays visible in bram[0x190] until
     * m->cycles reaches proc2_ack_cycle, then we overwrite it with the ack --
     * so a boot poll waiting for the intermediate state (e.g. MODE_STOP 0x10)
     * has a real window before it advances to the ack (STOPPED 0x11). */
    if (!m->proc2_on && addr == 0xB0000190u && m->proc2_ack_cycle &&
        m->cycles >= m->proc2_ack_cycle) {
        m->bram[0x190] = proc2_ack_of(m->proc2_cmd);
        m->proc2_ack_cycle = 0;
    }

    /* Focused decoder-playmode trace (CT952_PMTRACE): the boot thread polls
     * the vdec state (flash 0x6f054/0x375a0) waiting for MODE_STOP(0x10);
     * log the first reads of the hw playmode + its software mirrors. */
    if (g_pm_trace < 0) g_pm_trace = getenv("CT952_PMTRACE") ? 1 : 0;
    if (g_pm_trace && (addr == 0xB0000190u || addr == 0x40039cd0u ||
                       addr == 0x40039d34u) && g_pm_n < 80) {
        uint32_t v = (addr == 0xB0000190u) ? m->bram[0x190]
                     : mem_read_raw(m->dram + (addr - 0x40000000u), 1);
        fprintf(stderr, "[PM rd] %08x=%02x pc=%08x icount=%llu\n", addr,
                v & 0xff, m->cpu.pc, (unsigned long long)m->cpu.icount);
        g_pm_n++;
    }

    if (addr < MACH_FLASH_MAX) {
        if (addr + (uint32_t)size <= m->flash_size)
            return mem_read_raw(m->flash + addr, size);
        return 0xFFFFFFFFu;   /* erased flash */
    }
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE) {
        if (getenv("CT952_KEYTRACE") && (addr & ~3u) == 0x40039074u &&
            m->cpu.icount > 40001300ull && m->cpu.icount < 50000000ull) {
            static int krt; if (krt < 40) {
                fprintf(stderr, "[KEYrd] __bISRKey read pc=%08x icount=%llu\n",
                        m->cpu.pc, (unsigned long long)m->cpu.icount); krt++; }
        }
        /* Ungated __bISRKey read watch (CT952_KEYWATCH=<from>): who POLLS the key
         * var? If only the ISR reads it and no application loop does, the key
         * consumer (INPUT/AP_MainLoop) is not running -- which is why keys do
         * nothing in the slideshow. Distinct callers past <from>. */
        { static long kw = -2; static uint32_t seen[32]; static int nseen = 0;
          if (kw == -2) { const char *e = getenv("CT952_KEYWATCH");
              kw = e ? (long)strtoull(e, NULL, 0) : -1; }
          if (kw >= 0 && addr == 0x400235acu &&
              m->cpu.icount >= (uint64_t)kw &&
              m->cpu.icount < (uint64_t)kw + 200000ull) {
              /* windowed, non-deduped: every access to the IR key var right after
               * injection -- reader PCs and the clear (write 0xa0) name the
               * dispatcher/consumer. (nseen counter throttles total lines.) */
              (void)seen;
              if (nseen < 120) { nseen++;
                  fprintf(stderr, "[KEYWATCH] 400235ac read pc=%08x icount=%llu\n",
                          m->cpu.pc, (unsigned long long)m->cpu.icount); }
          }
        }
        /* DECODE-status poll locator (CT952_DSTRACE): log reads of the HAL/JEPG
         * status region around the logo-decode finish so we can find the var
         * HALJPEG_Status(DECODE) polls and force it OK (10.39 decoder work). */
        if (getenv("CT952_DSTRACE") && (addr & ~3u) >= 0x40040c00u &&
            (addr & ~3u) < 0x40041000u &&
            m->cpu.icount > 10600000ull && m->cpu.icount < 13000000ull) {
            static int ds; if (ds < 80) {
                fprintf(stderr, "[DSTrd] %08x pc=%08x icount=%llu\n",
                        addr & ~3u, m->cpu.pc, (unsigned long long)m->cpu.icount); ds++; }
        }
        if (m->skip_panelcfg && addr == 0x4002f770u)
            return 0xFFFFFFFFu;   /* desc+0x14 = -1: take the skip path */
        /* MODE-8 DECLINE test (CT952_MODE8DECLINE): §12.40 -- the mode-8 media/
         * source UI latch is gated by predicate 0x272a8's `ldub [0x40032b3b]; be`
         * = the CONFIGURED-source count (setting 0xa3), NOT present media (§12.15).
         * Present that count as 0 so the predicate declines -> 0x418f0's fallback
         * OSD_ChangeUI(POWERON_MENU/mode 7) runs -> mode-7 handler 0x61cf8 sets
         * __bPOWERONMENUInitial NATURALLY -> tests whether the OSDSS screensaver
         * then arms. This forces an UPSTREAM condition (source count) and lets the
         * real code flow set the flag -- it is NOT the confounded FORCEPOM (which
         * forced the flag's own read and tripped its setter's guard). */
        if (addr == 0x40032b3bu && size == 1) {
            static int m8 = -1;
            if (m8 < 0) m8 = getenv("CT952_MODE8DECLINE") ? 1 : 0;
            if (m8) return 0;
        }
        /* (CT952_CHOOSEMEDIA crutch removed §11.3: proven inert once decode is
         * faithful -- the raw boot reaches POWERONMENU without forcing the
         * __bChooseMedia byte.) */
        /* Decoder-STOP window: present the state mirror (0x40039cd0, read by
         * getter 0x6f054) as MODE_STOP(0x10) so the boot stop-poll latches it
         * before the state settles to STOPPED(0x11). See machine.h. */
        if (addr == 0x40039cd0u) {
            if (getenv("CT952_MIRTRACE")) {
                uint32_t rv = (m->cycles < m->vdec_stop_until) ? 0x10u :
                    m->vdec_frame_done ? 0x10u :
                    m->vdec_stopped ? 0x11u : mem_read_raw(m->dram + 0x39cd0u, size);
                static int mt; if (mt < 60) {
                    fprintf(stderr, "[MIRrd] ->%02x fdone=%d pc=%08x icount=%llu\n",
                            rv, m->vdec_frame_done, m->cpu.pc,
                            (unsigned long long)m->cpu.icount); mt++; }
            }
            /* Faithful software-mirror model: MODE_STOP(0x10) during the ack
             * dwell, then MODE_STOPPED(0x11) once stopped -- standing in for the
             * firmware/decoder-library mirror write that a real decoder-stop
             * produces (§10.25). Clears the boot's gate-1 (0x10) and gate-3
             * (0x11) decoder-stop polls naturally -- retired the sp-gated
             * CT952_TEST_MIRROR10 read-hack this replaced. */
            if (m->cycles < m->vdec_stop_until) return 0x10u;
            /* Once a JPEG frame has been functionally decoded, the decoder sits at
             * MODE_STOP(0x10)=frame-done: the decode-status getter (0x375a0 action 0)
             * maps 0x10 -> JPEG_STATUS_OK. This fires only after the boot's stop
             * gates (which run before any decode), so gate-3's 0x11 is unaffected.
             * Faithful decoder state: reports frame-done whenever a frame has been
             * decoded (the JPU-decode completion the firmware polls) -- §10.53. */
            if (m->vdec_frame_done) return 0x10u;
            if (m->vdec_stopped) return 0x11u;
        }
        /* EXPERIMENT (CT952_VDEC_IDLE): present the boot thread's decoder-init
         * handshake flags as "decoder idle/ready" so INITIAL_PowerONStatus's
         * chain of MODE_STOP-style waits (0x61170 -> next gate 0x40039f24==1 ...)
         * completes and the firmware proceeds to draw the menu. Measures how
         * deep the chain is (each cleared gate exposes the next). */
        {
            static int vi = -1;
            if (vi < 0) vi = getenv("CT952_VDEC_IDLE") ? 1 : 0;
            if (vi && addr == 0x40039f24u) return 1;   /* gate 2: ==1 */
        }
        /* INITIAL_PowerONStatus progression probe (CT952_PONSREADY): the
         * power-on state machine gates "Loading"->next on a countdown byte at
         * 0x40022F5E; poster fn 0x45660 posts CC event 0x1000 only while it
         * reads exactly 2. With TICK_MULT time-compression the byte can skip
         * the ==2 window (it sits at 0 in the stuck dump). Present it as 2 once
         * Loading is up so the poster fires and the state machine can advance.
         * Ground-truth addr from the live CC-thread stack decode (10.21/10.22). */
        {
            static int pr = -1;
            if (pr < 0) pr = getenv("CT952_PONSREADY") ? 1 : 0;
            if (pr && addr == 0x40022F5Eu && size == 1 && m->cpu.icount > 30000000ull)
                return 2;
        }
        /* (CT952_FORCEPOM probe removed §12.38: CONFOUNDED/NEGATIVE. Forcing the
         * __bPOWERONMENUInitial read (0x40023a10) to 1 past 40M trips POWERONMENU_
         * Initial's OWN guard (poweronmenu.c:384 `if(__bPOWERONMENUInitial) return`),
         * so the menu never initializes; the watchdog-pet thread stalls and the run
         * dies at ~63M with watchdog_fired. The screensaver never armed
         * (_bOSDSSScreenSaverMode 0x400239c4 stayed 0). A read-forcing probe can
         * never validate the gate -- it breaks the very routine that sets it. The
         * faithful path is making POWERONMENU_Initial complete on its own. */
        /* (CT952_THREADSDONE probe removed §12.32: NEGATIVE -- OR-ing the missing
         * MPEG(0x1)+InfoFilter(0x200) thread-done bits into __fThreadInit
         * 0x40038f80 does not advance the boot; the 0x418f0 thread-sync wait is
         * timed and already proceeds, so those bits are not the page-8 park.) */
        /* (CT952_FORCEADV probe removed §12.29: proven inert -- forcing the
         * page-8 guard VarB=1 does not advance, because by the time a photo has
         * decoded the page-8 enter handler has already latched/returned; the
         * real park is one layer deeper, the decode-status poll 0x36ff0 spinning
         * on the NULL decode handle 0x40022f88.) */
        /* (CT952_NOMEDIA crutch removed §11.3: proven inert past POWERONMENU --
         * it faked the USBSRC worker's CHECK_DEVICE->NO_MEDIA result without
         * waking the real (asleep) USB source thread, so it changed nothing.
         * The faithful fix is to model the USB/card host controller, not fake
         * the flags -- deferred to the event-starvation work.) */
        /* Read-watch on the card JPEG load buffer (CT952_RDWATCH): log the PC that
         * reads the info.a-loaded 01.JPG data at 0x401ec000 -- pinpoints the
         * parse-decision code that inspects the JPEG header (§12.96/97). */
        { static long rw = -2; if (rw == -2) rw = getenv("CT952_RDWATCH") ? 1 : 0;
          if (rw > 0 && addr >= 0x401ec000u && addr < 0x401ec040u) {
              static int rwn; if (rwn < 200) {
                  fprintf(stderr, "[RDWATCH] rd %08x (sz%d) pc=%08x icount=%llu\n",
                          addr, size, m->cpu.pc, (unsigned long long)m->cpu.icount);
                  rwn++; } } }
        return mem_read_raw(m->dram + (addr - 0x40000000u), size);
    }
    if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE) {
        { static long rw = -2; if (rw == -2) rw = getenv("CT952_RDWATCH") ? 1 : 0;
          if (rw > 0 && addr >= 0xC01ec000u && addr < 0xC01ec040u) {
              static int rwn; if (rwn < 200) {
                  fprintf(stderr, "[RDWATCH] rd %08x (sz%d, C0-alias) pc=%08x icount=%llu\n",
                          addr, size, m->cpu.pc, (unsigned long long)m->cpu.icount);
                  rwn++; } } }
        return mem_read_raw(m->dram + (addr - 0xC0000000u), size);
    }
    if (addr >= 0x80000000u && addr < 0x80000000u + MACH_IO_SIZE) {
        uint32_t off = (addr - 0x80000000u) & ~3u;
        uint32_t v = io_read(m, off);
        /* info.a JPEG-parse engine completion (§12.98): 0x80000a30 bits[16:21]
         * is the progress/done field the parse polls (>0x1f). Report full
         * progress (0x3f) once the engine has been kicked (GO at 0x80000a3c). */
        if (off == 0x0a30u && m->eng_done)
            v = (v & ~(0x3fu << 16)) | (0x3fu << 16);
        if (getenv("CT952_ENGTRACE") && ((off >= 0x0a00u && off < 0x0a80u)
                                         || (addr >= 0x80010000u && addr < 0x80011000u))) {
            static int er; if (er < 120) {
                fprintf(stderr, "[ENGrd] %08x=%08x pc=%08x icount=%llu\n",
                        addr, v, m->cpu.pc, (unsigned long long)m->cpu.icount); er++; } }
        /* IR-injection trace (CT952_IRTRACE): log the firmware reading the IR
         * data/status regs and the PROC1-2nd pending -- proves the injected IR
         * interrupt drove the real ISR/DSR (INPUT_RemoteScan) path (10.38). */
        if (getenv("CT952_IRTRACE") &&
            (off == R_IR_DATA || off == R_IR_RAWCODE || off == R_P1_2ND_PEND ||
             off == R_P1_2ND_STCL)) {
            static int it; if (it < 60) {
                fprintf(stderr, "[IRrd] %08x=%08x pc=%08x icount=%llu\n",
                        addr, v, m->cpu.pc, (unsigned long long)m->cpu.icount); it++; }
        }
        /* Decode-window IO trace (CT952_DECTRACE): log reads of the decode
         * DMA/status regs during the logo-decode burst so we can see what the
         * driver polls and where it gives up (roadmap P1). */
        if (getenv("CT952_DECTRACE") && m->cpu.icount > 10000000ull &&
            m->cpu.icount < 11600000ull &&
            (off == 0xb4u || off == 0xb8u || off == 0xc10u || off == 0xe00u || off == 0x2a28u || off == 0x2a34u ||
             off == 0x2a30u || off == 0x2884u || off == 0xc10u)) {
            static int dt; if (dt < 80) {
                fprintf(stderr, "[DECrd] %08x=%08x pc=%08x icount=%llu\n",
                        addr, v, m->cpu.pc, (unsigned long long)m->cpu.icount); dt++; }
        }
        /* IIC/EEPROM access trace (CT952_IICTRACE): §12.39 -- prove empirically
         * what the firmware reads from the board EEPROM master (0x80004204/0c/10/14)
         * and whether the current io_get (last-written) model gives it a value it
         * accepts or one that drives a retry/re-read cycle. Logs both rd here and
         * wr below (guarded by the same env). */
        if (getenv("CT952_IICTRACE") &&
            (off == 0x4204u || off == 0x4210u || off == 0x4214u)) {
            static int ic; if (ic < 400) {
                fprintf(stderr, "[IICrd] %08x=%08x pc=%08x icount=%llu\n",
                        addr, v, m->cpu.pc, (unsigned long long)m->cpu.icount); ic++; }
        }
        if (size == 4) return v;
        /* sub-word I/O read: extract big-endian lane */
        if (size == 1) return (v >> ((3 - (addr & 3)) * 8)) & 0xFF;
        return (v >> ((addr & 2) ? 0 : 16)) & 0xFFFF;
    }
    if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u) {
        /* DSP-interface read trace (CT952_DSPTRACE=<from_icount>): log every SRAM
         * (bram) read past the given icount -- reveals exactly which decoder-DSP
         * state the firmware polls at the stall, to build the DSP model (§12.67). */
        { static long dt = -2; static uint64_t dtn[64]; static int dti = 0;
          if (dt == -2) { const char *e = getenv("CT952_DSPTRACE");
                          dt = e ? (long)strtoull(e, NULL, 0) : -1; }
          if (dt >= 0 && m->cpu.icount >= (uint64_t)dt) {
              uint32_t o = addr - 0xB0000000u; int seen = 0, k;
              for (k = 0; k < dti; k++) if (dtn[k] == o) { seen = 1; break; }
              if (!seen && dti < 64) { dtn[dti++] = o;
                  fprintf(stderr, "[DSP rd] b0000%03x sz%d pc=%08x icount=%llu\n",
                          o, size, m->cpu.pc, (unsigned long long)m->cpu.icount); }
          } }
        /* EXPERIMENT (CT952_FORCE_PLAYMODE): present the vdec playmode
         * (0xb0000190) as a fixed value, standing in for the PROC2 decoder
         * microcode reaching MODE_STOP(0x10). Tests whether the boot thread's
         * decoder-state poll (flash 0x375a0/0x6f054) is the menu-draw gate. */
        if (addr == 0xB0000190u) {
            /* During the STOP window, present the live reg as STOPPED(0x11) so
             * the getter (0x6f054) adds no busy bit and returns the mirror's
             * MODE_STOP(0x10) cleanly. */
            if (m->cycles < m->vdec_stop_until) return 0x11u;
            /* After a decode frame completes, the live playmode is idle. The
             * getter (0x6f098) only returns the mirror CLEANLY (no 0x1000 busy
             * bit) when HW in {0,0x11}; present 0x11 so the mirror's 0x10 maps to
             * JPEG_STATUS_OK (needs both, 10.41). Faithful: idle after frame-done. */
            if (m->vdec_frame_done) return 0x11u;
            static int fp = -1;
            if (fp < 0) { const char *e = getenv("CT952_FORCE_PLAYMODE");
                          fp = e ? (int)strtoul(e, NULL, 0) : -2; }
            if (fp >= 0) { m->bram[0x190] = (uint8_t)fp; }
            /* Released-vdec idle playmode (CT952_VDEC_IDLE): a running PROC2
             * decoder reports MODE_RELEASE_MODE(0x86) as its idle state.
             * INITIAL_System's decoder-sync poll (flash 0x6f820) must read 0x86
             * to then command MODE_STOP(0x10) and proceed; otherwise it spins
             * forever (§12.56). PROC2 is held in reset and cannot post 0x86
             * itself, so present it while the register is uninitialized
             * (MODE_NONE). Once the firmware writes a real command the raw value
             * is non-zero and we respect it (so the immediate re-read of the
             * commanded 0x10 succeeds). */
            {
                static int vr = -1;
                if (vr < 0) vr = getenv("CT952_VDEC_IDLE") ? 1 : 0;
                if (vr && m->bram[0x190] == 0x00u) return 0x86u;
            }
        }
        return mem_read_raw(m->bram + (addr - 0xB0000000u), size);
    }
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return 0;                        /* DSU stub */
    if (addr >= EHCI_BASE && addr < EHCI_END) {
        uint32_t off = (addr - EHCI_BASE) & ~3u;
        uint32_t v = ehci_read(m, off);
        if (getenv("CT952_EHCITRACE"))
            fprintf(stderr, "[ehci] rd 0x%08x off 0x%02x -> 0x%08x (pc=0x%08x)\n",
                    addr, off, v, m->cpu.pc);
        if (size == 4) return v;
        if (size == 1) return (v >> ((3 - (addr & 3)) * 8)) & 0xFF;
        return (v >> ((addr & 2) ? 0 : 16)) & 0xFFFF;
    }
    /* SD host controller (0xa0001100..0xa00011ff): active only when a card image
     * is loaded; otherwise the region stubs to 0 (SDC_STAT card-inserted bit clear
     * => no card), preserving the card-less behaviour. */
    if (m->sd_img && addr >= SDC_BASE_ADDR && addr < SDC_BASE_ADDR + 0x100u) {
        uint32_t off = addr - SDC_BASE_ADDR;
        uint32_t v = sdc_read(m, off & 0xfc);
        if (getenv("CT952_SDCTRACE"))
            fprintf(stderr, "[SDC] rd %08x -> %08x (sz%d) pc=%08x icount=%llu\n",
                    addr, v, size, m->cpu.pc, (unsigned long long)m->cpu.icount);
        if (size == 4) return v;
        if (size == 1) return (v >> ((3 - (addr & 3)) * 8)) & 0xFF;
        return (v >> ((addr & 2) ? 0 : 16)) & 0xFFFF;
    }
    if (addr >= 0xA0000000u && addr < 0xA0010000u) {
        log_access(m, addr & ~3u, 0, 0);
        return 0;                        /* FCR/SDC/NFC stub */
    }
    m->unmapped_reads++;
    log_access(m, addr & ~3u, 0, 0);   /* record so it names the blocker */
    return 0;
}

/* boot PROC2: seed the second core at the entry/SP PROC1 staged in the AIU GR
 * bank (GR22 = 0x800007d8 start, GR21 = 0x800007d4 SP) and let it run. */
static void proc2_boot(machine_t *m)
{
    uint32_t entry = io_get(m, R_PROC2_START);
    uint32_t sp    = io_get(m, R_PROC2_SP);
    if (m->proc2_on || !m->proc2_enable || (entry & 0xF0000000u) != 0x40000000u)
        return;
    sparc_reset(&m->cpu2, &m->bus2);
    m->cpu2.pc = entry;
    m->cpu2.npc = entry + 4;
    sparc_set_reg(&m->cpu2, 14, sp);   /* %o6 / %sp */
    m->proc2_on = 1;
}

static void proc2_halt(machine_t *m)
{
    m->proc2_on = 0;
}

static void bus_wr(machine_t *m, uint32_t addr, uint32_t val,
                   int size, int *fault)
{
    *fault = 0;

    /* Buffer-handoff watch (CT952_BUFWATCH): log ANY write (any region) whose
     * VALUE is the staged card JPEG buffer 0x401ec000 -- catches info.a handing
     * the file to a hardware engine (thumbnail/validation decoder) or another
     * pointer slot. The SDC DMA_ADDR write at pc 0xcb0d8 is the known one; any
     * OTHER site taking 0x401ec000 is the abandoned-buffer's real consumer. */
    if (getenv("CT952_BUFWATCH") && (val == 0x401ec000u || val == 0xC01ec000u)
        && m->cpu.pc != 0x000cb0d8u) {
        fprintf(stderr, "[BUFWATCH] wr %08x <- %08x pc=%08x icount=%llu\n",
                addr, val, m->cpu.pc, (unsigned long long)m->cpu.icount);
    }
    /* Display-SM state watch (CT952_SMWATCH): writes to the 0x70240 gate vars --
     * NVRAM 0xb0000190 and OSD state 0x40039f1c/f24/f60 (§12.102). Shows whether
     * the state machine's inputs ever change or are frozen. */
    if (getenv("CT952_SMWATCH") &&
        (addr == 0xb0000190u || addr == 0x40039f1cu || (addr & ~1u) == 0x40039f24u
         || addr == 0x40039f60u || (addr & ~3u) == 0x40039cd0u || addr == 0x40022f06u
         || addr == 0x4003263cu || addr == 0x4003277cu
         || addr == 0x40039949u || addr == 0x4003274au
         || addr == 0x40022f81u || addr == 0x40032780u || addr == 0x4002fb2au || addr == 0x40022f5eu || addr == 0x40022f00u || (addr &~3u) == 0x4002fb50u || addr == 0x4003996cu || (addr &~3u) == 0x40032aa0u)) {
        static int smw; if (smw < 300) {
            fprintf(stderr, "[SMW] %08x <- %08x (sz%d) pc=%08x icount=%llu\n",
                    addr, val, size, m->cpu.pc, (unsigned long long)m->cpu.icount); smw++; }
    }
    /* Engine-register trace (CT952_ENGTRACE): all writes to the 0x80000800 block
     * (base+0x200..0x240 = the JPEG/DMA engine info.a kicks for the card parse). */
    if (getenv("CT952_ENGTRACE") && addr >= 0x80000800u && addr < 0x80000a80u) {
        fprintf(stderr, "[ENG] wr %08x <- %08x (sz%d) pc=%08x icount=%llu\n",
                addr, val, size, m->cpu.pc, (unsigned long long)m->cpu.icount);
    }
    /* info.a JPEG-parse engine (§12.98): the card parse programs a source
     * (0x80000a20 <- 0x401ec000) then polls a completion field at 0x80000a30
     * bits[16:21] until it exceeds 0x1f. Without a model the field stays 0 and
     * the parse spins forever (0x9bcb4). Capture the source and the GO
     * (0x80000a3c <- 1) so the status read reports "done". */
    if (getenv("CT952_JPUENG")) {
        /* per-op: a new source write re-arms the engine (clears done); GO
         * completes the op. Avoids a permanent latch that could bleed into the
         * display path's own use of the shared engine. */
        if (addr == 0x80000a20u) { m->eng_src = val; m->eng_done = 0; }
        if (addr == 0x80000a3cu && (val & 1u)) {
            m->eng_done = 1;
            if (getenv("CT952_ENGTRACE")) {
                uint32_t bt[32]; int nb = sparc_win_backtrace(&m->cpu, bt, 32), bi;
                fprintf(stderr, "[ENGGO] src=%08x pc=%08x winframes:", m->eng_src, m->cpu.pc);
                for (bi = 0; bi < nb; bi++) fprintf(stderr, " %08x", bt[bi]);
                fprintf(stderr, " icount=%llu\n", (unsigned long long)m->cpu.icount);
            }
        }
    }

    /* CT952_UITRACE: watch the OSD active-UI-record pointer (0x40020ec8) and the
     * __bPOWERONMENUInitial gate (0x40023a10). Logs every UI-mode transition and
     * every set/clear of the gate with the writing PC -- so the boot's UI path is
     * visible (does it ever attempt POWERON_MENU / run the 0x61cf8 setter?).
     * DP700WD_HW_REFERENCE.md 12.13. */
    { static int uit = -1; if (uit < 0) uit = getenv("CT952_UITRACE") ? 1 : 0;
      if (uit) {
        if (addr == 0x40020ec8u) {
            uint32_t rec = val, id = 0;
            if (rec >= 0x40000000u && rec < 0x40000000u + MACH_DRAM_SIZE)
                id = *(uint32_t *)(m->dram + (rec - 0x40000000u)); /* first word = mode id (BE host read below) */
            fprintf(stderr, "[UITRACE] activeUI -> rec=%08x (id?=%08x) pc=%08x i7=%08x o7=%08x icount=%llu\n",
                    val, __builtin_bswap32(id), m->cpu.pc,
                    sparc_get_reg(&m->cpu, 31), sparc_get_reg(&m->cpu, 15),
                    (unsigned long long)m->cpu.icount);
        }
        if (addr == 0x40023a10u)
            fprintf(stderr, "[UITRACE] __bPOWERONMENUInitial <- %02x pc=%08x icount=%llu\n",
                    val & 0xff, m->cpu.pc, (unsigned long long)m->cpu.icount);
        /* mode-8 (media UI) stay-flag + its media-event trigger (§12.14/12.15) */
        if (addr == 0x40032b3bu)
            fprintf(stderr, "[UITRACE] mode8_stayflag(32b3b) <- %02x pc=%08x icount=%llu\n",
                    val & 0xff, m->cpu.pc, (unsigned long long)m->cpu.icount);
        if (addr == 0x40032b0du)
            fprintf(stderr, "[UITRACE] media_evt(32b0d) <- %02x pc=%08x icount=%llu\n",
                    val & 0xff, m->cpu.pc, (unsigned long long)m->cpu.icount);
        /* page-8 advance-event guard VarB (§12.28): set to 1 only when a
         * completion/UI event is delivered; if this never fires the stage
         * parks showing the first photo and never reaches POWERONMENU. */
        if (addr == 0x40022f81u)
            fprintf(stderr, "[UITRACE] VarB(22f81) <- %02x pc=%08x i7=%08x icount=%llu\n",
                    val & 0xff, m->cpu.pc, sparc_get_reg(&m->cpu, 31),
                    (unsigned long long)m->cpu.icount);
        /* OSDSS idle-timer activity counter (§12.21 gate C): whatever writes this
         * during the idle is the phantom-activity source that resets the
         * screensaver's idle window. Log the writing PC to name it. */
        if (addr == 0x40031abcu) {
            static int n; if (n < 60) { n++;
            fprintf(stderr, "[UITRACE] activity(31abc) <- %08x pc=%08x i7=%08x icount=%llu\n",
                    val, m->cpu.pc, sparc_get_reg(&m->cpu, 31),
                    (unsigned long long)m->cpu.icount); }
        }
      }
    }

    /* CT952_FREQTRACE (§12.43): full-speed watch on the CC-event flag PAIR
     * F_REQ 0x40026e9c / F_DONE 0x40026ea4. OS_SetFlag/ClearFlag write the whole
     * word; log the new value + which bit(s) and the PC. The question: is bit 0x80
     * (message-delivery -> 0x6eec -> PostEvent -> page-8/OSDSS) EVER set in F_REQ,
     * and by whom? bit 0x1000 = per-frame redraw pulse (expected frequent). */
    { static int fq = -1; if (fq < 0) fq = getenv("CT952_FREQTRACE") ? 1 : 0;
      if (fq && (addr == 0x40026e9cu || addr == 0x40026ea4u)) {
          static uint32_t prev_req = 0, prev_done = 0;
          uint32_t *pp = (addr == 0x40026e9cu) ? &prev_req : &prev_done;
          uint32_t newly = val & ~*pp;
          const char *nm = (addr == 0x40026e9cu) ? "F_REQ " : "F_DONE";
          /* always announce a bit-0x80 set; otherwise cap the (frequent) 0x1000 spam */
          static int cap = 0;
          if ((newly & 0x80u) || cap < 120) {
              if (!(newly & 0x80u)) cap++;
              fprintf(stderr, "[FREQ] %s <- %08x newly=%08x%s pc=%08x i7=%08x icount=%llu\n",
                      nm, val, newly, (newly & 0x80u) ? "  <==BIT80!" : "",
                      m->cpu.pc, sparc_get_reg(&m->cpu, 31),
                      (unsigned long long)m->cpu.icount);
          }
          *pp = val;
      }
    }

    /* CT952_MONTRACE (§12.33): dynamic watch on the registered-monitor list head
     * (0x40032180/84) and the monitor callback table (0x40024980..0x40024b00).
     * page-8 advances only when 0x11f48 finds a monitor with a pending count in
     * this list (§10.24), and the list is empty at the park. Log every write here
     * -- catches the computed-pointer registrations static disasm can't annotate,
     * so we can see WHO registers a monitor (the event producer) or confirm none. */
    {
        static int mt = -1;
        if (mt < 0) mt = getenv("CT952_MONTRACE") ? 1 : 0;
        if (mt && addr >= 0x40032180u && addr <= 0x4003218cu &&
            m->cpu.icount > 5000000ull) {
            static int n; if (n < 120) { n++;
            fprintf(stderr, "[MON] %08x <- %08x pc=%08x i7=%08x o7=%08x icount=%llu\n",
                    addr, val, m->cpu.pc, sparc_get_reg(&m->cpu, 31),
                    sparc_get_reg(&m->cpu, 15), (unsigned long long)m->cpu.icount); }
        }
        /* CT952_UISTACK (§12.36): watch the REAL OSD_ChangeUI (0x4a754) UI stack
         * (0x400391d8) + index (0x40039509) -- detects whether POWERONMENU_Initial
         * (poweronmenu.c:409 OSD_ChangeUI(POWERON_MENU)) ever runs, i.e. whether
         * the block is in page-8 or downstream in POWERONMENU_Initial's display. */
        if (getenv("CT952_UISTACK") &&
            ((addr >= 0x400391d8u && addr <= 0x400391f8u) || addr == 0x40039509u) &&
            m->cpu.icount > 5000000ull) {
            static int n; if (n < 80) { n++;
            fprintf(stderr, "[UISTK] %08x <- %08x pc=%08x o7=%08x icount=%llu\n",
                    addr, val, m->cpu.pc, sparc_get_reg(&m->cpu, 15),
                    (unsigned long long)m->cpu.icount); }
        }
    }

    /* Count PROC2-reset asserts regardless of the PROC2 feature gate: the
     * config-callback-walk loop hammers 0x80000324 whether or not we model the
     * second core, so this measures the loop directly (diagnostic). */
    if (addr == 0x80000324u && (val & 0x1u)) g_proc2_reset_writes++;

    /* Log every PROC2 reset-control / release write + the staged entry, to see
     * whether the firmware ever tries to RELEASE PROC2 (0x80000304 bit0) and
     * with what entry (GR22 @0x800007d8). Gated on CT952_P2TRACE. */
    if (getenv("CT952_P2TRACE")) {
        /* Log only PROC2-CORE reset/release (0x304/0x324 bit0) + DSU2 ctrl --
         * the events that actually start/stop the second core. */
        int hit = ((addr == 0x80000304u || addr == 0x80000324u) && (val & 1u))
                  || addr == 0x98000000u;
        if (hit) fprintf(stderr, "[P2core] %08x=%08x pc=%08x start=%08x icount=%llu\n",
                         addr, val, m->cpu.pc, io_get(m, R_PROC2_START),
                         (unsigned long long)m->cpu.icount);
    }
    /* CT952_P2FULL: complete PROC2 lifecycle trace (any value) -- the reset
     * bit0 (PLAT_RESET_PROC2, ctkav_platform.h:302/340) vs. the VPU/JPU reset
     * bit23 (0x00800000) share 0x304/0x324, so a bit-0-only filter hides the
     * whole picture. Also logs the AIU-GR staging bank the JPEG-on-PROC2 boot
     * uses: START GR22(0x7d8), SP GR21(0x7d4), ACK GR25(0x7e4). */
    if (getenv("CT952_P2FULL") &&
        (addr == 0x80000304u || addr == 0x80000324u || addr == 0x98000000u ||
         addr == 0x800007d8u || addr == 0x800007d4u || addr == 0x800007e4u)) {
        static int n; if (n < 200) { n++;
        fprintf(stderr, "[P2full] %08x=%08x pc=%08x i7=%08x icount=%llu\n",
                addr, val, m->cpu.pc, sparc_get_reg(&m->cpu, 31),
                (unsigned long long)m->cpu.icount);
        /* On a PROC2-core reset write (bit0), dump the staged entry region so we
         * can see whether HAL_LoadAudioCode actually decompressed the "JPEG"
         * section to DS_PROC2_STARTADDR (0x40002000) before the release. */
        if ((addr == 0x80000324u || addr == 0x80000304u) && (val & 1u)) {
            uint8_t *p = machine_dram_ptr(m, 0x40002000u);
            if (p) fprintf(stderr, "       DRAM@40002000: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11]);
        } }
    }

    /* PROC2 reset/debug control (writes from PROC1) */
    if (m->proc2_enable) {
        int was = m->proc2_on;
        if (addr == 0x80000304u && (val & 0x1u)) { proc2_boot(m); }  /* RESET_DISABLE: release */
        else if (addr == 0x80000324u && (val & 0x1u)) { proc2_halt(m); } /* RESET_ENABLE: hold */
        else if (addr == 0x98000000u) {              /* DSU2 control */
            if (val & 0x00080000u) proc2_boot(m);    /* PLAT_DSU_CTL_RE */
            else if (val & 0x000000A0u) proc2_halt(m); /* BN|BW: break */
        }
        if (was != m->proc2_on && getenv("CT952_TRACE"))
            fprintf(stderr, "[P2] %s via %08x=%08x entry=%08x pc1=%08x\n",
                    m->proc2_on ? "BOOT" : "HALT", addr, val,
                    io_get(m, R_PROC2_START), m->cpu.pc);
    }
    if (addr >= 0x98000000u && addr < 0x98100000u)
        return;   /* DSU2 register file: writes accepted, not modeled */

    /* PROC1 issued a VDEC command via REG_SRAM_PLAYMODE: arm the PROC2 ack
     * (delivered after a few status polls, mimicking the microcode latency). */
    if (addr == 0xB0000190u) {
        uint8_t cmd = (uint8_t)val;
        m->proc2_cmd = cmd;
        /* Arm the dwell when the ack differs from the commanded value
         * (e.g. STOP 0x10 -> STOPPED 0x11); hold the commanded state for
         * proc2_ack_dwell cycles so the boot poll can latch it first. */
        m->proc2_ack_cycle = (proc2_ack_of(cmd) != cmd)
                             ? m->cycles + m->proc2_ack_dwell : 0;
        /* A STOP command from the COMDEC issuer (flash 0x6f2b0..0x6f400) --
         * whether a fresh stop (writes MODE_STOP 0x10) or an idempotent
         * repeat-stop (writes STOPPED 0x11 directly) -- opens the MODE_STOP
         * visibility window so the boot poll (which reads the mirror via
         * 0x6f054) latches 0x10 before the state settles to 0x11. */
        if ((cmd == 0x10u || cmd == 0x11u) &&
            m->cpu.pc >= 0x6f2b0u && m->cpu.pc < 0x6f400u) {
            m->vdec_stop_until = m->cycles + m->proc2_ack_dwell;
            m->vdec_stopped = 1;   /* mirror -> STOPPED(0x11) after the window */
        } else if (cmd != 0x10u && cmd != 0x11u && cmd != 0x00u) {
            m->vdec_stopped = 0;   /* a real play/scan command: no longer stopped */
        }
        if (getenv("CT952_STOPTRACE") && m->cpu.icount > 55000000ull) {
            static int sn; if (sn < 24) {
                fprintf(stderr, "[PMwr>55M] b0000190=%02x pc=%08x armed=%d icount=%llu\n",
                        cmd, m->cpu.pc,
                        (m->cycles < m->vdec_stop_until), (unsigned long long)m->cpu.icount);
                sn++; }
        }
    }

    /* Trace who writes the vdec playmode + its software mirrors (the state the
     * boot poll 0x375a0 reads). CT952_PMTRACE. */
    if (g_pm_trace == 1 && (addr == 0xB0000190u || addr == 0x40039cd0u ||
                            addr == 0x40039d34u || addr == 0x40039f24u) &&
        g_pm_wn < 60) {
        fprintf(stderr, "[PM wr] %08x=%02x pc=%08x icount=%llu\n", addr,
                val & 0xff, m->cpu.pc, (unsigned long long)m->cpu.icount);
        g_pm_wn++;
    }

    if (addr < MACH_FLASH_MAX)
        return;                          /* XIP flash: ignore writes */
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE) {
        mem_write_raw(m->dram + (addr - 0x40000000u), val, size);
        /* Media-state scan (CT952_MSCAN=<start_icount>): log byte writes of the
         * MediaInfo bitflag values (INSERT=1/RECOGNIZE=2/PARSING=4/READY=8/
         * WRONG=0x10) in the game-variable region past <start_icount>, to watch
         * the media state machine advance on card insert. §12.96: with the card
         * present from boot, NONE of these fire after the 01.JPG load (32.4M) --
         * the info.a parse never raises MediaInfo->READY. */
        { static long ms_at = -2;
          if (ms_at == -2) { const char *e = getenv("CT952_MSCAN");
                             ms_at = e ? (long)strtoull(e, NULL, 0) : -1; }
          if (ms_at >= 0 && size == 1 && m->cpu.icount > (uint64_t)ms_at
              && ((val&0xff)>=1 && (val&0xff)<=4)
              && addr >= 0x40020000u && addr < 0x40040000u) {
              static int msn; if (msn < 4000) {
                  fprintf(stderr, "[MSCAN] %08x=%u pc=%08x icount=%llu\n",
                          addr, val & 0xff, m->cpu.pc, (unsigned long long)m->cpu.icount);
                  msn++; }
          } }
        /* Screensaver-gate write-watch (CT952_WWATCH): trace the exact writes
         * that decide the OSDSS slideshow -- __bPOWERONMENUInitial (0x40023a10,
         * the gate), __dwOSDSSCheckTime (0x400239b8, reset by OSDSS_ResetTime
         * each time a "key/activity" is seen), and __bISRKey (0x40039074, the
         * decoded panel/IR key). If the gate never turns 1, POWERONMENU_Initial
         * never completes; if CheckTime keeps getting rewritten, the idle timer
         * is being reset (something reads as "activity"). */
        if (getenv("CT952_WWATCH")) {
            uint32_t a = addr & ~3u;
            /* also watch the display-state-machine var 0x40039949 (byte) + the
             * readiness flag 0x4003996c (§12.59/12.67): the 7->0xd progression is
             * the DSP-model target -- log every step + the pc that wrote it. */
            if (addr == 0x40039949u || (addr & ~3u) == 0x4003996cu) {
                static int dw; if (dw < 300) {
                    fprintf(stderr, "[WW] %-14s %08x=%02x pc=%08x icount=%llu\n",
                            addr == 0x40039949u ? "DISPSTATE" : "READYFLAG",
                            addr, val & 0xff, m->cpu.pc,
                            (unsigned long long)m->cpu.icount); dw++; }
            }
            if (a == 0x40023a10u || a == 0x400239b8u || a == 0x40039074u) {
                static int ww;
                if (ww < 200) {
                    const char *nm = a == 0x40023a10u ? "POMInit"
                                   : a == 0x400239b8u ? "OSDSSCheckTime"
                                                      : "ISRKey";
                    fprintf(stderr, "[WW] %-14s %08x=%08x pc=%08x icount=%llu\n",
                            nm, addr, val, m->cpu.pc,
                            (unsigned long long)m->cpu.icount);
                    ww++;
                }
            }
        }
        /* OSDSS screensaver-state watch (CT952_OSDSSWATCH): log writes to the
         * candidate _bOSDSSScreenSaverMode / __bOSDSSPicIdx / __bPOWERONMENUInitial
         * bytes over the whole run -- confirms whether the OSDSS JPEG screensaver
         * mode turns on and the picture index advances (the cycling slideshow). */
        if (getenv("CT952_OSDSSWATCH") && size == 1) {
            const char *nm = NULL;
            if (addr == 0x400239c4u) nm = "_bOSDSSScreenSaverMode";
            else if (addr == 0x400239ccu) nm = "__bOSDSSPicIdx";
            else if (addr == 0x40023a10u) nm = "__bPOWERONMENUInitial";
            if (nm) {
                static int ow; if (ow < 200) {
                    fprintf(stderr, "[OSDSS] %-22s =%02x pc=%08x icount=%llu\n",
                            nm, val & 0xff, m->cpu.pc,
                            (unsigned long long)m->cpu.icount); ow++; }
            }
        }
        /* Video-plane-enable trace (CT952_VENTRACE): log writes to the software
         * video-enable flag *0x40023fc0 (compositor 0xa683c copies it to VIDEO_EN)
         * and the gate *0x40040e70, to find where the JPEG display enables video. */
        if (getenv("CT952_VENTRACE") &&
            ((addr & ~3u) == 0x40023fc0u || (addr & ~3u) == 0x40040e70u)) {
            static int ve; if (ve < 80) {
                fprintf(stderr, "[VENwr] %08x=%08x pc=%08x icount=%llu\n",
                        addr & ~3u, val, m->cpu.pc, (unsigned long long)m->cpu.icount); ve++; }
        }
        /* IR-key propagation trace (CT952_KEYTRACE): after the IR injection, log
         * small DRAM byte writes so we can locate __bISRKey and see whether the
         * key reaches the "Loading" event queue / input struct (10.38). */
        if (getenv("CT952_KEYTRACE") && size == 1 && val != 0 && val != 0xFFu &&
            ((addr >= 0x40032200u && addr < 0x40032a40u) || addr == 0x40039074u) &&
            m->cpu.icount > 40001300ull && m->cpu.icount < 50000000ull) {
            static int kt; if (kt < 80) {
                fprintf(stderr, "[KEYwr] %08x=%02x pc=%08x icount=%llu\n",
                        addr, (unsigned)val, m->cpu.pc, (unsigned long long)m->cpu.icount); kt++; }
        }
        /* JPEG_Status write locator (CT952_JSTAT): the decode driver stores
         * UNFINISH(2)/OK(1)/FAIL to its JPEG_Status thread var; log small-value
         * byte writes in the HAL data region during the decode window + PC so we
         * can find that var and drive DECODE=OK (roadmap faithful-VIDEO_EN). */
        if (getenv("CT952_JSTAT") && size == 1 && (val == 1u || val == 2u) &&
            addr >= 0x40028000u && addr < 0x40080000u &&
            m->cpu.icount > 9500000ull && m->cpu.icount < 12500000ull) {
            static int js; if (js < 60) {
                fprintf(stderr, "[JSTAT] %08x=%u pc=%08x icount=%llu\n",
                        addr, val, m->cpu.pc, (unsigned long long)m->cpu.icount); js++; }
        }
        /* (CT952_NOMEDIA write-side crutch removed §11.3 -- see the read side.) */
        return;
    }
    if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE) {
        mem_write_raw(m->dram + (addr - 0xC0000000u), val, size);
        return;
    }
    if (addr >= 0x80000000u && addr < 0x80000000u + MACH_IO_SIZE) {
        uint32_t off = (addr - 0x80000000u) & ~3u;
        if (size != 4) {
            /* merge sub-word write into the 32-bit register */
            uint32_t cur = io_get(m, off);
            uint32_t sh = (size == 1) ? (3 - (addr & 3)) * 8
                                      : ((addr & 2) ? 0 : 16);
            uint32_t mask = (size == 1) ? 0xFFu : 0xFFFFu;
            val = (cur & ~(mask << sh)) | ((val & mask) << sh);
        }
        /* Logo-display locator (CT952_LOGOTRACE): DISP_VIDEO_POS/SIZE/EN,
         * F0Y/F0C and JPU_GO/BCR08 are written by the logo/JPEG display path --
         * log PC+icount to find its retail addresses (roadmap item 2). */
        if (getenv("CT952_LOGOTRACE") &&
            ((off == 0x1a48u || off == 0x1ac0u || off == 0x1ac4u ||
              (off == 0x1a4cu && val != 0)) ||   /* skip the compositor VIDEO_EN=0 spam */
             (getenv("CT952_LOGOTRACE_JPU") && (off == 0x2880u || off == 0x2a20u)))) {
            static int lt; if (lt < 500) {
                fprintf(stderr, "[LOGOwr] %08x=%08x pc=%08x sp=%08x icount=%llu\n",
                        addr, val, m->cpu.pc, sparc_get_reg(&m->cpu, 14),
                        (unsigned long long)m->cpu.icount); lt++; }
        }
        if (getenv("CT952_IICTRACE") &&
            (off == 0x4204u || off == 0x4210u || off == 0x4214u)) {
            static int icw; if (icw < 400) {
                fprintf(stderr, "[IICwr] %08x=%08x pc=%08x icount=%llu\n",
                        addr, val, m->cpu.pc, (unsigned long long)m->cpu.icount); icw++; }
        }
        io_write(m, off, val);
        return;
    }
    if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u) {
        mem_write_raw(m->bram + (addr - 0xB0000000u), val, size);
        return;
    }
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return;
    if (addr >= EHCI_BASE && addr < EHCI_END) {
        uint32_t off = (addr - EHCI_BASE) & ~3u;
        if (getenv("CT952_EHCITRACE"))
            fprintf(stderr, "[ehci] wr 0x%08x off 0x%02x <- 0x%08x (pc=0x%08x)\n",
                    addr, off, val, m->cpu.pc);
        ehci_write(m, off, val);
        return;
    }
    if (m->sd_img && addr >= SDC_BASE_ADDR && addr < SDC_BASE_ADDR + 0x100u) {
        if (getenv("CT952_SDCTRACE"))
            fprintf(stderr, "[SDC] wr %08x <- %08x (sz%d) pc=%08x icount=%llu\n",
                    addr, val, size, m->cpu.pc, (unsigned long long)m->cpu.icount);
        sdc_write(m, addr - SDC_BASE_ADDR, val, size);
        return;
    }
    if (addr >= 0xA0000000u && addr < 0xA0010000u) {
        log_access(m, addr & ~3u, 1, val);
        return;
    }
    m->unmapped_writes++;
}

/* ---- bus dispatch: two views onto the same machine ---- */
/* cpu1's bus is machine.bus (first field -> plain cast). cpu2's bus is
 * machine.bus2 (container-of by offset). Both hit the same memory/I/O; only
 * the interrupt wiring differs. */
static machine_t *M2(sparc_bus_t *b)
{ return (machine_t *)((char *)b - offsetof(machine_t, bus2)); }

static uint32_t bus_read(sparc_bus_t *b, uint32_t a, int s, int *f)
{ return bus_rd(M(b), a, s, f); }
static void bus_write(sparc_bus_t *b, uint32_t a, uint32_t v, int s, int *f)
{ bus_wr(M(b), a, v, s, f); }
static uint32_t bus2_read(sparc_bus_t *b, uint32_t a, int s, int *f)
{ return bus_rd(M2(b), a, s, f); }
static void bus2_write(sparc_bus_t *b, uint32_t a, uint32_t v, int s, int *f)
{ bus_wr(M2(b), a, v, s, f); }
/* PROC2 has its own interrupt controller; we don't wire it yet -- the
 * decoder microcode drives the datapath by polling, so run it with no
 * asynchronous interrupts rather than misdelivering PROC1's. */
static int  bus2_irq_level(sparc_bus_t *b) { (void)b; return 0; }
static void bus2_irq_ack(sparc_bus_t *b, int lvl) { (void)b; (void)lvl; }

static int bus_irq_level(sparc_bus_t *b)
{
    machine_t *m = M(b);
    uint32_t pend = io_get(m, R_INT_PENDING) | io_get(m, R_INT_FORCE);
    /* cascade: the secondary PROC1-1st controller drives LEON line 13
     * whenever any of its enabled sources is pending (level-triggered). */
    if (io_get(m, R_P1_1ST_PEND) & io_get(m, R_P1_1ST_MASK))
        pend |= (1u << INT_NO_PROC1_1ST);
    /* cascade: the secondary PROC1-2nd controller drives LEON line 10 whenever
     * any of its enabled sources (IR, SERVO, BIU, MCU, ...) is pending. */
    if (io_get(m, R_P1_2ND_PEND) & io_get(m, R_P1_2ND_MASK))
        pend |= (1u << INT_NO_PROC1_2ND);
    uint32_t eff = pend & io_get(m, R_INT_MASK);
    int lvl;
    for (lvl = 15; lvl >= 1; lvl--)
        if (eff & (1u << lvl)) {
            if (lvl == 13) g_irq13_asserted++;
            return lvl;
        }
    return 0;
}

static void bus_irq_ack(sparc_bus_t *b, int level)
{
    machine_t *m = M(b);
    if (level == 13) g_irq13_taken++;
    if (level >= 0 && level < 16) g_irq_taken[level]++;
    io_set(m, R_INT_PENDING, io_get(m, R_INT_PENDING) & ~(1u << level));
    io_set(m, R_INT_FORCE, io_get(m, R_INT_FORCE) & ~(1u << level));
}

/* ---- timers: 1 cycle per instruction ---- */

static void timer_tick_one(machine_t *m, uint32_t cnt_off, uint32_t rld_off,
                           uint32_t ctl_off, uint32_t irq_bit, uint32_t rld_div)
{
    uint32_t ctl = io_get(m, ctl_off);
    uint32_t cnt;
    if (!(ctl & TIMER_ENABLE)) return;
    cnt = io_get(m, cnt_off);
    if (cnt == 0) {
        if (ctl & TIMER_RELOAD) {
            uint32_t rld = io_get(m, rld_off);
            if (rld_div > 1) { rld /= rld_div; if (!rld) rld = 1; }
            io_set(m, cnt_off, rld);
        }
        io_set(m, R_INT_PENDING, io_get(m, R_INT_PENDING) | irq_bit);
    } else {
        io_set(m, cnt_off, cnt - 1);
    }
}

static void machine_cycle(machine_t *m)
{
    m->cycles++;
    /* Display VSYNC tick: raise the secondary VSYNC-pending bit at the
     * panel field rate so the firmware's display state machine advances.
     * Real timing is ~MCLK/50Hz (~2.66M cycles); we use a shorter, env-
     * tunable divider so many fields elapse within a bring-up run.
     * vsync_cnt also drives the live raster line (disp_cur_line / MEM_LINE):
     * MEM_LINE==0 lines up with the top-of-frame VSYNC assertion here. */
    if (++m->vsync_cnt >= m->vsync_div) {
        m->vsync_cnt = 0;
        m->disp_field ^= 1u;         /* even/odd field toggles each VSYNC */
        m->disp_hsync_grp = 0;       /* re-arm the N-hsync line counter */
        io_set(m, R_P1_1ST_PEND,
               io_get(m, R_P1_1ST_PEND) | IRQ_P1_1ST_VSYNC);
        /* DIAGNOSTIC (CT952_VSYNC_KEEP): the display mode-set at ~9.9M disables
         * the VSYNC interrupt (P1_1ST MDIS bit0) and never re-enables it, freezing
         * the VSYNC-clocked display sequencer at state 7 (§12.60). Force the mask
         * bit back on so VSYNC keeps being delivered -- tests whether continuous
         * VSYNC advances the sequencer to 0xd and unblocks INITIAL_System. */
        if (getenv("CT952_VSYNC_KEEP"))
            io_set(m, R_P1_1ST_MASK, io_get(m, R_P1_1ST_MASK) | IRQ_P1_1ST_VSYNC);
    }
    /* Display line interrupt (REG_DISP_N_HSYNC_INT 0x1A6C -> P1_1ST bit1,
     * INT_PROC1_1ST_HSYNC): "interrupt for each N hsyncs" (§12.50). Guarded on
     * the register being programmed non-zero AND the timing generator enabled,
     * so it costs nothing on this firmware (which never programs it -- the
     * INT_Proc1_1st_isr HSYNC handler is empty in this build) yet fires
     * faithfully at the programmed line cadence if a future path arms it. */
    {
        uint32_t nhs = io_get(m, R_DISP_N_HSYNC) & 0x0FFFu;
        if (nhs && (io_get(m, R_DISP_TGEN_TOTAL) & DISP_TGEN_EN)) {
            uint32_t grp = disp_cur_line(m) / nhs;
            if (grp != m->disp_hsync_grp) {
                m->disp_hsync_grp = grp;
                io_set(m, R_P1_1ST_PEND,
                       io_get(m, R_P1_1ST_PEND) | IRQ_P1_1ST_HSYNC);
            }
        }
    }
    if (m->presc_cnt == 0) {
        m->presc_cnt = io_get(m, R_PRESC_RLD);
        m->t3_value++;
        /* Deferred eCos-clock speedup (CT952_TICK_FAST_AT=<icount>[,<mult>]): the
         * early boot needs the 1x tick so its event-bound waits resolve before
         * their timeouts fire (a fast tick early derails the boot). Once past the
         * early gates, shrink the TIMER1 reload so the eCos clock (advanced once
         * per TIMER1 IRQ) ticks <mult>x faster -- late init OS_DelayTime()s and the
         * screensaver idle timeout then complete in a runnable budget (10.46). */
        uint32_t t1div = 1;
        {
            static long fat = -2; static int fmul = 64;
            if (fat == -2) { const char *e = getenv("CT952_TICK_FAST_AT");
                             fat = -1;
                             if (e) { char *c = NULL; fat = atol(e);
                                      if ((c = strchr((char*)e, ',')) ) fmul = atoi(c + 1); }
                             if (fmul < 1) fmul = 1; }
            if (fat >= 0 && (long)m->cpu.icount >= fat) t1div = (uint32_t)fmul;
        }
        timer_tick_one(m, R_TIMER1_CNT, R_TIMER1_RLD, R_TIMER1_CTL,
                       0x100u, t1div);   /* INT_TIMER1 (eCos clock) */
        timer_tick_one(m, R_TIMER2_CNT, R_TIMER2_RLD, R_TIMER2_CTL,
                       0x200u, 1);   /* INT_TIMER2 */
        /* watchdog counts on the same tick; 0 means untouched/disabled
         * here (real hw needs SYSCFG enable; we only fire if armed) */
        {
            uint32_t wd = io_get(m, R_WATCHDOG);
            if (wd > 1)
                io_set(m, R_WATCHDOG, wd - 1);
            else if (wd == 1)
                m->watchdog_fired = 1;
        }
    } else {
        m->presc_cnt--;
    }
}

int machine_init(machine_t *m, const uint8_t *flash, uint32_t flash_size)
{
    memset(m, 0, sizeof(*m));
    if (flash_size > MACH_FLASH_MAX)
        return -1;
    m->flash = (uint8_t *)malloc(MACH_FLASH_MAX);
    m->dram = (uint8_t *)malloc(MACH_DRAM_SIZE);
    m->bram = (uint8_t *)malloc(0x10000u);
    if (!m->flash || !m->dram || !m->bram)
        return -1;
    memset(m->flash, 0xFF, MACH_FLASH_MAX);
    memcpy(m->flash, flash, flash_size);
    memset(m->dram, 0, MACH_DRAM_SIZE);
    memset(m->bram, 0, 0x10000u);
    m->flash_size = flash_size;
    m->uart_echo = 1;

    /* Optional SD card image (CT952_SDCARD=<path>): a FAT image presented as an
     * inserted card by the SD host controller model, so the media manager
     * enumerates it and the browse UI populates (§12.94). */
    m->sd_img = NULL; m->sd_size = 0;
    {
        const char *e = getenv("CT952_SDCARD");
        if (e && *e) {
            FILE *sf = fopen(e, "rb");
            if (sf) {
                fseek(sf, 0, SEEK_END); long sz = ftell(sf); fseek(sf, 0, SEEK_SET);
                if (sz > 0) {
                    m->sd_img = (uint8_t *)malloc((size_t)sz);
                    if (m->sd_img && fread(m->sd_img, 1, (size_t)sz, sf) == (size_t)sz)
                        m->sd_size = (uint32_t)sz;
                    else { free(m->sd_img); m->sd_img = NULL; }
                }
                fclose(sf);
                fprintf(stderr, "[SDCARD] loaded %s (%u bytes) as inserted card\n",
                        e, m->sd_size);
            } else {
                fprintf(stderr, "[SDCARD] could not open %s\n", e);
            }
        }
    }

    /* display field-rate divider for the VSYNC IRQ (see machine_cycle) */
    {
        const char *e = getenv("CT952_VSYNC_DIV");
        m->vsync_cnt = 0;
        m->vsync_div = e ? (uint32_t)strtoul(e, NULL, 0) : 200000u;
        if (m->vsync_div == 0) m->vsync_div = 200000u;
    }

    /* SYSTEM_CONFIGURATION1 (0x8000031c): hardware strapping the boot code
     * decodes for DRAM/flash type. Bits[4:0] must be 0b11xxx or the AP
     * code-area calc (flash 0x40260) returns the 0x50000000 "unknown DRAM"
     * sentinel and boot-config aborts. Overridable via CT952_SYSCFG1 for
     * bring-up sweeps. */
    {
        const char *e = getenv("CT952_SYSCFG1");
        /* 0x1e -> AP-calc returns 0x40800000 (8 MB / 64 Mbit DRAM top),
         * matching this model's DRAM size. */
        m->io[0x31c / 4] = e ? (uint32_t)strtoul(e, NULL, 0) : 0x1eu;
    }

    m->bus.read = bus_read;
    m->bus.write = bus_write;
    m->bus.irq_level = bus_irq_level;
    m->bus.irq_ack = bus_irq_ack;
    sparc_reset(&m->cpu, &m->bus);

    /* PROC2 second core (gated during bring-up) */
    m->bus2.read = bus2_read;
    m->bus2.write = bus2_write;
    m->bus2.irq_level = bus2_irq_level;
    m->bus2.irq_ack = bus2_irq_ack;
    m->proc2_enable = getenv("CT952_PROC2") ? 1 : 0;
    m->proc2_on = 0;
    /* Decoder-stop dwell (cycles). Default sized to span a boot-thread poll
     * interval (~2 eCos ticks) so the MODE_STOP(0x10) window is visible before
     * the ack to STOPPED(0x11); tunable for bring-up. */
    {
        const char *e = getenv("CT952_VDEC_DWELL");
        m->proc2_ack_dwell = e ? (uint32_t)strtoul(e, NULL, 0) : 300000u;
    }
    m->proc2_ack_cycle = 0;

    /* functional JPEG decode (opt-in via main.c CLI); default source is the
     * power-on logo staging buffer */
    m->jpeg_decode_en = 0;
    m->jpeg_done = 0;
    m->jpeg_src = 0x401dc000u;
    m->jpeg_out = NULL;
    m->jpeg_rgb = NULL;
    m->jpeg_w = m->jpeg_h = 0;
    m->jpeg_sig = 0;
    m->jpeg_count = 0;
    sparc_reset(&m->cpu2, &m->bus2);
    m->cpu2.halted = 1;      /* idle until PROC1 releases it */
    return 0;
}

void machine_seed_boot(machine_t *m, uint32_t entry, uint32_t sp)
{
    if (entry) io_set(m, R_PROC2_START, entry);
    if (sp)    io_set(m, R_PROC2_SP, sp);
}

void machine_uart_feed(machine_t *m, const uint8_t *data, uint32_t len)
{
    uint8_t *nb = (uint8_t *)realloc(m->rx_buf, m->rx_len + len);
    if (!nb) return;
    m->rx_buf = nb;
    memcpy(m->rx_buf + m->rx_len, data, len);
    m->rx_len += len;
}

void machine_free(machine_t *m)
{
    if (getenv("CT952_TRACE")) {
        int L; fprintf(stderr, "[IRQ13] asserted=%ld taken=%ld  [PROC2-reset writes(0x324)]=%ld\n[IRQ take/level]", g_irq13_asserted, g_irq13_taken, g_proc2_reset_writes);
        for (L = 1; L < 16; L++) if (g_irq_taken[L]) fprintf(stderr, " L%d=%ld", L, g_irq_taken[L]);
        fprintf(stderr, "\n");
        /* eCos software tick: OS_GetSysTimer reads *(*(0x400398f8)+8) (64-bit).
         * If this froze while instructions kept running, the RTOS time base
         * (and every OS_DelayTime / poll timeout) is dead. */
        uint32_t clkobj = machine_dram_rd(m, 0x400398f8u, 4);
        if ((clkobj & 0xF0000000u) == 0x40000000u) {
            uint32_t tick = machine_dram_rd(m, clkobj + 8u + 4u, 4); /* low word of 64b */
            fprintf(stderr, "[eCos tick] clkobj=%08x  counter@%08x=%u\n",
                    clkobj, clkobj + 8u, tick);
        }
        /* GPU 2-D op accounting: the power-on menu is drawn via GPU font/blit
         * ops into the OSD plane. Many font ops => _POWERONMENU_ShowIcon ran =>
         * POWERONMENU_Initial was reached. Near-zero => the stall is upstream
         * of the menu (still in INITIAL_System / the logo splash). */
        fprintf(stderr, "[GPU] ops=%llu font=%llu fill/blit-by-mode=",
                (unsigned long long)m->gpu_ops,
                (unsigned long long)m->gpu_font_ops);
        { int gi; for (gi = 0; gi < 8; gi++)
            fprintf(stderr, "%llu ", (unsigned long long)m->gpu_mode_ops[gi]); }
        fprintf(stderr, "\n");
    }
    if (getenv("CT952_TRACE")) {
        /* Dump the last 64 PROC1 PCs: for a persistent high-PIL spin this ring
         * IS the loop. psr shows PIL/ET at exit. */
        int k; uint32_t lo = 0xFFFFFFFFu, hi = 0;
        fprintf(stderr, "[PCRING] psr=%08x (PIL=%u ET=%u)\n", m->cpu.psr,
                (m->cpu.psr & 0xF00u) >> 8, (m->cpu.psr >> 5) & 1u);
        for (k = 0; k < 64; k++) {
            uint32_t p = m->cpu.pc_ring[(m->cpu.pc_ri + k) & 63];
            if (p < lo) lo = p; if (p > hi) hi = p;
            fprintf(stderr, " %08x", p);
            if ((k & 7) == 7) fprintf(stderr, "\n");
        }
        fprintf(stderr, "[PCRING] span=%08x..%08x\n", lo, hi);
        { const char *e = getenv("CT952_PCHIST_N");
          sparc_pchist_dump(stderr, e ? atoi(e) : 16); }
    }
    if (getenv("CT952_TRACE"))
        fprintf(stderr, "[EXIT] pc1=%08x icount1=%llu halted1=%d (%s)  "
                "PROC2 on=%d pc=%08x icount=%llu halted=%d (%s)\n",
                m->cpu.pc, (unsigned long long)m->cpu.icount, m->cpu.halted,
                m->cpu.halt_reason[0] ? m->cpu.halt_reason : "-",
                m->proc2_on, m->cpu2.pc,
                (unsigned long long)m->cpu2.icount, m->cpu2.halted,
                m->cpu2.halt_reason[0] ? m->cpu2.halt_reason : "-");
    free(m->flash);
    free(m->dram);
    free(m->bram);
    free(m->rx_buf);
    free(m->jpeg_rgb);
    m->flash = NULL;
    m->dram = NULL;
    m->bram = NULL;
    m->rx_buf = NULL;
}

/* Env-gated PC sampler (CT952_PCSAMP=threshold): after `threshold` icount,
 * sample the CPU pc once per run chunk into an open-addressed histogram and
 * dump the hottest PCs at exit -- pinpoints the spin loop a boot gate rides. */
#define PCSAMP_N 8192
static uint32_t g_pcsamp_pc[PCSAMP_N], g_pcsamp_ct[PCSAMP_N];
static uint32_t g_spsamp_sp[256], g_spsamp_ct[256];
static uint64_t g_pcsamp_thresh = 0;
static int g_pcsamp_on = -1;
static void pcsamp_dump(void)
{
    int i, j, top = 20;
    fprintf(stderr, "[PCSAMP] top %d hot PCs (sampled per chunk past icount %llu):\n",
            top, (unsigned long long)g_pcsamp_thresh);
    for (j = 0; j < top; j++) {
        int best = -1; uint32_t bc = 0;
        for (i = 0; i < PCSAMP_N; i++)
            if (g_pcsamp_ct[i] > bc) { bc = g_pcsamp_ct[i]; best = i; }
        if (best < 0 || !bc) break;
        fprintf(stderr, "  pc=%08x  %u\n", g_pcsamp_pc[best], bc);
        g_pcsamp_ct[best] = 0;
    }
    fprintf(stderr, "[PCSAMP] stack (sp&~0xfff) buckets:\n");
    for (j = 0; j < 12; j++) {
        int best = -1; uint32_t bc = 0;
        for (i = 0; i < 256; i++)
            if (g_spsamp_ct[i] > bc) { bc = g_spsamp_ct[i]; best = i; }
        if (best < 0 || !bc) break;
        fprintf(stderr, "  sp~%08x  %u\n", g_spsamp_sp[best], bc);
        g_spsamp_ct[best] = 0;
    }
}
static void spsamp_hit(uint32_t sp)
{
    uint32_t bucket = sp & 0xFFFFF000u;   /* page-granular stack bucket */
    uint32_t h = (bucket >> 12) & 0xFF, i;
    for (i = 0; i < 256; i++) {
        uint32_t k = (h + i) & 0xFF;
        if (g_spsamp_ct[k] == 0) { g_spsamp_sp[k] = bucket; g_spsamp_ct[k] = 1; return; }
        if (g_spsamp_sp[k] == bucket) { g_spsamp_ct[k]++; return; }
    }
}
static void pcsamp_hit(uint32_t pc)
{
    uint32_t h = (pc * 2654435761u) & (PCSAMP_N - 1), i;
    for (i = 0; i < PCSAMP_N; i++) {
        uint32_t k = (h + i) & (PCSAMP_N - 1);
        if (g_pcsamp_ct[k] == 0) { g_pcsamp_pc[k] = pc; g_pcsamp_ct[k] = 1; return; }
        if (g_pcsamp_pc[k] == pc) { g_pcsamp_ct[k]++; return; }
    }
}
/* Windowed periodic dump (CT952_PCSAMP_WIN=<icount window>, default 200M): print
 * the hottest PCs/stacks for the CURRENT window and reset the histograms, so a
 * long run that the sandbox kills before atexit still leaves the live spin loop
 * in its log. Distinct from pcsamp_dump (cumulative, atexit). */
static uint64_t g_pcsamp_lastdump = 0;
static uint64_t g_pcsamp_win = 0;
static void pcsamp_window_dump(uint64_t icount)
{
    int i, j;
    fprintf(stderr, "[PCSAMP @%lluM] window top PCs:\n",
            (unsigned long long)(icount / 1000000));
    for (j = 0; j < 10; j++) {
        int best = -1; uint32_t bc = 0;
        for (i = 0; i < PCSAMP_N; i++)
            if (g_pcsamp_ct[i] > bc) { bc = g_pcsamp_ct[i]; best = i; }
        if (best < 0 || !bc) break;
        fprintf(stderr, "   pc=%08x  %u\n", g_pcsamp_pc[best], bc);
        g_pcsamp_ct[best] = 0;
    }
    for (j = 0; j < 4; j++) {
        int best = -1; uint32_t bc = 0;
        for (i = 0; i < 256; i++)
            if (g_spsamp_ct[i] > bc) { bc = g_spsamp_ct[i]; best = i; }
        if (best < 0 || !bc) break;
        fprintf(stderr, "   sp~%08x  %u\n", g_spsamp_sp[best], bc);
        g_spsamp_ct[best] = 0;
    }
    /* full reset so the next window is clean */
    for (i = 0; i < PCSAMP_N; i++) { g_pcsamp_ct[i] = 0; }
    for (i = 0; i < 256; i++) { g_spsamp_ct[i] = 0; }
}

uint64_t machine_run(machine_t *m, uint64_t n)
{
    uint64_t done = 0;
    if (g_pcsamp_on < 0) {
        const char *e = getenv("CT952_PCSAMP");
        g_pcsamp_on = e ? 1 : 0;
        if (e) { g_pcsamp_thresh = strtoull(e, NULL, 0); atexit(pcsamp_dump);
            const char *w = getenv("CT952_PCSAMP_WIN");
            g_pcsamp_win = w ? strtoull(w, NULL, 0) : 200000000ull; }
    }
    static int g_reach = -1;
    static uint64_t ccev = 0; static int ccev_done = 0;
    static int g_logoev = -1;
    if (g_logoev < 0) g_logoev = getenv("CT952_LOGOEVENT") ? 1 : 0;
    if (g_reach < 0) g_reach = getenv("CT952_REACH") ? 1 : 0;
    if (!ccev && !ccev_done) { const char *e = getenv("CT952_CCEVENT");
                               ccev = e ? strtoull(e, NULL, 0) : 0; if (!ccev) ccev_done = 1; }
    /* One-shot faithful IR keypress (CT952_IRKEY="<scancode>[@<icount>]"): drive
     * the modeled IR receiver + PROC1-2nd IR interrupt so the firmware's own
     * INT_Proc1_2nd_isr / DSR_IR / INPUT_RemoteScan decode the key into __bISRKey
     * (10.38). scancode indexes _IRInfo.aIRMap; icount defaults to 40M (Loading). */
    static int irkey_init = -1; static uint32_t irkey_code = 0;
    static uint64_t irkey_at = 40000000ull; static int irkey_done = 0;
    if (irkey_init < 0) {
        const char *e = getenv("CT952_IRKEY");
        irkey_init = e ? 1 : 0;
        if (e) { char *at = NULL; irkey_code = (uint32_t)strtoul(e, &at, 0);
                 if (at && *at == '@') irkey_at = strtoull(at + 1, NULL, 0); }
        else irkey_done = 1;
    }
    static long snap_at = -2; static int snap_done = 0;
    if (snap_at == -2) { const char *e = getenv("CT952_SNAP");
                         snap_at = e ? (long)strtoull(e, NULL, 0) : -1; }
    /* DIAGNOSTIC PROBE (CT952_CARDSHOW=<icount>): past <icount>, point the JPU
     * decode source at the card photo buffer 0x401ec000 (info.a loaded 01.JPG
     * there). Proves the display pipeline decodes the card image end-to-end --
     * isolating the remaining gap to the firmware's auto-play trigger (which
     * would DMA the photo into the decode buffer / kick the JPU). NOT faithful;
     * a probe only. */
    static long cardshow_at = -2;
    if (cardshow_at == -2) { const char *e = getenv("CT952_CARDSHOW");
                             cardshow_at = e ? (long)strtoull(e, NULL, 0) : -1; }
    while (done < n && !m->cpu.halted && !m->watchdog_fired) {
        if (cardshow_at >= 0 && m->cpu.icount > (uint64_t)cardshow_at) {
            static int cardshow_done = 0;
            if (!cardshow_done) {
                /* info.a loads only the first 16KB (header) of the photo; for the
                 * probe, stage the FULL 01.JPG (card sector 67, contiguous) into
                 * the decode buffer so the whole image decodes. */
                uint8_t *dst = machine_dram_ptr(m, 0x401ec000u);
                if (m->sd_img && dst) {
                    uint32_t foff = 67u * 512u, flen = 0x10000u; /* 64KB covers 01.JPG */
                    if (foff + flen <= m->sd_size)
                        memcpy(dst, m->sd_img + foff, flen);
                }
                m->jpeg_src = 0x401ec000u;
                m->biu_drained = 0;
                m->jpeg_sig = 0;              /* force re-decode */
                machine_maybe_jpeg_decode(m);
                cardshow_done = 1;
                fprintf(stderr, "[CARDSHOW] forced full-photo decode of card 0x401ec000 at icount=%llu\n",
                        (unsigned long long)m->cpu.icount);
            }
        }
        /* One-shot call-chain snapshot (CT952_SNAP=<icount>): dump the winframe
         * backtrace + recent-PC ring once past <icount> -- names the info.a
         * functions in the post-engine recursion loop (§12.98). */
        if (snap_at >= 0 && !snap_done && m->cpu.icount > (uint64_t)snap_at) {
            uint32_t bt[32]; int nb = sparc_win_backtrace(&m->cpu, bt, 32), bi, k;
            fprintf(stderr, "[SNAP] pc=%08x winframes:", m->cpu.pc);
            for (bi = 0; bi < nb; bi++) fprintf(stderr, " %08x", bt[bi]);
            fprintf(stderr, "\n[SNAP] recent-PC ring:");
            for (k = 20; k < 64; k++)
                fprintf(stderr, " %08x", m->cpu.pc_ring[(m->cpu.pc_ri + k) & 63]);
            fprintf(stderr, " icount=%llu\n", (unsigned long long)m->cpu.icount);
            snap_done = 1;
        }
        /* One-shot CC event-flag poke (CT952_CCEVENT=<icount>): the CC/boot
         * thread spins in a wait-for-event dispatcher polling the flag object at
         * 0x40026EA4 for bit 0x1000 (peek-and-clear via flash 0x66a0), redrawing
         * "Loading" each iteration. Post that bit once, past the given icount, to
         * probe what the firmware does when the event fires. Retail-verified addr
         * (from the live thread-stack decode, not the SDK symbols). */
        if (!ccev_done && m->cpu.icount > ccev) {
            uint32_t v = mem_read_raw(m->dram + 0x26ea4u, 4);
            mem_write_raw(m->dram + 0x26ea4u, v | 0x1000u, 4);
            fprintf(stderr, "[CCEVENT] posted flag 0x40026EA4 |= 0x1000 at icount=%llu\n",
                    (unsigned long long)m->cpu.icount);
            ccev_done = 1;
        }
        /* Experiment (CT952_SET_POM=<icount>): the OSDSS screen saver is gated in
         * OSDSS_Monitor behind __bPOWERONMENUInitial != 0 (0x40023a10), which stays 0
         * because POWERONMENU_Initial never completes. Set it to 1 once, past <icount>,
         * to test whether OSDSS_Entry then fires after the ~58s idle timeout and the
         * genuine OSDSS screen saver renders. */
        { static long sp = -2; static int sp_done = 0;
          if (sp == -2) { const char *e = getenv("CT952_SET_POM");
              sp = e ? (long)strtoull(e, NULL, 0) : -1; }
          if (sp >= 0 && !sp_done && m->cpu.icount > (uint64_t)sp) {
              mem_write_raw(m->dram + 0x23a10u, 1, 1);
              fprintf(stderr, "[SETPOM] __bPOWERONMENUInitial=1 at icount=%llu\n",
                      (unsigned long long)m->cpu.icount);
              sp_done = 1;
          }
        }
        /* Experiment (CT952_POKE="<hexaddr>=<val>[:<size>]@<icount>"): one-shot
         * byte/word write into DRAM past <icount>, to test whether forcing a gate
         * (e.g. the display-tick transition gate 0x4002fb48=1) advances the pump to
         * an interactive UI handler and makes keys dispatch. */
        { static long pk_at = -2; static int pk_done = 0;
          static uint32_t pk_addr = 0, pk_val = 0, pk_sz = 1;
          if (pk_at == -2) { const char *e = getenv("CT952_POKE"); pk_at = -1;
              if (e) { char b[96]; strncpy(b, e, 95); b[95] = 0;
                  char *at = strchr(b, '@'); if (at) { *at = 0; pk_at = (long)strtoull(at + 1, NULL, 0); }
                  char *sz = strchr(b, ':'); if (sz) { *sz = 0; pk_sz = (uint32_t)strtoul(sz + 1, NULL, 0); }
                  char *eq = strchr(b, '='); if (eq) { *eq = 0; pk_val = (uint32_t)strtoul(eq + 1, NULL, 0); }
                  pk_addr = (uint32_t)strtoul(b, NULL, 0); } }
          if (pk_at >= 0 && !pk_done && m->cpu.icount > (uint64_t)pk_at &&
              pk_addr >= 0x40000000u) {
              mem_write_raw(m->dram + (pk_addr - 0x40000000u), pk_val, (int)pk_sz);
              fprintf(stderr, "[POKE] *%08x = %u (size %u) at icount=%llu\n",
                      pk_addr, pk_val, pk_sz, (unsigned long long)m->cpu.icount);
              pk_done = 1;
          }
        }
        /* Diagnostic (CT952_DRAMDUMP="<hexaddr>:<hexlen>@<icount>"): one-shot raw
         * DRAM dump to /tmp/dramdump.bin past <icount>, for offline geometry
         * analysis (e.g. the OSD plane's true stride). */
        { static long dd_at = -2; static int dd_done = 0;
          static uint32_t dd_addr = 0, dd_len = 0;
          if (dd_at == -2) { const char *e = getenv("CT952_DRAMDUMP"); dd_at = -1;
              if (e) { char b[96]; strncpy(b, e, 95); b[95] = 0;
                  char *at = strchr(b, '@'); if (at) { *at = 0; dd_at = (long)strtoull(at + 1, NULL, 0); }
                  char *cl = strchr(b, ':'); if (cl) { *cl = 0; dd_len = (uint32_t)strtoul(cl + 1, NULL, 0); }
                  dd_addr = (uint32_t)strtoul(b, NULL, 0); } }
          if (dd_at >= 0 && !dd_done && m->cpu.icount > (uint64_t)dd_at &&
              dd_addr >= 0x40000000u && dd_len) {
              const uint8_t *p = machine_dram_ptr(m, dd_addr);
              FILE *df = p ? fopen("/tmp/dramdump.bin", "wb") : NULL;
              if (df) { fwrite(p, 1, dd_len, df); fclose(df);
                  fprintf(stderr, "[DRAMDUMP] %08x len %u -> /tmp/dramdump.bin at icount=%llu\n",
                      dd_addr, dd_len, (unsigned long long)m->cpu.icount); }
              dd_done = 1;
          }
        }
        if (!irkey_done && m->cpu.icount > irkey_at) {
            /* present a clean NEC data frame (not repeat 0x100, not invalid 0x400)
             * with the scancode in the low byte; customer=0x00 customer1=0xFF.
             * Also enable the IR source in the PROC1-2nd mask: on real silicon IR
             * is enabled, but the stuck-at-Loading boot never ran that init, so the
             * secondary mask lacks bit2 -- set it so the cascade can deliver. */
            io_set(m, R_IR_DATA, irkey_code & 0xFFu);
            io_set(m, R_IR_RAWCODE, 0x00FF0000u | ((irkey_code & 0xFFu) << 8));
            io_set(m, R_P1_2ND_MASK, io_get(m, R_P1_2ND_MASK) | INT_P1_2ND_IR);
            io_set(m, R_P1_2ND_PEND, io_get(m, R_P1_2ND_PEND) | INT_P1_2ND_IR);
            fprintf(stderr, "[IRKEY] injected scancode 0x%02x: IR_DATA=%08x "
                    "P1_2ND mask=%08x pend=%08x leonmask=%08x at icount=%llu\n",
                    irkey_code & 0xFF, io_get(m, R_IR_DATA), io_get(m, R_P1_2ND_MASK),
                    io_get(m, R_P1_2ND_PEND), io_get(m, R_INT_MASK),
                    (unsigned long long)m->cpu.icount);
            irkey_done = 1;
        }
        uint64_t chunk = n - done;
        uint64_t ran, i;
        /* Logo/status-state event injection (CT952_LOGOEVENT): the power-on
         * state-8 handler advances only when its post-wait check (flash 0x254a4)
         * returns 0 -- which happens when the key/event queue (0x400329FC) has a
         * pending message. With no input and the decoder in reset the queue stays
         * empty. Simulate "an event arrived" surgically: at the instruction right
         * after `call 0x254a4` (0x26e78), force its return value %o0=0 so the
         * handler takes the advance path (0x26ea0 -> 0x2605c). Preserves 0x254a4's
         * side effects (it still ran). Past 30M icount so it only fires in the
         * Loading state. See DP700WD_HW_REFERENCE.md 10.23. */
        if (g_logoev && m->cpu.icount > 30000000ull) {
            /* Breakpoint at 0x26e78 (the instr after `call 0x254a4`): stop there
             * without single-stepping the whole run. Also keep the CC event flag
             * 0x1000 posted so the modal-wait dispatcher exits each poll and the
             * handler reaches the queue check that leads to 0x26e78. Gated past
             * 30M icount so only the Loading state is affected (event 0x1000 is
             * reused by earlier boot states). */
            m->cpu.brk_pc = 0x26e78u;
            uint32_t v = mem_read_raw(m->dram + 0x26ea4u, 4);
            if (!(v & 0x1000u)) mem_write_raw(m->dram + 0x26ea4u, v | 0x1000u, 4);
            if (m->cpu.pc == 0x26e78u) {
                static int announced = 0;
                sparc_set_reg(&m->cpu, 8, 0);   /* %o0 = 0 -> 0x254a4 "event pending" */
                if (!announced) { announced = 1;
                    fprintf(stderr, "[LOGOEVENT] reached 0x26e78, forcing advance at icount=%llu\n",
                            (unsigned long long)m->cpu.icount); }
                m->cpu.brk_pc = 0;                     /* clear so we can step past it */
                sparc_run(&m->cpu, 1);                 /* execute the forced instr */
                for (i = 0; i < 1; i++) machine_cycle(m);
                done += 1;
                continue;
            }
        }
        if (g_reach) {
            static const struct { uint32_t pc; const char *name; } wl[] = {
                {0x0000eb90u,"INITIAL_System(F)"}, {0x0000f6f8u,"ShowFirstLOGO(F)"},
                {0x0004b808u,"POWERONMENU_Initial(F)"}, {0x00002014u,"CC_DVD_MainLoop(F)"},
                {0x00002318u,"Thread_CTKDVD(F)"}, {0x0001152cu,"MEDIA_Management(F)"},
                {0x0001186cu,"MEDIA_MonitorStatus(F)"}, {0x000118b8u,"_MEDIA_MonitorMediaStatus(F)"},
                {0x4000eb90u,"INITIAL_System(D)"}, {0x4004b808u,"POWERONMENU_Initial(D)"},
                {0x00012f10u,"PostEvent->list(F)"}, {0x00006eecu,"EvtDispatch_bit80(F)"},
                {0x00061170u,"UTL_ShowJPEG_Slide(F)"}, {0x000591b4u,"OSDSS_Monitor(F)"},
                {0x00059108u,"OSDSS_Entry(F)"}, {0x00059004u,"_OSDSS_PictureUpdate(F)"},
                {0x0007f0c4u,"DecoderDSR_A(F)"}, {0x000830acu,"DecoderDSR_B(F)"},
                {0x00006130u,"MediaPresentPost(F)"}, {0x00045660u,"MonitorThrottle(F)"},
            };
            static uint8_t hit[24];
            int wi;
            for (wi = 0; wi < (int)(sizeof(wl)/sizeof(wl[0])); wi++)
                if (!hit[wi] && m->cpu.pc == wl[wi].pc) {
                    hit[wi] = 1;
                    fprintf(stderr, "[REACH] %-26s pc=%08x icount=%llu\n",
                            wl[wi].name, wl[wi].pc, (unsigned long long)m->cpu.icount);
                }
            chunk = 1;   /* single-step so every function entry is observed */
        }
        if (g_pcsamp_on && m->cpu.icount > g_pcsamp_thresh) {
            uint32_t sp = sparc_get_reg(&m->cpu, 14);
            static uint32_t spfilt = 0xFFFFFFFFu;
            if (spfilt == 0xFFFFFFFFu) {
                const char *e = getenv("CT952_PCSAMP_SP");
                spfilt = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
            }
            spsamp_hit(sp);
            if (!spfilt || (sp & 0xFFFFF000u) == (spfilt & 0xFFFFF000u)) {
                uint32_t pc = m->cpu.pc;
                /* When the boot thread sits in memcpy (flash 0xd3900..0xd39a4),
                 * sample the return address (%o7) instead so we see the CALLER. */
                if (getenv("CT952_PCSAMP_O7") && pc >= 0xd3900u && pc <= 0xd39a4u)
                    pc = sparc_get_reg(&m->cpu, 15);
                pcsamp_hit(pc);
            }
            if (g_pcsamp_win && m->cpu.icount - g_pcsamp_lastdump >= g_pcsamp_win) {
                g_pcsamp_lastdump = m->cpu.icount;
                pcsamp_window_dump(m->cpu.icount);
            }
        }
        /* Faithful panel-config build (opt-in): at the first fetch of the
         * config thunk (flash 0x3d564), run the firmware's own descriptor
         * builder (0x3ce60) on the live machine -- it reads the real SETD
         * settings sector -- preserving the boot CPU context across the
         * call. Reproduces the default-init pass the eCos init-callback
         * list would run before the apply. NOTE: builds the descriptor
         * (desc+0x10/0x14 from SETD) but is not yet sufficient on its own
         * -- the inner register table at 0x40042000 is populated by the
         * apply itself, which is the next layer. */
        if (m->build_panelcfg && !m->panelcfg_built) {
            if (m->cpu.pc == 0x3d564u) {
                sparc_t save = m->cpu;
                machine_call(m, 0x3ce60u, 0, 0, 0, 0x40700000u, 50000000ull);
                m->cpu = save;
                m->panelcfg_built = 1;
            } else {
                chunk = 1;   /* single-step until the thunk is reached */
            }
        }
        if (chunk > 4096) chunk = 4096;
        ran = sparc_run(&m->cpu, chunk);
        done += ran;
        for (i = 0; i < ran; i++)
            machine_cycle(m);
        /* Interleave PROC2 on the same wall-clock budget. It shares the bus
         * (DRAM / vdec SRAM / JPU), so its decode work is visible to PROC1. */
        if (m->proc2_on && !m->cpu2.halted)
            sparc_run(&m->cpu2, ran ? ran : chunk);
        /* A short run because we stopped AT the breakpoint is not a halt --
         * keep going so the next iteration can intervene. */
        if (ran < chunk && !(m->cpu.brk_pc && m->cpu.pc == m->cpu.brk_pc))
            break;
    }
    return done;
}

/* --- snapshot / restore (fast re-attach for the gdb stub) ---------------- *
 * Dump the whole machine (registers + I/O + DRAM/BRAM/flash) so a boot to an
 * interesting point can be reached once and re-loaded instantly. Function
 * pointers are NOT trusted across processes (PIE/ASLR) -- the bus vtable and
 * cpu.bus links are re-established on restore. Heap pointers not needed for
 * debugging (rx_buf, jpeg_rgb, jpeg_out, uart_file) are reset. */
#define MACH_SNAP_MAGIC 0x43543935u   /* "CT95" */

int machine_snapshot(machine_t *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t magic = MACH_SNAP_MAGIC;
    if (!f) return -1;
    fwrite(&magic, 4, 1, f);
    fwrite(m, sizeof(*m), 1, f);              /* struct (pointers ignored on load) */
    fwrite(m->flash, MACH_FLASH_MAX, 1, f);
    fwrite(m->dram, MACH_DRAM_SIZE, 1, f);
    fwrite(m->bram, 0x10000u, 1, f);
    fclose(f);
    return 0;
}

int machine_restore(machine_t *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t magic = 0;
    uint8_t *flash = m->flash, *dram = m->dram, *bram = m->bram;
    FILE *uf = m->uart_file;
    int echo = m->uart_echo;
    if (!f) return -1;
    if (fread(&magic, 4, 1, f) != 1 || magic != MACH_SNAP_MAGIC) { fclose(f); return -1; }
    if (fread(m, sizeof(*m), 1, f) != 1) { fclose(f); return -1; }
    /* restore the live heap buffers + their contents */
    m->flash = flash; m->dram = dram; m->bram = bram;
    if (fread(m->flash, MACH_FLASH_MAX, 1, f) != 1 ||
        fread(m->dram, MACH_DRAM_SIZE, 1, f) != 1 ||
        fread(m->bram, 0x10000u, 1, f) != 1) { fclose(f); return -1; }
    fclose(f);
    /* reset host-only pointers that the snapshot's stale values would corrupt */
    m->uart_file = uf; m->uart_echo = echo;
    m->rx_buf = NULL; m->rx_len = m->rx_pos = 0;
    m->jpeg_rgb = NULL; m->jpeg_out = NULL;
    /* re-establish the bus vtable + cpu links in THIS process */
    m->bus.read = bus_read;   m->bus.write = bus_write;
    m->bus.irq_level = bus_irq_level; m->bus.irq_ack = bus_irq_ack;
    m->bus2.read = bus2_read; m->bus2.write = bus2_write;
    m->bus2.irq_level = bus2_irq_level; m->bus2.irq_ack = bus2_irq_ack;
    m->cpu.bus = &m->bus; m->cpu2.bus = &m->bus2;
    return 0;
}

/* --- gdb-stub support (see gdbstub.c) ------------------------------------ *
 * Run up to `maxsteps` instructions with the full per-instruction device
 * tick (timers/IRQ/PROC2), stopping the instant the PROC1 PC lands on any of
 * `bps`. Returns 1 if a breakpoint was hit (pc now sits ON it, unexecuted),
 * 0 if maxsteps ran out, -1 if the CPU halted/watchdog-fired. `*out_steps`
 * gets the instruction count actually run. The tight loop lives here (not in
 * the stub) so a "continue" batch stays fast between socket polls. */
int machine_step_bp(machine_t *m, const uint32_t *bps, int nbp,
                    uint64_t maxsteps, uint64_t *out_steps)
{
    uint64_t s = 0;
    int rc = 0, i;
    m->cpu.brk_pc = 0;   /* the stub manages breakpoints itself */
    while (s < maxsteps) {
        if (m->cpu.halted || m->watchdog_fired) { rc = -1; break; }
        if (sparc_run(&m->cpu, 1) == 0) { rc = -1; break; }
        machine_cycle(m);
        if (m->proc2_on && !m->cpu2.halted)
            sparc_run(&m->cpu2, 1);
        s++;
        for (i = 0; i < nbp; i++)
            if (m->cpu.pc == bps[i]) { rc = 1; goto done; }
    }
done:
    if (out_steps) *out_steps = s;
    return rc;
}

/* Debug memory access for the stub: hit the backing stores directly (no I/O
 * side effects). Covers flash (XIP low), DRAM (+ 0xC0000000 alias) and the
 * 0xB0000000 scratch SRAM; other regions read 0 / ignore writes. size=1/2/4. */
uint32_t machine_dbg_read(machine_t *m, uint32_t addr, int size)
{
    if (addr + (uint32_t)size <= m->flash_size)
        return mem_read_raw(m->flash + addr, size);
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE)
        return mem_read_raw(m->dram + (addr - 0x40000000u), size);
    if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE)
        return mem_read_raw(m->dram + (addr - 0xC0000000u), size);
    if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u)
        return mem_read_raw(m->bram + (addr - 0xB0000000u), size);
    return 0;
}

void machine_dbg_write(machine_t *m, uint32_t addr, uint32_t val, int size)
{
    if (addr >= 0x40000000u && addr + (uint32_t)size <= 0x40000000u + MACH_DRAM_SIZE)
        mem_write_raw(m->dram + (addr - 0x40000000u), val, size);
    else if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE)
        mem_write_raw(m->dram + (addr - 0xC0000000u), val, size);
    else if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u)
        mem_write_raw(m->bram + (addr - 0xB0000000u), val, size);
    else if (addr + (uint32_t)size <= m->flash_size)
        mem_write_raw(m->flash + addr, val, size);   /* allow XIP code patch */
}

#define CALL_SENTINEL 0xE0000000u

int machine_call(machine_t *m, uint32_t entry,
                 uint32_t a0, uint32_t a1, uint32_t a2,
                 uint32_t sp, uint64_t budget)
{
    sparc_t *c = &m->cpu;
    uint64_t i;

    /* Trap-free environment: S=1, ET=0, PIL=15, CWP=0; WIM=0 so save/
     * restore just rotate windows (no overflow/underflow traps). */
    c->halted = 0;
    c->psr = 0xF3000F00u | PSR_S;    /* impl/ver | PIL=15 | S, ET=0 */
    c->wim = 0;
    c->pc = entry;
    c->npc = entry + 4;
    sparc_set_reg(c, 8, a0);          /* %o0 */
    sparc_set_reg(c, 9, a1);          /* %o1 */
    sparc_set_reg(c, 10, a2);         /* %o2 */
    sparc_set_reg(c, 11, 0);          /* %o3 (extra args -> 0/NULL) */
    sparc_set_reg(c, 12, 0);          /* %o4 */
    sparc_set_reg(c, 13, 0);          /* %o5 */
    sparc_set_reg(c, 14, sp);         /* %o6 / %sp */
    sparc_set_reg(c, 15, CALL_SENTINEL - 8); /* %o7: retl -> sentinel */

    for (i = 0; i < budget; i++) {
        if (c->pc == CALL_SENTINEL) return 0;
        if (c->halted) return -1;
        sparc_run(c, 1);
    }
    return -2;
}

uint32_t machine_dram_rd(machine_t *m, uint32_t addr, int size)
{
    int f = 0;
    return bus_read(&m->bus, addr, size, &f);
}

uint8_t *machine_dram_ptr(machine_t *m, uint32_t addr)
{
    if (addr >= 0x40000000u && addr < 0x40000000u + MACH_DRAM_SIZE)
        return m->dram + (addr - 0x40000000u);
    return NULL;
}

/* Public accessor for an IO register (offset from 0x80000000), for diagnostics. */
uint32_t machine_io_get(machine_t *m, uint32_t off) { return io_get(m, off); }

/* ---- Stock-ROM section loader (mask-ROM equivalent) ---- */

#define UZIP_DECODE  0x00002C50u   /* UZIP blob @ flash 0x2000, wrapper +0xc50 */
#define UZIP_WORKMEM 0x40400000u   /* decompress scratch (clear of sections) */
#define UZIP_SP      0x40700000u

static uint32_t flash_be32(machine_t *m, uint32_t off)
{
    if (off + 4 > m->flash_size) return 0;
    return ((uint32_t)m->flash[off] << 24) | ((uint32_t)m->flash[off+1] << 16) |
           ((uint32_t)m->flash[off+2] << 8) | m->flash[off+3];
}

/* One load pass. only_data: 0 = load everything except DATA, 1 = only
 * DATA. DATA is applied last so it wins its overlap with SFAT (the VSR
 * table at DATA's base must be authoritative). */
static uint32_t rom_load_pass(machine_t *m, FILE *log, int only_data)
{
    uint32_t off = 0x10, romv_entry = 0;

    while (off + 24 <= m->flash_size) {
        char nm[5];
        uint32_t lma, rma, lsz, rsz;
        int i, printable = 1, is_data;
        for (i = 0; i < 4; i++) {
            nm[i] = (char)m->flash[off + i];
            if (nm[i] < 32 || nm[i] > 126) printable = 0;
        }
        nm[4] = 0;
        if (!printable) break;
        lma = flash_be32(m, off + 4);  rma = flash_be32(m, off + 8);
        lsz = flash_be32(m, off + 12); rsz = flash_be32(m, off + 16);
        off += 24;

        is_data = !memcmp(nm, "DATA", 4);
        if (is_data != only_data)
            continue;

        if (lma < 0x40000000u || lma + lsz > 0x40000000u + MACH_DRAM_SIZE) {
            if (log) fprintf(log, "  %-4s %10x %9x %9x %8x  (flash/skip)\n",
                             nm, lma, rma, lsz, rsz);
            continue;   /* flash-XIP or non-DRAM: nothing to stage */
        }
        if (!memcmp(nm, "ROMV", 4)) romv_entry = lma;

        if (rsz < lsz) {
            /* zipped: run the firmware's decompressor */
            int rc = machine_call(m, UZIP_DECODE, rma, lma, UZIP_WORKMEM,
                                  UZIP_SP, 200000000ull);
            if (log) fprintf(log, "  %-4s %10x %9x %9x %8x  UNZIP rc=%d\n",
                             nm, lma, rma, lsz, rsz, rc);
            if (rc != 0) return 0;
        } else {
            /* raw: copy flash -> DRAM */
            uint8_t *dst = machine_dram_ptr(m, lma);
            if (dst && rma + lsz <= m->flash_size)
                memcpy(dst, m->flash + rma, lsz);
            if (log) fprintf(log, "  %-4s %10x %9x %9x %8x  copy\n",
                             nm, lma, rma, lsz, rsz);
        }
    }
    return romv_entry;
}

uint32_t machine_rom_load(machine_t *m, FILE *log)
{
    uint32_t entry;
    if (log) fprintf(log, "# section  run       flash     unpacked  packed   action\n");
    rom_load_pass(m, log, 0);              /* everything except DATA */
    entry = rom_load_pass(m, log, 1);      /* DATA last (wins SFAT overlap) */
    /* entry (ROMV) came from pass 0; recover it */
    if (!entry) {
        /* ROMV is loaded in pass 0; re-scan for its LMA */
        uint32_t off = 0x10;
        while (off + 24 <= m->flash_size) {
            if (m->flash[off] < 32 || m->flash[off] > 126) break;
            if (!memcmp(m->flash + off, "ROMV", 4)) {
                entry = flash_be32(m, off + 4); break;
            }
            off += 24;
        }
    }
    sparc_reset(&m->cpu, &m->bus);   /* clean boot state; DRAM preserved */
    return entry;
}

static int cmp_log(const void *a, const void *b)
{
    const mach_logent_t *x = (const mach_logent_t *)a;
    const mach_logent_t *y = (const mach_logent_t *)b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

void machine_dump_iolog(machine_t *m, FILE *f)
{
    int i;
    qsort(m->log, (size_t)m->log_n, sizeof(m->log[0]), cmp_log);
    fprintf(f, "# unmodeled I/O access inventory (%d unique regs)\n",
            m->log_n);
    fprintf(f, "# addr        reads   writes  last_write\n");
    for (i = 0; i < m->log_n; i++)
        fprintf(f, "0x%08x  %6u  %6u  0x%08x\n",
                m->log[i].addr, m->log[i].reads, m->log[i].writes,
                m->log[i].last_write);
    fprintf(f, "# unmapped: %u reads, %u writes\n",
            m->unmapped_reads, m->unmapped_writes);
}

/* ---- DISP display engine: OSD plane scanout ------------------------ *
 * The stock display path is a blob (display.a), but the OSD plane it
 * scans is fully described by header-visible state: the 8bpp palette-
 * indexed pixels live linearly in DRAM (the firmware's OSD region,
 * DS_OSDFRAME_ST = 0x4005F000), and the colour palette is the DISP
 * GAM_OSD RAM at 0x80001C00 -- 256 words of 0x00YYUUVV, BT.601 studio
 * range (jrgb2yuv.c). This composites that plane to an RGB PPM, exactly
 * what the DISP scan-out does before the panel TCON. Register offsets
 * from ctkav_disp.h. */
#define R_DISP_OSD_SIZE  0x1A54          /* bit28 = DISP_OSD_EN */
#define R_DISP_GAM_OSD   0x1C00          /* GAM_OSD[n] = +n*4, 256 entries */
#define DISP_OSD_EN      0x10000000u

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Sample one RGB pixel of the de-tiled video/slideshow plane (macroblock-tiled
 * YUV 4:2:0 at 0x40065000/0x400B3C00, strip 0x2D00) at native coords (vx,vy).
 * The single source of truth for the tile geometry shared by the video scan-out
 * and the panel composite. Returns 0 if the plane isn't mapped. */
static uint32_t video_sample_rgb(machine_t *m, uint32_t vx, uint32_t vy)
{
    const uint32_t strip = 0x2D00u;
    const uint8_t *yb = machine_dram_ptr(m, 0x40065000u);
    const uint8_t *cb = machine_dram_ptr(m, 0x400B3C00u);
    uint32_t yo, cx, cy, co;
    int Y, U, V, R, G, B;
    if (!yb || !cb) return 0;
    yo = (vy >> 4) * strip + (vx >> 2) * 64u + (vy & 15) * 4u + (vx & 3);
    cx = vx >> 1; cy = vy >> 1;
    co = (cy >> 4) * strip + (cx >> 3) * 256u + ((cx & 7) >> 2) * 64u
       + (cy & 15) * 4u + (cx & 3);
    Y = yb[yo];
    U = (int)cb[co] - 128;
    V = (int)cb[co + 128] - 128;
    R = clamp8(Y + ((91881 * V) >> 16));
    G = clamp8(Y - ((22554 * U + 46802 * V) >> 16));
    B = clamp8(Y + ((116130 * U) >> 16));
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

int machine_disp_scanout(machine_t *m, uint32_t osd_base,
                         uint32_t w, uint32_t h, uint32_t stride,
                         const char *ppm_path)
{
    uint32_t pal[256];
    const uint8_t *fb;
    uint64_t span;
    FILE *f;
    uint32_t x, y;
    int osd_en, i;

    {
        int loaded = 0;
        for (i = 0; i < 256; i++) {
            uint32_t raw = io_get(m, R_DISP_GAM_OSD + (uint32_t)i * 4);
            /* The OSD palette RAM on this firmware stores plain 0x00RRGGBB (verified
             * from the live GAM_OSD contents: grays like 0xbbbbbb/0x464646/0x101010
             * have R==G==B, which only holds for RGB -- a YUV gray would be U=V=0x80;
             * and 0x0f7d10/0xcd1b24/0xe9ca2b read as sensible UI green/red/gold). The
             * high byte is an attribute/alpha flag (e.g. 0x01xxxxxx), so mask to 24b.
             * The earlier YUV interpretation turned 0xbbbbbb gray into magenta. */
            pal[i] = raw & 0x00FFFFFFu;
            if (i && (raw & 0x00FFFFFFu)) loaded = 1;
        }
        /* if the firmware hasn't loaded the OSD palette RAM yet, fall back
         * to a visible per-index ramp so drawn content stays legible */
        if (!loaded)
            for (i = 0; i < 256; i++) {
                uint32_t gr = i ? (uint32_t)((i * 40 + 40) & 0xFF) : 0u;
                pal[i] = (gr << 16) | (gr << 8) | gr;
            }
    }

    osd_en = (io_get(m, R_DISP_OSD_SIZE) & DISP_OSD_EN) != 0;

    if (osd_base < 0x40000000u) return -1;
    span = (uint64_t)(h ? h - 1 : 0) * stride + w;
    if ((uint64_t)(osd_base - 0x40000000u) + span > MACH_DRAM_SIZE) return -1;
    fb = machine_dram_ptr(m, osd_base);
    if (!fb) return -1;

    f = fopen(ppm_path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    /* Composite the panel: the decoded photo on the video/main plane (de-tiled
     * straight from DRAM, scaled to the panel) with the OSD plane on top -- OSD
     * index 0 is transparent, so the photo shows through wherever no UI is drawn.
     * The OSD read is CLAMPED to the real OSD region (DS_OSDFRAME_ST .. _END =
     * 0x4005F000 .. 0x40065000); beyond it lies the tiled video buffer, so
     * reading it as OSD indices produced the spurious "green stripes" (index 0 ->
     * BT.601 green). Pixels past the region are treated as transparent. Renders
     * OSD content regardless of the enable bit (firmware draws before flipping
     * enable); enable state is still reported via the return value. */
    {
        const uint32_t osd_end = 0x40065000u;   /* DS_OSDFRAME_END */
        int have_video = (m->jpeg_rgb && m->jpeg_w > 0 && m->jpeg_h > 0);
        /* CT952_OSD_ONLY: render the OSD plane on a black field (transparent
         * index 0 -> black) instead of compositing the photo behind it, so the
         * drawn UI (menu/cursor/icons) is visible in isolation for debugging. */
        int osd_only = getenv("CT952_OSD_ONLY") != NULL;
        if (osd_only) have_video = 0;
        uint32_t vw = m->jpeg_w > 0 ? (uint32_t)m->jpeg_w : 640u;
        uint32_t vh = m->jpeg_h > 0 ? (uint32_t)m->jpeg_h : 360u;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint64_t lin = (uint64_t)y * stride + x;
                int in_osd = osd_en && (osd_base + lin < osd_end);
                uint8_t idx = in_osd ? fb[lin] : 0;
                uint32_t c;
                if (idx == 0 && osd_only) {
                    c = 0;   /* OSD-only debug view: transparent -> black */
                } else if (idx == 0 && have_video) {
                    /* transparent OSD pixel -> the de-tiled video plane */
                    uint32_t vx = (uint32_t)((uint64_t)x * vw / (w ? w : 1));
                    uint32_t vy = (uint32_t)((uint64_t)y * vh / (h ? h : 1));
                    c = video_sample_rgb(m, vx, vy);
                } else {
                    c = pal[idx];
                }
                fputc((int)((c >> 16) & 0xFF), f);
                fputc((int)((c >> 8) & 0xFF), f);
                fputc((int)(c & 0xFF), f);
            }
    }
    fclose(f);
    return osd_en ? 0 : 1;
}

/* ---- DISP video plane scanout (the slideshow photo) ----------------- *
 * The photo-frame slideshow decode lands in the video/main frame buffer as
 * macroblock-tiled YUV 4:2:0 (Y at DS_FRAMEBUF_ST_SLIDESHOW 0x40065000, C at
 * 0x400B3C00, strip 0x2D00) -- the exact bytes the hardware scan-out DAC reads
 * to the panel, and what the firmware's own DSP/JPU write-back produced. This
 * de-tiles that plane straight from DRAM (no host-side shortcut) and emits the
 * RGB the panel shows. Tile geometry matches the MCU-BIU write-back above and
 * videoplane.py (§12.12/§12.19). */
int machine_video_scanout(machine_t *m, uint32_t w, uint32_t h,
                          const char *ppm_path)
{
    FILE *f;
    uint32_t x, y;
    if (!machine_dram_ptr(m, 0x40065000u) || !machine_dram_ptr(m, 0x400B3C00u))
        return -1;
    f = fopen(ppm_path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t c = video_sample_rgb(m, x, y);
            fputc((int)((c >> 16) & 0xFF), f);
            fputc((int)((c >> 8) & 0xFF), f);
            fputc((int)(c & 0xFF), f);
        }
    fclose(f);
    return 0;
}
