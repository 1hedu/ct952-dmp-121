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

#define OSD_FB        ((volatile uint8_t *)0x4005F000u)
#define OSD_FB_ADDR   0x4005F000u
#define GAM_OSD       ((volatile uint32_t *)0x80001C00u)
#define REG_OSD_POS   (*(volatile uint32_t *)0x80001A50u)
#define REG_OSD_SIZE  (*(volatile uint32_t *)0x80001A54u)
#define DISP_OSD_EN   0x10000000u
#define OSD_W         480
#define OSD_H         240

/* I/O register access (peripheral space at 0x80000000). */
#define IOREG(off)    (*(volatile uint32_t *)(0x80000000u + (off)))

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

// init() -- clear the framebuffer, set the window, and enable the plane.
static mp_obj_t ct952_init(void) {
    for (int i = 0; i < OSD_W * OSD_H; i++) {
        OSD_FB[i] = 0;
    }
    REG_OSD_POS = 0;
    REG_OSD_SIZE = DISP_OSD_EN | ((uint32_t)OSD_H << 16) | (uint32_t)OSD_W;
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
    uint32_t addr = (uint32_t)mp_obj_get_int(addr_in);
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
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(uint32_t)mp_obj_get_int(addr_in);
    return mp_obj_new_int_from_uint(*p);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ct952_peek32_obj, ct952_peek32);

static mp_obj_t ct952_poke32(mp_obj_t addr_in, mp_obj_t val_in) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(uint32_t)mp_obj_get_int(addr_in);
    *p = (uint32_t)mp_obj_get_int(val_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ct952_poke32_obj, ct952_poke32);

// poke_bytes(addr, data) -- copy a bytes-like object into memory (e.g. stage a
// JPEG in DRAM before decode_jpeg).
static mp_obj_t ct952_poke_bytes(mp_obj_t addr_in, mp_obj_t data_in) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data_in, &bi, MP_BUFFER_READ);
    memcpy((void *)(uintptr_t)(uint32_t)mp_obj_get_int(addr_in), bi.buf, bi.len);
    return MP_OBJ_NEW_SMALL_INT(bi.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ct952_poke_bytes_obj, ct952_poke_bytes);

// call(addr, a0=0, a1=0, a2=0, a3=0) -- call a firmware function at `addr` with
// up to four integer arguments (SPARC %o0..%o3) and return its result (%o0).
// Only meaningful in the embedded "app" build, where Python runs inside the
// live firmware and shares its address space + calling convention.
static mp_obj_t ct952_call(size_t n_args, const mp_obj_t *args) {
    uint32_t addr = (uint32_t)mp_obj_get_int(args[0]);
    mp_int_t a[4] = {0, 0, 0, 0};
    for (size_t i = 1; i < n_args && i <= 4; i++) a[i - 1] = mp_obj_get_int(args[i]);
    typedef mp_int_t (*fw_fn_t)(mp_int_t, mp_int_t, mp_int_t, mp_int_t);
    fw_fn_t f = (fw_fn_t)(uintptr_t)addr;
    mp_int_t r = f(a[0], a[1], a[2], a[3]);
    return mp_obj_new_int(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ct952_call_obj, 1, 5, ct952_call);

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
