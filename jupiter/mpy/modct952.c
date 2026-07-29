/* MicroPython "ct952" module: draw to the CT952 OSD plane from Python.
 *
 * The OSD is an 8bpp palette-indexed plane at DS_OSDFRAME_ST (0x4005F000),
 * 480x240, row stride 480 (verified geometry). The palette is 256 entries
 * of 0x00RRGGBB in the DISP GAM_OSD RAM at 0x80001C00. Enabling the plane
 * and setting its window is a write to REG_DISP_OSD_SIZE (0x80001A54,
 * bit28 = enable, value = (h<<16)|w). ct952emu's machine_disp_scanout
 * composites this to the panel (--fb-out). This is the same register
 * sequence the firmware / the bare-metal disp_test uses -- now driven by
 * Python. */
#include <string.h>
#include "py/runtime.h"
#include "py/obj.h"

/* CONFIRMED-ON-HARDWARE OSD target (see DP700WD_HW_REFERENCE 10.20): the AP
 * loader's 4bpp plane at DS_OSDFRAME_ST_AP, drawn at a line pitch of 292 bytes.
 * The pitch was MEASURED on the panel (a self-identifying probe rendered
 * "288"/"292"/"296" each using itself as the pitch; only 292 was legible) and
 * matches no register: VCR23>>16 reads 308 and the window width implies 360. */
#define OSD_FB        ((volatile uint8_t *)0x40084000u)
#define OSD_FB_ADDR   0x40084000u
#define OSD_PITCH     292u                  /* bytes per display line (measured) */
#define OSD_REGION    24024u                /* AP region size; clamp all writes  */
#define GAM_OSD       ((volatile uint32_t *)0x80001C00u)
#define REG_OSD_POS   (*(volatile uint32_t *)0x80001A50u)
#define REG_OSD_SIZE  (*(volatile uint32_t *)0x80001A54u)
#define DISP_OSD_EN   0x10000000u
#define OSD_W         480                   /* panel's visible width in pixels   */
#define OSD_H         82                    /* 24024/292 lines                    */

/* I/O register access (peripheral space at 0x80000000). */
#define IOREG(off)    (*(volatile uint32_t *)(0x80000000u + (off)))

/* Platform registers needed to survive on REAL silicon (the emulator hides
 * these -- see ctkav_platform.h / hsystem.c):
 *   - REG_PLAT_SYSTEM_CONFIGURATION1 bit28 gates the hardware WATCHDOG. A normal
 *     upgrade-AP flashes and reboots within the watchdog window; our run-AP takes
 *     the CPU bare-metal and never pets it, so the SoC resets ~a fraction of a
 *     second in -> boot loop. We clear the bit to disable the watchdog outright.
 *   - The PROC1 D-cache is copy-back; CPU writes to the OSD framebuffer sit in
 *     cache and never reach the display DMA until flushed. HAL flushes via
 *     REG_PLAT_CACHE_CONTROL (PLAT_PROC1_DCACHE_FLUSH). */
#define REG_PLAT_SYSCFG1      (*(volatile uint32_t *)0x8000031Cu)
#define WDOG_ENABLE_BIT       0x10000000u   /* SYSCFG1[28] = watchdog enable      */
#define REG_PLAT_CACHE_CTRL   (*(volatile uint32_t *)0x80000014u)
#define CACHE_FLUSH_DCACHE    0x00400000u
#define CACHE_DCACHE_PWRSAVE  0x00040000u

/* VOU OSD Read Channel Base Address (ctkav_mcu.h REG_MCU_VCR20/21 @ REG_MCU_BASE
 * 0x80000880 + 0x500/0x504): the DRAM address the DISPLAY DMA fetches the OSD
 * plane from. GDI sets it via __dwOSD_Region_Base when a region is created. The
 * AP loader points it at DS_OSDFRAME_ST_AP (0x40084000) to draw its "Loading"
 * screen; if we draw to a different buffer without repointing this, the panel
 * keeps scanning the loader's buffer and our console is invisible. The emulator
 * takes the scanout address as a parameter, so it never modelled this register. */
#define REG_MCU_VCR20         (*(volatile uint32_t *)0x80000D80u)   /* OSD base (SP1) */
#define REG_MCU_VCR21         (*(volatile uint32_t *)0x80000D84u)   /* OSD base (SP2) */
#define DRAM_BASE             0x40000000u
#define DRAM_TOP              0x40200000u   /* real 2 MB frame */

