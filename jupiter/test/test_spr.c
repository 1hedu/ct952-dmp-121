/*
 * JupiterSDK on CT952 -- sprite engine (jspr) verification.
 *
 * jspr's clipping and mirror-aware source adjustment feed the (already
 * model-verified) jgpu builders; here the executor is the software
 * model, and every result is compared byte-exactly against a plain CPU
 * reference blitter that implements the intended semantics directly.
 * Sweeps sprites across all four edges (mirrored and not) so every
 * clip branch is exercised. Endian-stable CRC as always.
 */
#include <stdio.h>
#include <string.h>
#include "jspr.h"

#define PITCH 616
#define H     440
#define AW    128
#define AH    64
#define BASE_D 0x10000u
#define BASE_A 0x90000u

static uint8_t dmem[PITCH * H];
static uint8_t ref[PITCH * H];
static uint8_t amem[AW * AH];

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

/* Model-backed executor mapping the fake hardware addresses */
static void exec_model(const jgpu_op_t *op)
{
    jgpu_model_exec(op, dmem, BASE_D, amem, BASE_A);
}

/* CPU reference with clipping done the obvious way */
static void ref_sprite(int32_t dx, int32_t dy, uint32_t sx, uint32_t sy,
                       uint32_t w, uint32_t h, uint8_t key,
                       int keyed, int mirror)
{
    int32_t r, c;
    for (r = 0; r < (int32_t)h; r++) {
        int32_t y = dy + r;
        if (y < 0 || y >= H) continue;
        for (c = 0; c < (int32_t)w; c++) {
            int32_t x = dx + c;
            uint8_t px;
            if (x < 0 || x >= PITCH) continue;
            px = amem[(sy + (uint32_t)r) * AW +
                      (mirror ? (sx + w - 1 - (uint32_t)c)
                              : (sx + (uint32_t)c))];
            if (keyed && px == key) continue;
            ref[y * PITCH + x] = px;
        }
    }
}

int main(void)
{
    jspr_surface_t dst, atlas;
    uint32_t i;
    int32_t dx, dy;
    int m;

    for (i = 0; i < AW * AH; i++)
        amem[i] = (uint8_t)(((i % AW) / 4 + (i / AW / 4)) & 1 ? (i & 0xFF) : 0);
    memset(dmem, 7, sizeof(dmem));
    memset(ref, 7, sizeof(ref));

    dst.base = BASE_D; dst.pitch = PITCH; dst.w = PITCH; dst.h = H;
    atlas.base = BASE_A; atlas.pitch = AW; atlas.w = AW; atlas.h = AH;

    jspr_set_exec(exec_model);

    /* Fill via jspr (clipped) */
    jspr_fill(&dst, -10, -10, 120, 60, 0x22);
    for (dy = 0; dy < 50; dy++)
        for (dx = 0; dx < 110; dx++)
            ref[dy * PITCH + dx] = 0x22;

    /* Sweep a 48x32 sprite across all edges, mirrored and not, keyed */
    for (m = 0; m < 2; m++) {
        static const int32_t P[][2] = {
            {-30, 100}, {600, 120}, {300, -20}, {280, 420},
            {-47, -31}, {615, 439}, {200, 200}, {0, 0},
        };
        for (i = 0; i < sizeof(P) / sizeof(P[0]); i++) {
            int32_t px = P[i][0] + (m ? 7 : 0);
            int32_t py = P[i][1];
            uint32_t fl = m ? JSPR_HFLIP : 0;
            jspr_blit(&dst, px, py, &atlas, 8, 4, 48, 32, 0, fl);
            ref_sprite(px, py, 8, 4, 48, 32, 0, 1, m);
        }
    }

    /* Opaque blit */
    jspr_blit(&dst, 500, 50, &atlas, 0, 0, 64, 40, 0, JSPR_OPAQUE);
    ref_sprite(500, 50, 0, 0, 64, 40, 0, 0, 0);

    /* Fully-clipped must report -1 and draw nothing */
    if (jspr_blit(&dst, -100, 10, &atlas, 0, 0, 48, 32, 0, 0) != -1 ||
        jspr_blit(&dst, 10, 500, &atlas, 0, 0, 48, 32, 0, 0) != -1) {
        printf("FAIL: fully-clipped blit not rejected\n");
        return 1;
    }

    if (memcmp(dmem, ref, sizeof(dmem)) != 0) {
        for (i = 0; i < sizeof(dmem); i++)
            if (dmem[i] != ref[i]) break;
        printf("FAIL: sprite output differs at byte %u (%02x vs %02x)\n",
               i, dmem[i], ref[i]);
        return 1;
    }

    printf("CRC spr=%08x\n", (unsigned)crc32_buf(dmem, sizeof(dmem)));
    printf("spr tests OK\n");
    return 0;
}
