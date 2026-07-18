/*
 * emujpeg.c -- decode a baseline JPEG buffer to a packed RGB raster.
 *
 * Thin wrapper over the vendored picojpeg (public-domain baseline decoder,
 * picojpeg.c/.h). The CT952 decodes photos/logos in a hardware DMA/VLD block
 * we don't model gate-for-gate; this provides the functional equivalent so the
 * emulator can turn the JPEG the firmware staged in DRAM into real pixels.
 *
 * picojpeg hands back 8x8 component blocks per MCU; we reassemble them into a
 * top-to-bottom, left-to-right RGB888 raster (w*h*3 bytes, malloc'd).
 */
#include "emujpeg.h"
#include "picojpeg.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *data;
    unsigned int len, pos;
} src_t;

static unsigned char need_bytes(unsigned char *pBuf, unsigned char buf_size,
                                unsigned char *pBytes_read, void *cb)
{
    src_t *s = (src_t *)cb;
    unsigned int n = s->len - s->pos;
    if (n > buf_size) n = buf_size;
    memcpy(pBuf, s->data + s->pos, n);
    s->pos += n;
    *pBytes_read = (unsigned char)n;
    return 0;
}

int emu_jpeg_decode(const unsigned char *jpeg, unsigned int len,
                    unsigned char **out_rgb, int *out_w, int *out_h)
{
    src_t src = { jpeg, len, 0 };
    pjpeg_image_info_t info;
    unsigned char *rgb;
    int w, h, mcu_x = 0, mcu_y = 0;

    if (pjpeg_decode_init(&info, need_bytes, &src, 0) != 0)
        return -1;
    if (info.m_comps != 3 && info.m_comps != 1)
        return -2;

    w = info.m_width;
    h = info.m_height;
    rgb = (unsigned char *)malloc((size_t)w * h * 3);
    if (!rgb) return -3;

    for (;;) {
        unsigned char status = pjpeg_decode_mcu();
        int bx, by;
        if (status) {
            if (status == PJPG_NO_MORE_BLOCKS) break;
            free(rgb);
            return -4;
        }
        /* place this MCU's blocks into the raster */
        for (by = 0; by < info.m_MCUHeight; by += 8) {
            for (bx = 0; bx < info.m_MCUWidth; bx += 8) {
                /* block byte offset within the MCU buffers (picojpeg layout:
                 * 0,64 across; +128 for the second row of a 2x2 MCU) */
                int blk = (bx ? 64 : 0) + (by ? 128 : 0);
                int px = mcu_x * info.m_MCUWidth + bx;
                int py = mcu_y * info.m_MCUHeight + by;
                int row, col;
                for (row = 0; row < 8; row++) {
                    int y = py + row;
                    if (y >= h) break;
                    for (col = 0; col < 8; col++) {
                        int x = px + col;
                        int si = blk + row * 8 + col;
                        unsigned char *d;
                        if (x >= w) break;
                        d = rgb + ((size_t)y * w + x) * 3;
                        d[0] = info.m_pMCUBufR[si];
                        d[1] = info.m_pMCUBufG[si];
                        d[2] = info.m_pMCUBufB[si];
                    }
                }
            }
        }
        if (++mcu_x == info.m_MCUSPerRow) { mcu_x = 0; mcu_y++; }
    }

    *out_rgb = rgb;
    *out_w = w;
    *out_h = h;
    return 0;
}