/* Live OSD framebuffer base -- resolved at console_setup() from REG_MCU_VCR20
 * (where the display is actually scanning) so our text lands on-screen. Defaults
 * to DS_OSDFRAME_ST until then. */
static volatile uint8_t *g_osd_fb = OSD_FB;

/* Live OSD geometry -- resolved at console_setup() from REG_DISP_OSD_SIZE (the
 * value the AP loader programmed for THIS panel). We used to hardcode 480x240
 * (the emulator's geometry); if the real panel's OSD is taller, a 240-row clear
 * leaves the loader's screen showing below it (the "white line 2/3 down"). */
static int g_osd_w = OSD_W, g_osd_h = OSD_H;
static int g_cols  = OSD_W / 8, g_rows = OSD_H / 8;

/* Disable the hardware watchdog (clear SYSCFG1[28]). No PROC1/PROC2 key-lock is
 * needed: our AP owns the CPU and eCos/PROC2 are gone. Called first thing in the
 * app, and exposed as ct952.watchdog_off(). */
void ct952_watchdog_off(void) {
    REG_PLAT_SYSCFG1 &= ~WDOG_ENABLE_BIT;
}

/* Flush the PROC1 D-cache so CPU writes to the OSD plane become visible to the
 * display DMA (replicates PLAT_PROC1_DCACHE_FLUSH: drop power-saving, raise the
 * flush bit, idle >=128 cycles for the 2KB/16B-line cache, restore power-saving). */
static void osd_flush(void) {
    REG_PLAT_CACHE_CTRL &= ~CACHE_DCACHE_PWRSAVE;
    REG_PLAT_CACHE_CTRL |= CACHE_FLUSH_DCACHE;
    for (volatile int i = 0; i < 256; i++) { __asm__ __volatile__("nop"); }
    REG_PLAT_CACHE_CTRL |= CACHE_DCACHE_PWRSAVE;
}

/* GPU 2-D engine (ctkav_gpu.h offsets), shared JPU/GPU block at 0x2880. The
 * emulator's gpu_exec() runs fill-rectangle and 1-bit font expansion into the
 * OSD plane; the JPU path decodes a staged JPEG to the video plane. */
#define R_GPU_CTL0    0x2880
#define R_GPU_CTL1    0x2884
#define R_GPU_COL_NDX 0x2888
#define R_GPU_OP_SIZE 0x288C
#define R_GPU_AG_OFF  0x2890
#define R_GPU_DEST    0x2898
#define R_GPU_FONT_ADR 0x289C
#define R_GPU_FONT_CFG 0x28A0
#define R_GPU_FONT_IDX 0x28A8
#define GPU_OP        0x10000000u   /* CTL0[28]=1: GPU op (0: JPU op)      */
#define GPU_START     0x00000002u   /* CTL0[1]                            */
#define GPU_FONT_1BIT 0x00000020u   /* CTL0[5]: 1-bit font expansion      */
#define GPU_FILLRECT  (6u << 2)     /* CTL0[4:2]=6: fill-rectangle op     */
#define R_JPU_SRC     0x2a20        /* MCU-BIU bitstream source (-> decode) */

/* IR receiver (ctkav_platform.h): IR_DATA[7:0]=scancode, [8]=repeat,
 * [10]=invalid. Fed by ct952emu's CT952_IRKEY / CT952_IRKEYS. */
#define R_IR_DATA     0x0390

/* GPU stride encoding: bytes/row = (ag_width + ag_offset - 1) * 8, with
 * ag_width = (op_width + (dest&3) + 3) >> 2. For the 480-byte OSD pitch we
 * need ag_width + ag_offset - 1 == 60, i.e. ag_offset = 61 - ag_width, which
 * is valid while ag_width <= 61 (op width up to ~244 px). Wider ops are tiled. */
#define GPU_MAX_OPW   240

// init() -- enable the OSD plane and bring up the on-screen text console, so
// print()/REPL output goes to the frame's screen. Forward-declared below.
static void console_setup(void);
static mp_obj_t ct952_init(void) {
    console_setup();          /* palette + clear + enable plane + console on */
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_init_obj, ct952_init);

// palette(index, rgb) -- set palette entry (rgb = 0xRRGGBB).
static mp_obj_t ct952_palette(mp_obj_t index_in, mp_obj_t rgb_in) {
    mp_int_t i = mp_obj_get_int(index_in) & 0xFF;
    mp_int_t rgb = mp_obj_get_int(rgb_in) & 0x00FFFFFF;
    GAM_OSD[i] = (uint32_t)rgb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ct952_palette_obj, ct952_palette);

