/*
 * JupiterSDK on CT952 -- jfb tiled-YUV canvas verification.
 *
 * The swizzle math must be exact: a wrong offset scribbles outside the
 * framebuffer on real hardware. Verified here without hardware:
 *   1. Bijectivity: every (x,y) luma offset within a 704x480 canvas is
 *      in-bounds and unique; same for every chroma quad's U and V.
 *   2. Guard canaries around the buffers stay intact through fill/blit.
 *   3. Read-back equals write for randomized-ish patterns.
 *   4. CRCs of a composed frame -- must match between native
 *      little-endian and qemu-sparc64 big-endian builds.
 */
#include <stdio.h>
#include <string.h>
#include "jfb.h"
#include "jrgb2yuv.h"

#define W 704
#define H 480
/* strip: (704/4) tiles * 64 bytes = 11264 bytes per 16-line block row */
#define STRIP ((W / 4) * 64)
#define YSIZE (STRIP * (H / 16))
/* chroma: 352x240, (352/8) tiles * 256 = 11264 bytes per 16-crow block */
#define CSIZE (STRIP * (240 / 16))

#define GUARD 4096

static uint8_t ybuf[GUARD + YSIZE + GUARD];
static uint8_t cbuf[GUARD + CSIZE + GUARD];
static uint8_t seen_y[YSIZE];
static uint8_t seen_c[CSIZE];

static uint32_t crc32_buf(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int main(void)
{
    jfb_t fb;
    uint32_t x, y, i;
    int fail = 0;

    fb.y_base = ybuf + GUARD;
    fb.c_base = cbuf + GUARD;
    fb.strip = STRIP;
    fb.w = W;
    fb.h = H;

    memset(ybuf, 0xA5, sizeof(ybuf));
    memset(cbuf, 0xA5, sizeof(cbuf));
    memset(seen_y, 0, sizeof(seen_y));
    memset(seen_c, 0, sizeof(seen_c));

    /* ---- 1. bijectivity / bounds ---- */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t off = jfb_y_offset(&fb, x, y);
            if (off >= YSIZE) {
                printf("FAIL: Y offset OOB at %u,%u -> %u\n", x, y, off);
                return 1;
            }
            if (seen_y[off]) {
                printf("FAIL: Y offset collision at %u,%u -> %u\n",
                       x, y, off);
                return 1;
            }
            seen_y[off] = 1;
        }
    for (y = 0; y < H / 2; y++)
        for (x = 0; x < W / 2; x++) {
            uint32_t off = jfb_uv_offset(&fb, x, y);
            if (off + 128 >= CSIZE) {
                printf("FAIL: UV offset OOB at %u,%u -> %u\n", x, y, off);
                return 1;
            }
            if (seen_c[off] || seen_c[off + 128]) {
                printf("FAIL: UV offset collision at %u,%u -> %u\n",
                       x, y, off);
                return 1;
            }
            seen_c[off] = 1;
            seen_c[off + 128] = 1;
        }
    /* every Y byte must be covered exactly once (dense tiling) */
    for (i = 0; i < YSIZE; i++)
        if (!seen_y[i]) {
            printf("FAIL: Y byte %u never addressed\n", i);
            return 1;
        }
    printf("swizzle bijective: Y %u bytes, C %u bytes\n",
           (unsigned)YSIZE, (unsigned)CSIZE);

    /* ---- 2+3. write/read-back ---- */
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            jfb_set_y(&fb, 100 + x, 200 + y, (uint8_t)(x * 3 + y * 5));
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            if (jfb_get_y(&fb, 100 + x, 200 + y) !=
                (uint8_t)(x * 3 + y * 5)) {
                printf("FAIL: Y readback %u,%u\n", x, y);
                return 1;
            }
    for (y = 0; y < 32; y += 2)
        for (x = 0; x < 32; x += 2) {
            uint8_t u, v;
            jfb_set_uv(&fb, 300 + x, 100 + y,
                       (uint8_t)(x + 1), (uint8_t)(y + 7));
            jfb_get_uv(&fb, 300 + x, 100 + y, &u, &v);
            if (u != (uint8_t)(x + 1) || v != (uint8_t)(y + 7)) {
                printf("FAIL: UV readback %u,%u\n", x, y);
                return 1;
            }
        }

    /* ---- 4. composed frame + CRC ---- */
    jfb_fill(&fb, 0, 0, W, H, jup_argb_to_yuv(0xFF204060));
    jfb_fill(&fb, 100, 80, 320, 200, jup_argb_to_yuv(0xFFC04010));
    {
        static uint8_t spr[64 * 48];
        static uint32_t pal[256];
        for (i = 0; i < 256; i++)
            pal[i] = jup_argb_to_yuv(0xFF000000u |
                                     (i << 16) | ((255 - i) << 8) | 0x40);
        for (y = 0; y < 48; y++)
            for (x = 0; x < 64; x++)
                spr[y * 64 + x] = (uint8_t)((x * 4) ^ (y * 5));
        jfb_blit_indexed(&fb, 320, 216, spr, 64, 48, 64, pal);
    }
    for (y = 0; y < H; y += 2)
        jfb_dot(&fb, (y * 3) % (W - 2) & ~1u, y, 0x00EB8080);

    printf("CRC fb_y=%08x fb_c=%08x\n",
           (unsigned)crc32_buf(fb.y_base, YSIZE),
           (unsigned)crc32_buf(fb.c_base, CSIZE));

    /* guard canaries */
    for (i = 0; i < GUARD; i++) {
        if (ybuf[i] != 0xA5 || ybuf[GUARD + YSIZE + i] != 0xA5) {
            printf("FAIL: Y guard corrupted at %u\n", i);
            fail = 1;
            break;
        }
        if (cbuf[i] != 0xA5 || cbuf[GUARD + CSIZE + i] != 0xA5) {
            printf("FAIL: C guard corrupted at %u\n", i);
            fail = 1;
            break;
        }
    }
    if (fail) return 1;

    printf("fb tests OK\n");
    return 0;
}
