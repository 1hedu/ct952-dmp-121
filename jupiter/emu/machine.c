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
    if (m->jpeg_src < 0x40000000u ||
        m->jpeg_src >= 0x40000000u + MACH_DRAM_SIZE)
        return;
    src = m->dram + (m->jpeg_src - 0x40000000u);
    avail = (0x40000000u + MACH_DRAM_SIZE) - m->jpeg_src;
    if (src[0] != 0xFF || src[1] != 0xD8)   /* need a JPEG SOI staged */
        return;
    sig = jpeg_stage_sig(src, avail);
    if (sig == m->jpeg_sig)           /* same frame as last time -> done */
        return;
    if (emu_jpeg_decode(src, avail, &rgb, &w, &h) != 0)
        return;
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
    /* keep the raster for the scan-out video-plane composite */
    free(m->jpeg_rgb);
    m->jpeg_rgb = rgb;
    m->jpeg_w = w;
    m->jpeg_h = h;

    /* P1 (CT952_LOGODECODE): write the decoded frame into the firmware's video
     * frame buffer as macroblock-tiled YUV 4:2:0 (§10.2/§10.10), so the hardware
     * decode the driver kicked "produces" real pixels -- what §10.33 showed is
     * missing (Y buffer 0x40065000 was all-zero). Y at DS_FRAMEBUF_ST_SLIDESHOW
     * (0x40065000), C at +0x4EC00 (0x400B3C00); strip=0x2D00 (720-wide buffer). */
    if (getenv("CT952_LOGODECODE")) {
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
            fprintf(stderr, "[LOGODECODE] wrote %dx%d tiled YUV to 0x%08x/0x%08x\n",
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
    case 0xc10:
        /* Decoder progress/state word. The firmware's wait loop (flash
         * 0x72810) polls bits[20:16] for >=7. This is driven by the hardware
         * JPEG decoder consuming the staged bitstream; with the functional
         * decode armed, run it here (once) and then report >=7 (done). */
        if (m->jpeg_decode_en) {
            machine_maybe_jpeg_decode(m);
            return (io_get(m, 0xc10) & ~0x001f0000u) | 0x00070000u;
        }
        log_access(m, 0x80000c10u, 0, 0);
        return io_get(m, 0xc10);
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
             * to JPEG_STATUS_OK. Set after the boot stop gates (JPU decode runs
             * only once the firmware is decoding), so gate-3's 0x11 is unaffected.
             * Effective only under CT952_VDEC_DONE (mirror read gate) (10.40). */
            m->vdec_frame_done = 1;
            /* P1: on the JPU decode kick, functionally decode the staged JPEG
             * and emit the tiled-YUV frame the real hardware would produce
             * (CT952_LOGODECODE). Guarded by jpeg_sig so it decodes once/frame. */
            if (getenv("CT952_LOGODECODE")) machine_maybe_jpeg_decode(m);
        }
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
        io_set(m, R_P1_1ST_MASK, io_get(m, R_P1_1ST_MASK) & ~v);
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

static uint32_t bus_rd(machine_t *m, uint32_t addr, int size, int *fault)
{
    *fault = 0;

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
        /* Decoder-STOP window: present the state mirror (0x40039cd0, read by
         * getter 0x6f054) as MODE_STOP(0x10) so the boot stop-poll latches it
         * before the state settles to STOPPED(0x11). See machine.h. */
        if (addr == 0x40039cd0u) {
            if (getenv("CT952_MIRTRACE")) {
                uint32_t rv = (m->cycles < m->vdec_stop_until) ? 0x10u :
                    (m->vdec_frame_done && getenv("CT952_VDEC_DONE")) ? 0x10u :
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
             * Opt-in via CT952_VDEC_DONE for clean A/B (10.40). */
            if (m->vdec_frame_done && getenv("CT952_VDEC_DONE")) return 0x10u;
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
        /* No-media model (CT952_NOMEDIA): stand in for the USBSRC worker
         * thread, which never runs in the emulator (it would block in the
         * opaque usb.a/card.a HW init). The firmware's media-detect loop
         * (_MEDIA_MonitorMediaStatus, media.c:1489) gates on the USB source
         * thread having initialised: USBSRC_TriggerCmd requires
         * __fThreadInit & INIT_SRC_THREAD_USB_DONE (0x80000). Present that bit
         * as set on every read of __fThreadInit (0x4003e590) so the trigger
         * path runs; the CHECK_DEVICE handshake completion is modelled on the
         * write side. See DP700WD_HW_REFERENCE.md 10.17. */
        if (m->nomedia && addr == 0x4003e590u && size == 4)
            return mem_read_raw(m->dram + (addr - 0x40000000u), 4) | 0x00080000u;
        return mem_read_raw(m->dram + (addr - 0x40000000u), size);
    }
    if (addr >= 0xC0000000u && addr + (uint32_t)size <= 0xC0000000u + MACH_DRAM_SIZE)
        return mem_read_raw(m->dram + (addr - 0xC0000000u), size);
    if (addr >= 0x80000000u && addr < 0x80000000u + MACH_IO_SIZE) {
        uint32_t off = (addr - 0x80000000u) & ~3u;
        uint32_t v = io_read(m, off);
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
        if (size == 4) return v;
        /* sub-word I/O read: extract big-endian lane */
        if (size == 1) return (v >> ((3 - (addr & 3)) * 8)) & 0xFF;
        return (v >> ((addr & 2) ? 0 : 16)) & 0xFFFF;
    }
    if (addr >= 0xB0000000u && addr + (uint32_t)size <= 0xB0010000u) {
        /* EXPERIMENT (CT952_FORCE_PLAYMODE): present the vdec playmode
         * (0xb0000190) as a fixed value, standing in for the PROC2 decoder
         * microcode reaching MODE_STOP(0x10). Tests whether the boot thread's
         * decoder-state poll (flash 0x375a0/0x6f054) is the menu-draw gate. */
        if (addr == 0xB0000190u) {
            /* During the STOP window, present the live reg as STOPPED(0x11) so
             * the getter (0x6f054) adds no busy bit and returns the mirror's
             * MODE_STOP(0x10) cleanly. */
            if (m->cycles < m->vdec_stop_until) return 0x11u;
            static int fp = -1;
            if (fp < 0) { const char *e = getenv("CT952_FORCE_PLAYMODE");
                          fp = e ? (int)strtoul(e, NULL, 0) : -2; }
            if (fp >= 0) { m->bram[0x190] = (uint8_t)fp; }
        }
        return mem_read_raw(m->bram + (addr - 0xB0000000u), size);
    }
    if (addr >= 0x90000000u && addr < 0x90010000u)
        return 0;                        /* DSU stub */
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
        /* No-media model (CT952_NOMEDIA): emulate the USBSRC worker completing
         * a CHECK_DEVICE command with a "no removable media" verdict. The
         * firmware sets bit CHECK_DEVICE(0x1) in _fUSBSRCCmdd (0x4003f540) via
         * OS_SetFlag; the real worker thread would then run USB_CheckConnect,
         * set _bUSBSRCState, post the status flag and clear the command. We do
         * that here so the media-detect loop sees SRCFTR_USB_STATE_NO_MEDIA and
         * falls through to POWERONMENU_Initial / MM_PlayPhotoInFlash. */
        if (m->nomedia && addr == 0x4003f540u &&
            (mem_read_raw(m->dram + 0x3f540u, 4) & 0x1u)) {
            mem_write_raw(m->dram + 0x3f510u, 1, 1);       /* _bUSBSRCState = NO_MEDIA */
            mem_write_raw(m->dram + 0x3f514u,              /* _fUSBSRCCmddStatus |= CHECK_DEVICE */
                          mem_read_raw(m->dram + 0x3f514u, 4) | 0x1u, 4);
            mem_write_raw(m->dram + 0x3f540u,              /* _fUSBSRCCmdd &= ~CHECK_DEVICE */
                          mem_read_raw(m->dram + 0x3f540u, 4) & ~0x1u, 4);
            mem_write_raw(m->dram + 0x3f524u,              /* _fUSBSRCCmddRunning &= ~CHECK_DEVICE */
                          mem_read_raw(m->dram + 0x3f524u, 4) & ~0x1u, 4);
        }
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
            (off == 0x1a48u || off == 0x1a4cu || off == 0x1ac0u || off == 0x1ac4u ||
             (getenv("CT952_LOGOTRACE_JPU") && (off == 0x2880u || off == 0x2a20u)))) {
            static int lt; if (lt < 500) {
                fprintf(stderr, "[LOGOwr] %08x=%08x pc=%08x sp=%08x icount=%llu\n",
                        addr, val, m->cpu.pc, sparc_get_reg(&m->cpu, 14),
                        (unsigned long long)m->cpu.icount); lt++; }
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
                           uint32_t ctl_off, uint32_t irq_bit)
{
    uint32_t ctl = io_get(m, ctl_off);
    uint32_t cnt;
    if (!(ctl & TIMER_ENABLE)) return;
    cnt = io_get(m, cnt_off);
    if (cnt == 0) {
        if (ctl & TIMER_RELOAD)
            io_set(m, cnt_off, io_get(m, rld_off));
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
     * tunable divider so many fields elapse within a bring-up run. */
    if (++m->vsync_cnt >= m->vsync_div) {
        m->vsync_cnt = 0;
        io_set(m, R_P1_1ST_PEND,
               io_get(m, R_P1_1ST_PEND) | IRQ_P1_1ST_VSYNC);
    }
    if (m->presc_cnt == 0) {
        m->presc_cnt = io_get(m, R_PRESC_RLD);
        m->t3_value++;
        timer_tick_one(m, R_TIMER1_CNT, R_TIMER1_RLD, R_TIMER1_CTL,
                       0x100u);   /* INT_TIMER1 */
        timer_tick_one(m, R_TIMER2_CNT, R_TIMER2_RLD, R_TIMER2_CTL,
                       0x200u);   /* INT_TIMER2 */
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
    m->nomedia = getenv("CT952_NOMEDIA") ? 1 : 0;
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

uint64_t machine_run(machine_t *m, uint64_t n)
{
    uint64_t done = 0;
    if (g_pcsamp_on < 0) {
        const char *e = getenv("CT952_PCSAMP");
        g_pcsamp_on = e ? 1 : 0;
        if (e) { g_pcsamp_thresh = strtoull(e, NULL, 0); atexit(pcsamp_dump); }
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
    while (done < n && !m->cpu.halted && !m->watchdog_fired) {
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
                {0x00061170u,"UTL_ShowJPEG_Slide(F)"}, {0x000118b8u,"_MEDIA_MonitorMediaStatus(F)"},
            };
            static uint8_t hit[14];
            int wi;
            for (wi = 0; wi < 14; wi++)
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

/* BT.601 studio-range YCbCr (0x00YYUUVV) -> packed 0x00RRGGBB: the exact
 * inverse of the SDK's jup_argb_to_yuv, so a colour loaded into the OSD
 * palette scans back out to its original ARGB. */
static uint32_t disp_yuv_to_rgb(uint32_t yuv)
{
    int y = (int)((yuv >> 16) & 0xFF);
    int u = (int)((yuv >> 8) & 0xFF);
    int v = (int)(yuv & 0xFF);
    int c = y - 16, d = u - 128, e = v - 128;
    int r = clamp8((298 * c + 409 * e + 128) >> 8);
    int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    int b = clamp8((298 * c + 516 * d + 128) >> 8);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
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
            pal[i] = disp_yuv_to_rgb(raw);
            /* Test the RAW palette word, not the converted RGB: an all-zero
             * GAM_OSD entry converts to (0,135,0) green (BT.601 Y=U=V=0), which
             * is nonzero and would falsely read as "palette loaded". */
            if (i && raw) loaded = 1;
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
    /* Composite the panel: the decoded photo on the video/main plane (scaled
     * to the panel), with the OSD plane on top -- OSD index 0 is transparent,
     * so the photo shows through wherever no UI is drawn. Renders the OSD
     * content regardless of the hardware enable bit (the firmware draws before
     * flipping enable); enable state is still reported via the return value. */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t idx = fb[(uint64_t)y * stride + x];
            uint32_t c;
            if (idx == 0 && m->jpeg_rgb && m->jpeg_w > 0 && m->jpeg_h > 0) {
                /* transparent OSD pixel -> sample the video plane */
                int vx = (int)((uint64_t)x * m->jpeg_w / (w ? w : 1));
                int vy = (int)((uint64_t)y * m->jpeg_h / (h ? h : 1));
                const uint8_t *p = m->jpeg_rgb +
                                   ((size_t)vy * m->jpeg_w + vx) * 3;
                c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            } else {
                c = pal[idx];
            }
            fputc((int)((c >> 16) & 0xFF), f);
            fputc((int)((c >> 8) & 0xFF), f);
            fputc((int)(c & 0xFF), f);
        }
    fclose(f);
    return osd_en ? 0 : 1;
}