// pixel(x, y, idx)
static mp_obj_t ct952_pixel(mp_obj_t x_in, mp_obj_t y_in, mp_obj_t idx_in) {
    mp_int_t x = mp_obj_get_int(x_in);
    mp_int_t y = mp_obj_get_int(y_in);
    if (x >= 0 && x < OSD_W && y >= 0 && y < OSD_H) {
        OSD_FB[y * OSD_W + x] = (uint8_t)mp_obj_get_int(idx_in);
        osd_flush();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(ct952_pixel_obj, ct952_pixel);

// fill(idx) -- fill the whole plane.
static mp_obj_t ct952_fill(mp_obj_t idx_in) {
    uint8_t v = (uint8_t)mp_obj_get_int(idx_in);
    for (int i = 0; i < OSD_W * OSD_H; i++) {
        OSD_FB[i] = v;
    }
    osd_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ct952_fill_obj, ct952_fill);

// rect(x, y, w, h, idx) -- filled rectangle, clipped.
static mp_obj_t ct952_rect(size_t n_args, const mp_obj_t *args) {
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    mp_int_t w = mp_obj_get_int(args[2]);
    mp_int_t h = mp_obj_get_int(args[3]);
    uint8_t idx = (uint8_t)mp_obj_get_int(args[4]);
    mp_int_t x0 = x < 0 ? 0 : x;
    mp_int_t y0 = y < 0 ? 0 : y;
    mp_int_t x1 = x + w; if (x1 > OSD_W) x1 = OSD_W;
    mp_int_t y1 = y + h; if (y1 > OSD_H) y1 = OSD_H;
    for (mp_int_t yy = y0; yy < y1; yy++) {
        volatile uint8_t *row = OSD_FB + yy * OSD_W;
        for (mp_int_t xx = x0; xx < x1; xx++) {
            row[xx] = idx;
        }
    }
    osd_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ct952_rect_obj, 5, 5, ct952_rect);

/* ---- GPU 2-D engine: hardware-accelerated fill ------------------------------
 * Program one fill-rectangle op and kick the engine. The emulator's gpu_exec()
 * writes `color` into the 8bpp OSD plane at `dest` with a 480-byte pitch. */
static void gpu_fill_op(uint32_t dest, uint32_t w, uint32_t h, uint8_t color) {
    uint32_t agw = (w + (dest & 3) + 3) >> 2;
    uint32_t ago = (agw <= 61) ? (61 - agw) : 0;   /* -> 480-byte plane pitch */
    IOREG(R_GPU_OP_SIZE) = w | (h << 16);
    IOREG(R_GPU_DEST)    = dest;
    IOREG(R_GPU_AG_OFF)  = ago << 16;
    IOREG(R_GPU_CTL1)    = (uint32_t)color << 24;
    IOREG(R_GPU_CTL0)    = GPU_OP | GPU_START | GPU_FILLRECT;
}

// gpu_fill(x, y, w, h, color) -- fill a rectangle using the real 2-D engine
// (not a CPU loop), clipped to the plane and tiled into <=240px-wide ops.
static mp_obj_t ct952_gpu_fill(size_t n_args, const mp_obj_t *args) {
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    mp_int_t w = mp_obj_get_int(args[2]);
    mp_int_t h = mp_obj_get_int(args[3]);
    uint8_t color = (uint8_t)mp_obj_get_int(args[4]);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > OSD_W) w = OSD_W - x;
    if (y + h > OSD_H) h = OSD_H - y;
    if (w <= 0 || h <= 0) return mp_const_none;
    for (mp_int_t cx = 0; cx < w; cx += GPU_MAX_OPW) {
        mp_int_t cw = w - cx; if (cw > GPU_MAX_OPW) cw = GPU_MAX_OPW;
        gpu_fill_op(OSD_FB_ADDR + (uint32_t)(y * OSD_W + x + cx),
                    (uint32_t)cw, (uint32_t)h, color);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ct952_gpu_fill_obj, 5, 5, ct952_gpu_fill);

/* ---- GPU font engine: hardware 1-bit glyph blit -----------------------------
 * A compact 8x8 font (public-domain font8x8_basic, printable ASCII 0x20..0x7F,
 * rows LSB-first). The emulator's font path expects glyphs MSB-first, one DW
 * (4 bytes) per row with the 8 pixels in byte 0, so we expand the packed font
 * into a DRAM glyph table (32 bytes/glyph) on first use and bit-reverse rows. */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};
static uint8_t g_glyphtab[96 * 32];   /* DRAM (.bss): 8 rows x 4 bytes/glyph */
static int g_glyphtab_ready;

/* ---- on-screen text console (software 8x8 blit into the OSD plane) ----------
 * Routes MicroPython stdout (print output + REPL echo) to the frame's SCREEN, so
 * the REPL is usable with no serial cable -- a real on-device terminal. Cursor +
 * scroll; fg/bg are palette indices set up by console_setup(). uart_core.c's
 * mp_hal_stdout_tx_strn calls ct952_console_write() so every byte also lands here. */
#define CON_COLS (OSD_W / 8)          /* 60 columns */
#define CON_ROWS (OSD_H / 8)          /* 30 rows    */
static int con_col, con_row, con_on;
static uint8_t con_fg = 0x0F, con_bg = 0x00;

/* The firmware's standard OSD plane is 8bpp at DS_OSDFRAME_ST (0x4005F000),
 * stride = width bytes, one byte per pixel = palette index (DP700WD_HW_REFERENCE
 * §8.4/§10.16; _gdi_SetPixel 8B mode). We reconfigure the display to this plane
 * rather than the AP loader's 4bpp band. */
#define OSD_STRIDE   OSD_PITCH                     /* measured line pitch  */
#define BG_FILL_BYTE ((uint8_t)((con_bg << 4) | (con_bg & 0x0F)))  /* 4bpp: 2px/byte */
static void osd_glyph(int cx, int cy, uint8_t ch) {
    if (ch < 0x20 || ch > 0x7F) ch = 0x20;
    const uint8_t *g = font8x8[ch - 0x20];
    for (int row = 0; row < 8; row++) {
        uint32_t off = (uint32_t)(cy * 8 + row) * OSD_STRIDE + (uint32_t)(cx * 8) / 2u;
        volatile uint8_t *p = g_osd_fb + off;
        uint8_t bits = g[row];                    /* LSB = leftmost pixel */
        if (off + 4u > OSD_REGION) return;        /* never leave the region */
        for (int b = 0; b < 4; b++) {             /* 4bpp: 8 px = 4 bytes */
            uint8_t hi = (bits & (1u << (2*b)))     ? con_fg : con_bg;
            uint8_t lo = (bits & (1u << (2*b + 1))) ? con_fg : con_bg;
            p[b] = (uint8_t)((hi << 4) | (lo & 0x0F));
        }
    }
}
static void osd_scroll(void) {
    uint32_t keep = (uint32_t)(g_osd_h - 8) * OSD_STRIDE;
    if (keep + 8u * OSD_STRIDE > OSD_REGION) keep = OSD_REGION - 8u * OSD_STRIDE;
    memmove((void *)g_osd_fb, (void *)(g_osd_fb + 8 * OSD_STRIDE), (size_t)keep);
    memset((void *)(g_osd_fb + keep), BG_FILL_BYTE, 8u * OSD_STRIDE);
}
static void osd_putc(char c) {
    if (c == '\n')      { con_col = 0; con_row++; }
    else if (c == '\r') { con_col = 0; }
    else if (c == '\b') { if (con_col > 0) { con_col--; osd_glyph(con_col, con_row, ' '); } }
    else if (c == '\t') { con_col = (con_col + 4) & ~3; }
    else {
        if (con_col >= g_cols) { con_col = 0; con_row++; }
        if (con_row >= g_rows) { osd_scroll(); con_row = g_rows - 1; }
        osd_glyph(con_col, con_row, (uint8_t)c);
        con_col++;
    }
    if (con_row >= g_rows) { osd_scroll(); con_row = g_rows - 1; }
}
/* external: called from uart_core.c so print()/REPL echo appear on screen */
void ct952_console_write(const char *s, unsigned int len) {
    if (!con_on) return;
    for (unsigned int i = 0; i < len; i++) osd_putc(s[i]);
    osd_flush();   /* push the glyphs from the D-cache to DRAM so the panel updates */
}
/* Generous clear extent (bytes) -- resolved in console_setup so both the clear
 * and cls() wipe any leftover OSD content BELOW the visible area (the loader's
 * "white line"), capped so it never runs into our code at 0x400c0000. */
static uint32_t g_osd_clearbytes = OSD_W * OSD_H;

/* Reconfigure the OSD to the firmware's STANDARD plane and draw there, instead
 * of inheriting the AP loader's 4bpp 616x78 band. All values are source/doc
 * verified (DP700WD_HW_REFERENCE §8.4/10.16): 8bpp plane at DS_OSDFRAME_ST
 * (0x4005F000), stride = width (480), window OSD_SIZE=0x00f002d0 (720x240) at
 * OSD_POS=0x0017006c, palette in BT.601 YUV gated by REG_DISP_BRIGHT_CR bit24,
 * index 0 = transparent color key. */
#define REG_DISP_OSD_POS_R   (*(volatile uint32_t *)0x80001A50u)
#define REG_DISP_BRIGHT_CR   (*(volatile uint32_t *)0x80001A60u)
#define YUV_BLACK            0x00108080u          /* Y=16  U=V=128 */
#define YUV_WHITE            0x00EB8080u          /* Y=235 U=V=128 */

/* palette + clear + enable the plane + turn the console on */
static void console_setup(void) {
    ct952_watchdog_off();                         /* belt-and-suspenders: no reset */

    /* INHERIT the AP loader's display configuration -- do not reprogram it.
     * aploader.c STEP2 has already configured and activated the OSD (4bpp plane
     * at 0x40084000) before jumping to us. Writing VCR20/OSD_POS/OSD_SIZE, or
     * poking palette RAM directly, CLOBBERS that working config on real silicon:
     * proven on hardware, and palette RAM only accepts writes through the DISP
     * blob's REG_VLD_SHO32 handshake anyway (GDI_LoadPalette -> DISP_SetPalette).
     * So: touch no display registers, and use the loader's LIVE palette, whose
     * indices are empirically 0 = transparent key, 1 = yellow, 2 = white. */
    g_osd_w = OSD_W; g_osd_h = OSD_H;              /* 480 x 82, pitch 292 */
    g_cols  = g_osd_w / 8; g_rows = g_osd_h / 8;   /* 60 cols x 10 rows   */
    g_osd_fb = OSD_FB;                             /* 0x40084000          */

    con_bg = 0x00;                                 /* transparent: panel shows through */
    con_fg = 0x02;                                 /* white in the loader's palette    */

    /* Clear exactly the AP region -- never past it. */
    g_osd_clearbytes = OSD_REGION;
    for (uint32_t i = 0; i < g_osd_clearbytes; i++) g_osd_fb[i] = BG_FILL_BYTE;
    con_col = con_row = 0;
    con_on = 1;
    osd_flush();

    /* Self-test banner drawn DIRECTLY (not via MicroPython's print path): if the
     * panel already shows these lines, the console + display are correct and any
     * remaining problem is in the interpreter, not the display. */
    {
        static const char probe[] =
            "CT952A MICROPYTHON - ON-SCREEN CONSOLE\n"
            "pitch 292 4bpp @0x40084000  60x10\n"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\n";
        for (const char *p = probe; *p; p++) osd_putc(*p);
        osd_flush();
    }
}

// cls() -- clear the screen and home the cursor.
static mp_obj_t ct952_cls(void) {
    for (uint32_t i = 0; i < g_osd_clearbytes; i++) g_osd_fb[i] = BG_FILL_BYTE;
    con_col = con_row = 0;
    osd_flush();
    return mp_const_none;
}

// watchdog_off() -- disable the hardware watchdog (exposed for manual use).
static mp_obj_t ct952_watchdog_off_py(void) {
    ct952_watchdog_off();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_watchdog_off_obj, ct952_watchdog_off_py);

// flush() -- flush the D-cache so CPU-drawn pixels reach the panel.
static mp_obj_t ct952_flush_py(void) {
    osd_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_flush_obj, ct952_flush_py);
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_cls_obj, ct952_cls);

