/*
 * JupiterSDK on CT952 -- 2bpp layer drawing (jdraw2) verification.
 *
 * The SP-plane bitmaps are 2bpp packed; wrong packing scribbles wrong
 * pixels on two neighbors. Verified: set/get round-trip over every
 * position and value, byte-level packing convention (leftmost pixel in
 * the high bits), fill/clear/frame patterns vs a naive reference,
 * guard canaries, endian-stable CRC.
 */
#include <stdio.h>
#include <string.h>
#include "jdraw2.h"

#define W 624
#define H 78
#define PITCH (W / 4)
#define GUARD 512

static uint8_t buf[GUARD + PITCH * H + GUARD];
static uint8_t ref[W * H];   /* 1 byte per pixel reference */

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

static uint8_t *bmp(void) { return buf + GUARD; }

static int verify(const char *what)
{
    uint32_t x, y, i;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (jdraw2_get(bmp(), PITCH, x, y) != ref[y * W + x]) {
                printf("FAIL: %s mismatch at %u,%u\n", what, x, y);
                return 1;
            }
    for (i = 0; i < GUARD; i++)
        if (buf[i] != 0xA5 || buf[GUARD + PITCH * H + i] != 0xA5) {
            printf("FAIL: guard corrupted after %s\n", what);
            return 1;
        }
    return 0;
}

int main(void)
{
    uint32_t x, y;

    memset(buf, 0xA5, sizeof(buf));

    /* packing convention: leftmost pixel = bits 7:6 */
    memset(bmp(), 0, PITCH * H);
    jdraw2_set(bmp(), PITCH, 0, 0, 3);
    if (bmp()[0] != 0xC0) {
        printf("FAIL: packing (got %02x, want C0)\n", bmp()[0]);
        return 1;
    }
    jdraw2_set(bmp(), PITCH, 3, 0, 2);
    if (bmp()[0] != 0xC2) {
        printf("FAIL: packing pixel 3 (got %02x, want C2)\n", bmp()[0]);
        return 1;
    }

    /* full-surface set/get round trip with a rolling pattern */
    memset(ref, 0, sizeof(ref));
    jdraw2_clear(bmp(), PITCH, H, 0);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint8_t ci = (uint8_t)((x * 3 + y * 7 + (x >> 3)) & 3);
            jdraw2_set(bmp(), PITCH, x, y, ci);
            ref[y * W + x] = ci;
        }
    if (verify("roundtrip")) return 1;

    /* clear / fill / frame vs reference */
    jdraw2_clear(bmp(), PITCH, H, 2);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) ref[y * W + x] = 2;
    jdraw2_fill(bmp(), PITCH, 13, 5, 101, 30, 1);
    for (y = 5; y < 35; y++)
        for (x = 13; x < 114; x++) ref[y * W + x] = 1;
    jdraw2_frame(bmp(), PITCH, 2, 2, 620, 74, 3, 3);
    for (y = 2; y < 76; y++)
        for (x = 2; x < 622; x++)
            if (y < 5 || y >= 73 || x < 5 || x >= 619)
                ref[y * W + x] = 3;
    if (verify("shapes")) return 1;

    printf("CRC layer=%08x\n", (unsigned)crc32_buf(bmp(), PITCH * H));
    printf("layer tests OK\n");
    return 0;
}
