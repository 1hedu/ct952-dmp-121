/*
 * JupiterSDK on CT952 -- 2D engine (jgpu) verification.
 *
 * The op builders are pure functions and the software model executes
 * exactly the walk the register programming implies, so correctness is
 * testable off-hardware:
 *
 *   1. Every built op's register-derived geometry must round-trip:
 *      the model reconstructs the pitch from ag_off and must land on
 *      the same rows the builder intended.
 *   2. Model results must MATCH the CPU reference (jdraw / plain
 *      loops) byte-exactly for a battery of fills and blits covering
 *      unaligned x, odd widths, color key on/off, mirror, sub-rect
 *      sources, and surface-to-surface copies.
 *   3. Guard canaries around all surfaces stay intact (the ag math
 *      must never step outside the rect).
 *   4. CRCs endian-stable (native vs qemu-sparc64).
 */
#include <stdio.h>
#include <string.h>
#include "jgpu.h"
#include "jdraw.h"

#define PITCH  616
#define H      440
#define BASE_D 0x10000u   /* pretend DRAM addresses */
#define BASE_S 0x80000u
#define GUARD  2048

static uint8_t dmem[GUARD + PITCH * H + GUARD];
static uint8_t smem[GUARD + PITCH * H + GUARD];
static uint8_t ref[PITCH * H];
static uint8_t src_lin[PITCH * H];

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

static uint8_t *dst_at(void) { return dmem + GUARD; }

static int check_guards(const char *what)
{
    uint32_t i;
    for (i = 0; i < GUARD; i++) {
        if (dmem[i] != 0xA5 || dmem[GUARD + PITCH * H + i] != 0xA5 ||
            smem[i] != 0xA5 || smem[GUARD + PITCH * H + i] != 0xA5) {
            printf("FAIL: guard corrupted after %s\n", what);
            return 1;
        }
    }
    return 0;
}

static int compare(const char *what)
{
    if (memcmp(dst_at(), ref, PITCH * H) != 0) {
        uint32_t i;
        for (i = 0; i < PITCH * H; i++)
            if (dst_at()[i] != ref[i]) break;
        printf("FAIL: %s differs from reference at byte %u (%02x vs %02x)\n",
               what, i, dst_at()[i], ref[i]);
        return 1;
    }
    return check_guards(what);
}

/* CPU reference blit with the intended semantics */
static void ref_blit(uint32_t dx, uint32_t dy,
                     uint32_t sx, uint32_t sy,
                     uint32_t w, uint32_t h,
                     uint8_t key, int keyed, int mirror)
{
    uint32_t r, c;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            uint8_t px = src_lin[(sy + r) * PITCH +
                                 (mirror ? (sx + w - 1 - c) : (sx + c))];
            if (keyed && px == key) continue;
            ref[(dy + r) * PITCH + dx + c] = px;
        }
}

int main(void)
{
    jgpu_op_t op;
    uint32_t i;
    int t;

    /* deterministic source surface */
    for (i = 0; i < PITCH * H; i++)
        src_lin[i] = (uint8_t)((i * 7) ^ (i >> 9));
    memset(dmem, 0xA5, sizeof(dmem));
    memset(smem, 0xA5, sizeof(smem));
    memcpy(smem + GUARD, src_lin, PITCH * H);

    memset(dst_at(), 0, PITCH * H);
    memset(ref, 0, PITCH * H);

    /* ---- fills: aligned, unaligned x, odd width, 1px, full width ---- */
    {
        static const uint32_t F[][4] = {
            {0, 0, 616, 440},     /* full surface */
            {4, 8, 100, 50},      /* aligned */
            {3, 17, 101, 33},     /* unaligned x, odd w */
            {613, 0, 3, 440},     /* right edge, narrow */
            {77, 431, 539, 9},    /* bottom */
            {1, 1, 1, 1},         /* single pixel */
        };
        for (t = 0; t < (int)(sizeof(F) / sizeof(F[0])); t++) {
            uint8_t col = (uint8_t)(0x30 + t);
            if (jgpu_build_fill(&op, BASE_D, PITCH,
                                F[t][0], F[t][1], F[t][2], F[t][3],
                                col, (t & 1) ? JGPU_F_HP : 0) != 0) {
                printf("FAIL: build_fill %d rejected\n", t);
                return 1;
            }
            jgpu_model_exec(&op, dst_at(), BASE_D, NULL, 0);
            jdraw_rect(ref, PITCH, (int)F[t][0], (int)F[t][1],
                       (int)F[t][2], (int)F[t][3], col);
        }
        if (compare("fills")) return 1;
        printf("CRC gpu_fills=%08x\n",
               (unsigned)crc32_buf(dst_at(), PITCH * H));
    }

    /* ---- blits: plain, keyed, mirrored, unaligned, sub-rect ---- */
    {
        struct {
            uint32_t dx, dy, sx, sy, w, h;
            uint8_t key;
            uint32_t flags;
        } B[] = {
            {0, 0, 0, 0, 64, 64, 0, 0},
            {100, 10, 32, 200, 200, 100, 0, 0},
            {301, 111, 3, 5, 97, 55, 0, 0},          /* both unaligned */
            {40, 300, 200, 50, 120, 80, 0x12, JGPU_F_KEY},
            {400, 250, 17, 301, 111, 77, 0x00, JGPU_F_KEY | JGPU_F_HP},
            {200, 380, 0, 0, 128, 32, 0, JGPU_F_MIRROR},
            {500, 400, 599, 420, 17, 20, 0, JGPU_F_HP}, /* corners */
        };
        for (t = 0; t < (int)(sizeof(B) / sizeof(B[0])); t++) {
            if (jgpu_build_blit(&op, BASE_D, PITCH, B[t].dx, B[t].dy,
                                BASE_S, PITCH, B[t].sx, B[t].sy,
                                B[t].w, B[t].h, B[t].key,
                                B[t].flags) != 0) {
                printf("FAIL: build_blit %d rejected\n", t);
                return 1;
            }
            jgpu_model_exec(&op, dst_at(), BASE_D, smem + GUARD, BASE_S);
            ref_blit(B[t].dx, B[t].dy, B[t].sx, B[t].sy, B[t].w, B[t].h,
                     B[t].key, (B[t].flags & JGPU_F_KEY) != 0,
                     (B[t].flags & JGPU_F_MIRROR) != 0);
        }
        if (compare("blits")) return 1;
        printf("CRC gpu_blits=%08x\n",
               (unsigned)crc32_buf(dst_at(), PITCH * H));
    }

    /* ---- limits ---- */
    if (jgpu_build_fill(&op, BASE_D, PITCH, 0, 0, 1024, 10, 1, 0) == 0 ||
        jgpu_build_fill(&op, BASE_D, PITCH, 0, 0, 10, 1024, 1, 0) == 0 ||
        jgpu_build_fill(&op, BASE_D, 615, 0, 0, 10, 10, 1, 0) == 0 ||
        jgpu_build_fill(&op, BASE_D, PITCH, 0, 0, 0, 10, 1, 0) == 0) {
        printf("FAIL: limit violations accepted\n");
        return 1;
    }

    /* ---- beam gate packing ---- */
    jgpu_build_fill(&op, BASE_D, PITCH, 0, 100, 64, 64, 5, 0);
    jgpu_op_set_gate(&op, 100, 163);
    if (op.op_thre != ((100u << 16) | 163u) || !(op.ctl0 & 0x800)) {
        printf("FAIL: beam gate packing\n");
        return 1;
    }

    printf("gpu tests OK\n");
    return 0;
}
