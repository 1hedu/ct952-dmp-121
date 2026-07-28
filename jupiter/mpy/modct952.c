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
#include "py/runtime.h"
#include "py/obj.h"

#define OSD_FB        ((volatile uint8_t *)0x4005F000u)
#define GAM_OSD       ((volatile uint32_t *)0x80001C00u)
#define REG_OSD_POS   (*(volatile uint32_t *)0x80001A50u)
#define REG_OSD_SIZE  (*(volatile uint32_t *)0x80001A54u)
#define DISP_OSD_EN   0x10000000u
#define OSD_W         480
#define OSD_H         240

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

static const mp_rom_map_elem_t ct952_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ct952) },
    { MP_ROM_QSTR(MP_QSTR_WIDTH), MP_ROM_INT(OSD_W) },
    { MP_ROM_QSTR(MP_QSTR_HEIGHT), MP_ROM_INT(OSD_H) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&ct952_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&ct952_palette_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&ct952_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&ct952_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&ct952_rect_obj) },
};
static MP_DEFINE_CONST_DICT(ct952_module_globals, ct952_module_globals_table);

const mp_obj_module_t ct952_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ct952_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ct952, ct952_user_cmodule);
