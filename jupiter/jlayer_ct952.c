/*
 * JupiterSDK on CT952 -- hardware layer manager implementation.
 *
 * SP planes via the firmware's own subpicture-bitmap recipe
 * (char_subpict.c:_CHAR_SP_InitSPU): SPU_BMP_Init once, then per
 * plane SetDisplayArea + 4x SetColorContrast + SetDisplay. OSD mix
 * via GDI_SetMixRatio.
 *
 * Default bitmap buffers (idle while the Jupiter app owns the
 * screen):
 *   SP1: DS_SP_OSD_ST     (the 2bpp SP-OSD region, 3K DW = 12 KB)
 *   SP2: DS_SP1BUF_ADDR_ST (subpicture bitstream buffer; no disc
 *        subpicture stream runs in app mode)
 */
#include "Winav.h"
#include "gdi.h"
#include "subpict.h"

#include "jlayer.h"
#include "jrgb2yuv.h"

static BYTE _bSpuInited = 0;
static BYTE _bSpOpen[2] = {0, 0};
static jlayer_sp_t _SpState[2];

uint8_t *jlayer_sp_buffer(uint8_t sp, uint32_t *size)
{
    if (sp == JLAYER_SP1) {
        if (size) *size = (uint32_t)(DS_SP_OSD_END - DS_SP_OSD_ST);
        return (uint8_t *)DS_SP_OSD_ST;
    }
    if (size) *size = (uint32_t)(DS_SP1BUF_ADDR_END - DS_SP1BUF_ADDR_ST);
    return (uint8_t *)DS_SP1BUF_ADDR_ST;
}

int jlayer_sp_open(jlayer_sp_t *layer, uint8_t sp,
                   uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint8_t *bmp)
{
    if (!layer || sp > 1 || bmp == NULL || w == 0 || h == 0 ||
        (w & 3) != 0)
        return -1;

    if (!_bSpuInited) {
        SPU_BMP_Init();
        _bSpuInited = 1;
    }

    layer->bmp = bmp;
    layer->pitch = (uint32_t)w / 4;
    layer->w = w;
    layer->h = h;

    SPU_BMP_SetDisplayArea(sp, x, y, w, h, (const BYTE *)bmp);
    _SpState[sp] = *layer;
    _bSpOpen[sp] = 1;
    return 0;
}

void jlayer_sp_move(uint8_t sp, uint16_t x, uint16_t y)
{
    if (sp > 1 || !_bSpOpen[sp])
        return;
    SPU_BMP_SetDisplayArea(sp, x, y,
                           (WORD)_SpState[sp].w, (WORD)_SpState[sp].h,
                           (const BYTE *)_SpState[sp].bmp);
}

void jlayer_sp_color(uint8_t sp, uint8_t idx, uint32_t argb,
                     uint8_t alpha_0_15)
{
    if (sp > 1 || idx > 3)
        return;
    SPU_BMP_SetColorContrast(sp, idx,
                             (DWORD)jup_argb_to_yuv(argb),
                             (BYTE)(alpha_0_15 & 15));
}

void jlayer_sp_show(uint8_t sp, int on)
{
    if (sp > 1 || !_bSpOpen[sp])
        return;
    SPU_BMP_SetDisplay(sp, on ? TRUE : FALSE);
}

void jlayer_sp_close(uint8_t sp)
{
    if (sp > 1 || !_bSpOpen[sp])
        return;
    SPU_BMP_SetDisplay(sp, FALSE);
    _bSpOpen[sp] = 0;
}

void jlayer_osd_mix(uint8_t ratio_0_63)
{
    GDI_SetMixRatio((BYTE)(ratio_0_63 & 63));
}