static uint8_t bitrev8(uint8_t b) {
    b = (uint8_t)((b >> 4) | (b << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}
static void glyphtab_build(void) {
    memset(g_glyphtab, 0, sizeof g_glyphtab);
    for (int c = 0; c < 96; c++)
        for (int row = 0; row < 8; row++)
            g_glyphtab[c * 32 + row * 4] = bitrev8(font8x8[c][row]);
    g_glyphtab_ready = 1;
}

/* Blit up to `n` glyphs at `dest` via the GPU font engine (one op). */
static void gpu_text_op(uint32_t dest, const char *s, int n, uint8_t fg, uint8_t bg) {
    uint32_t opw = (uint32_t)n * 8u;
    uint32_t agw = (opw + (dest & 3) + 3) >> 2;
    uint32_t ago = (agw <= 61) ? (61 - agw) : 0;
    IOREG(R_GPU_FONT_ADR) = (uint32_t)(uintptr_t)g_glyphtab;
    IOREG(R_GPU_FONT_CFG) = (1u << 24) | 8u;             /* wdw=1 DW, cap=8 DW */
    IOREG(R_GPU_COL_NDX)  = (uint32_t)fg | ((uint32_t)bg << 8);
    IOREG(R_GPU_OP_SIZE)  = opw | (8u << 16);            /* 8px tall */
    IOREG(R_GPU_DEST)     = dest;
    IOREG(R_GPU_AG_OFF)   = ago << 16;
    for (int i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch < 0x20 || ch > 0x7F) ch = 0x20;
        IOREG(R_GPU_FONT_IDX) = (uint32_t)(ch - 0x20);    /* push glyph index */
    }
    IOREG(R_GPU_CTL0) = GPU_OP | GPU_START | GPU_FONT_1BIT;
}

// text(x, y, string, fg, bg) -- draw text with the hardware font engine.
// '\n' starts a new 8px line; long lines are tiled into <=30-glyph ops.
static mp_obj_t ct952_text(size_t n_args, const mp_obj_t *args) {
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    size_t slen;
    const char *s = mp_obj_str_get_data(args[2], &slen);
    uint8_t fg = (uint8_t)mp_obj_get_int(args[3]);
    uint8_t bg = (uint8_t)mp_obj_get_int(args[4]);
    if (!g_glyphtab_ready) glyphtab_build();
    mp_int_t cx = x;
    for (size_t i = 0; i < slen; ) {
        if (s[i] == '\n') { y += 8; cx = x; i++; continue; }
        size_t run = 0;
        while (i + run < slen && s[i + run] != '\n' && run < 30) run++;
        if (y >= 0 && y + 8 <= OSD_H && cx < OSD_W)
            gpu_text_op(OSD_FB_ADDR + (uint32_t)(y * OSD_W + cx), s + i, (int)run, fg, bg);
        cx += (mp_int_t)run * 8;
        i += run;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ct952_text_obj, 5, 5, ct952_text);

// decode_jpeg(addr) -- point the JPU at a JPEG staged in DRAM and kick the
// hardware decoder; the reconstructed frame lands on the video plane, which
// the display composites under the OSD. `addr` must be a DRAM address.
static mp_obj_t ct952_decode_jpeg(mp_obj_t addr_in) {
    uint32_t addr = (uint32_t)mp_obj_get_int_truncated(addr_in);
    IOREG(R_JPU_SRC) = addr;      /* MCU-BIU bitstream source -> jpeg_src */
    IOREG(R_GPU_CTL0) = 0;        /* JPU op (CTL0[28]=0): run the decode   */
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ct952_decode_jpeg_obj, ct952_decode_jpeg);

// ir_poll() -- read the IR remote receiver; returns the scancode of a pending
// key (and consumes it), or None. Feed keys with ct952emu CT952_IRKEY(S).
static mp_obj_t ct952_ir_poll(void) {
    uint32_t v = IOREG(R_IR_DATA);
    if (v & (1u << 10)) return mp_const_none;   /* invalid-data flag */
    uint32_t code = v & 0xFF;
    if (code == 0) return mp_const_none;
    IOREG(R_IR_DATA) = 0;                        /* consume */
    return MP_OBJ_NEW_SMALL_INT(code);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_ir_poll_obj, ct952_ir_poll);

// peek32(addr) / poke32(addr, val) -- raw 32-bit access to any modeled
// register or memory. The whole SoC is reachable from the REPL.
static mp_obj_t ct952_peek32(mp_obj_t addr_in) {
    // Truncated accessor: addresses (and register values) >= 2^31 -- e.g. the
    // I/O regions 0x80000000/0xa0000000/0x98000000 -- don't fit a signed
    // mp_int_t; mp_obj_get_int_truncated takes the raw 32-bit bit pattern.
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(uint32_t)mp_obj_get_int_truncated(addr_in);
    return mp_obj_new_int_from_uint(*p);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ct952_peek32_obj, ct952_peek32);

static mp_obj_t ct952_poke32(mp_obj_t addr_in, mp_obj_t val_in) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(uint32_t)mp_obj_get_int_truncated(addr_in);
    *p = (uint32_t)mp_obj_get_int_truncated(val_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ct952_poke32_obj, ct952_poke32);

// poke_bytes(addr, data) -- copy a bytes-like object into memory (e.g. stage a
// JPEG in DRAM before decode_jpeg).
static mp_obj_t ct952_poke_bytes(mp_obj_t addr_in, mp_obj_t data_in) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data_in, &bi, MP_BUFFER_READ);
    memcpy((void *)(uintptr_t)(uint32_t)mp_obj_get_int_truncated(addr_in), bi.buf, bi.len);
    return MP_OBJ_NEW_SMALL_INT(bi.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ct952_poke_bytes_obj, ct952_poke_bytes);

// call(addr, a0=0, a1=0, a2=0, a3=0) -- call a firmware function at `addr` with
// up to four integer arguments (SPARC %o0..%o3) and return its result (%o0).
// Only meaningful in the embedded "app" build, where Python runs inside the
// live firmware and shares its address space + calling convention.
static mp_obj_t ct952_call(size_t n_args, const mp_obj_t *args) {
    uint32_t addr = (uint32_t)mp_obj_get_int_truncated(args[0]);
    mp_int_t a[4] = {0, 0, 0, 0};
    for (size_t i = 1; i < n_args && i <= 4; i++) a[i - 1] = mp_obj_get_int_truncated(args[i]);
    typedef mp_int_t (*fw_fn_t)(mp_int_t, mp_int_t, mp_int_t, mp_int_t);
    fw_fn_t f = (fw_fn_t)(uintptr_t)addr;
    mp_int_t r = f(a[0], a[1], a[2], a[3]);
    return mp_obj_new_int(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ct952_call_obj, 1, 5, ct952_call);

// resume() -- for a PROC1-seize app: hand control back to the firmware, which
// continues from exactly where it was seized (with whatever peek/poke/call
// mutations Python made to the frozen state now in effect). The app does not
// run past this point.
static mp_obj_t ct952_resume(void) {
    *(volatile uint32_t *)0x80007FE8u = 0x0C0FFEE0u;
    for (;;) { }   // spin until the emulator restores the firmware context
    return mp_const_none;   // unreachable; satisfies -Werror=return-type
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_resume_obj, ct952_resume);

/* ---- SD host controller (SDHC spec, base 0xA0001100) ------------------------
 * A minimal bare-metal SD driver: the emulator presents a FAT card image
 * (CT952_SDCARD) as an inserted SDHC card and serves CMD18 block reads by DMA.
 * We run the standard init handshake (CMD0/8/ACMD41/CMD2/3/7) then read blocks
 * into a DRAM buffer the controller DMAs into. */
#define REG32(a)      (*(volatile uint32_t *)(uintptr_t)(uint32_t)(a))
#define SDC_BASE      0xA0001100u
#define SDC_DMA       (SDC_BASE + 0x00)   /* DMA target address              */
#define SDC_BLK       (SDC_BASE + 0x04)   /* [31:16]=block size, [15:0]=count */
#define SDC_ARG       (SDC_BASE + 0x08)   /* command argument                */
#define SDC_CMD       (SDC_BASE + 0x0C)   /* [31:16]=tran mode, [15:8]=index  */
#define SDC_RESP0     (SDC_BASE + 0x10)
#define SDC_STAT      (SDC_BASE + 0x24)   /* bit16=card inserted             */
#define SDC_INT       (SDC_BASE + 0x30)   /* W1C; bit16=cmd, bit17=tran done  */
#define SDC_CARD_INS  (1u << 16)
#define SDC_TRAN_DONE (1u << 17)
#define SD_MAXBLK     64                  /* per sd_read() call (32 KB)       */

static uint8_t g_sdbuf[SD_MAXBLK * 512] __attribute__((aligned(4)));  /* DRAM DMA */
static uint8_t g_sd_ready;

/* Issue one SD command; returns R1/R6 (RESP0). The model completes commands
 * synchronously, but we clear the completion latch for a faithful handshake. */
static uint32_t sd_cmd(uint32_t idx, uint32_t arg, uint32_t tranmode) {
    REG32(SDC_INT) = 0xFFFFFFFFu;                 /* clear stale status */
    REG32(SDC_ARG) = arg;
    REG32(SDC_CMD) = (tranmode << 16) | (idx << 8);   /* word write issues it */
    return REG32(SDC_RESP0);
}

// sd_present() -- True if a card is inserted.
static mp_obj_t ct952_sd_present(void) {
    return (REG32(SDC_STAT) & SDC_CARD_INS) ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_sd_present_obj, ct952_sd_present);

// sd_init() -- run the SDHC init handshake; returns True on success.
static mp_obj_t ct952_sd_init(void) {
    g_sd_ready = 0;
    if (!(REG32(SDC_STAT) & SDC_CARD_INS)) return mp_const_false;
    sd_cmd(0, 0, 0);                     /* GO_IDLE_STATE            */
    sd_cmd(8, 0x1AA, 0);                 /* SEND_IF_COND             */
    for (int i = 0; i < 100; i++) {      /* ACMD41: wait card ready  */
        sd_cmd(55, 0, 0);
        if (sd_cmd(41, 0x40FF8000u, 0) & 0x80000000u) break;  /* busy bit -> ready */
    }
    sd_cmd(2, 0, 0);                     /* ALL_SEND_CID             */
    uint32_t rca = sd_cmd(3, 0, 0) >> 16;/* SEND_RELATIVE_ADDR       */
    sd_cmd(7, rca << 16, 0);             /* SELECT_CARD              */
    g_sd_ready = 1;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ct952_sd_init_obj, ct952_sd_init);

// sd_read(lba, count) -- read `count` 512-byte blocks from block `lba` via the
// controller's DMA engine; returns the data as bytes.
static mp_obj_t ct952_sd_read(mp_obj_t lba_in, mp_obj_t count_in) {
    if (!g_sd_ready) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("sd not initialised"));
    uint32_t lba = (uint32_t)mp_obj_get_int(lba_in);
    mp_int_t count = mp_obj_get_int(count_in);
    if (count < 1) count = 1;
    if (count > SD_MAXBLK) count = SD_MAXBLK;
    REG32(SDC_DMA) = (uint32_t)(uintptr_t)g_sdbuf;
    REG32(SDC_BLK) = (512u << 16) | (uint32_t)count;
    sd_cmd(18, lba, 0x0032);             /* READ_MULTIPLE_BLOCK (DMA read) */
    for (volatile int i = 0; i < 100000 && !(REG32(SDC_INT) & SDC_TRAN_DONE); i++) { }
    REG32(SDC_INT) = SDC_TRAN_DONE | (1u << 16);
    return mp_obj_new_bytes(g_sdbuf, (size_t)count * 512u);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ct952_sd_read_obj, ct952_sd_read);

// panel_adc(channel=0x84) -- select an analog key-ladder line and read the ADC
// voltage (0..255). Feed keypresses with ct952emu CT952_PANELKEY / CT952_ADC.
static mp_obj_t ct952_panel_adc(size_t n_args, const mp_obj_t *args) {
    uint32_t chan = n_args >= 1 ? (uint32_t)mp_obj_get_int(args[0]) : 0x84u;
    volatile uint32_t *adc = (volatile uint32_t *)0x8000407Cu;
    *adc = (*adc & ~0x00FF0000u) | ((chan & 0xFF) << 16);   /* select ladder line */
    return MP_OBJ_NEW_SMALL_INT((*adc >> 24) & 0xFF);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ct952_panel_adc_obj, 0, 1, ct952_panel_adc);

static const mp_rom_map_elem_t ct952_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ct952) },
    { MP_ROM_QSTR(MP_QSTR_WIDTH), MP_ROM_INT(OSD_W) },
    { MP_ROM_QSTR(MP_QSTR_HEIGHT), MP_ROM_INT(OSD_H) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&ct952_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_cls), MP_ROM_PTR(&ct952_cls_obj) },
    { MP_ROM_QSTR(MP_QSTR_watchdog_off), MP_ROM_PTR(&ct952_watchdog_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&ct952_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&ct952_palette_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&ct952_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&ct952_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&ct952_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpu_fill), MP_ROM_PTR(&ct952_gpu_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&ct952_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_decode_jpeg), MP_ROM_PTR(&ct952_decode_jpeg_obj) },
    { MP_ROM_QSTR(MP_QSTR_ir_poll), MP_ROM_PTR(&ct952_ir_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_peek32), MP_ROM_PTR(&ct952_peek32_obj) },
    { MP_ROM_QSTR(MP_QSTR_poke32), MP_ROM_PTR(&ct952_poke32_obj) },
    { MP_ROM_QSTR(MP_QSTR_poke_bytes), MP_ROM_PTR(&ct952_poke_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_call), MP_ROM_PTR(&ct952_call_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&ct952_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_sd_present), MP_ROM_PTR(&ct952_sd_present_obj) },
    { MP_ROM_QSTR(MP_QSTR_sd_init), MP_ROM_PTR(&ct952_sd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_sd_read), MP_ROM_PTR(&ct952_sd_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_panel_adc), MP_ROM_PTR(&ct952_panel_adc_obj) },
};
static MP_DEFINE_CONST_DICT(ct952_module_globals, ct952_module_globals_table);

const mp_obj_module_t ct952_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ct952_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ct952, ct952_user_cmodule);
