/*
 * ct952emu OSD-palette verification: load the DISP OSD palette RAM the
 * way the Jupiter port's jvid_load_palette() does -- ARGB -> BT.601 YUV
 * via jup_argb_to_yuv(), written to GAM_OSD -- then let the emulator's
 * DISP scan-out convert it back so the host can check each colour
 * round-trips (converter format <-> register <-> scan-out).
 *
 * Two palette banks are written for the same colours:
 *   [0 .. N)   FIXED : GAM_OSD = jup_argb_to_yuv(argb)   (tagged-YUV path)
 *   [N .. 2N)  BUGGY : GAM_OSD = jup_argb_to_yuv(that)   (the double
 *              conversion GDI_ChangePALEntry does to an UN-tagged value)
 * and two index strips are drawn so the host can compare both.
 *
 * The expected ARGB table is left at ARGB_OUT for the host.
 * Freestanding: start.S calls testmain().
 */
#include "jup_types.h"
#include "jrgb2yuv.h"
#include "testapi.h"

#define GAM_OSD   ((volatile uint32_t *)0x80001C00u)   /* REG_DISP_GAM_OSD */
#define OSD_SIZE  (*(volatile uint32_t *)0x80001A54u)
#define OSD_EN    0x10000000u

#define OSD_BASE  0x4005F000u
#define SW        10u                    /* strip width in px */
#define SH        24u                    /* strip height       */
#define OW        (16u * SW)             /* 160 wide (16 colours) */
#define OH        (2u * SH)              /* two banks stacked     */
#define ARGB_OUT  0x40380000u            /* host reads expected colours here */

static const uint32_t COLORS[16] = {
    0x000000, 0xFFFFFF, 0x404040, 0x808080, 0xC0C0C0,
    0xFF0000, 0x00FF00, 0x0000FF,
    0x00FFFF, 0xFF00FF, 0xFFFF00,
    0xFF8000, 0x8000FF, 0x0080FF, 0x80FF00, 0x2E5C8A
};

unsigned testmain(void)
{
    uint8_t  *fb  = (uint8_t *)OSD_BASE;
    uint32_t *exp = (uint32_t *)ARGB_OUT;
    uint32_t i, x, y;

    for (i = 0; i < 16; i++) {
        uint32_t fixed = jup_argb_to_yuv(COLORS[i]);   /* port's YUV */
        uint32_t buggy = jup_argb_to_yuv(fixed);        /* double convert */
        GAM_OSD[i]      = fixed;         /* bank 0: correct  */
        GAM_OSD[16 + i] = buggy;         /* bank 1: the bug  */
        exp[i] = COLORS[i];
    }

    /* two rows of index strips: top = fixed bank, bottom = buggy bank */
    for (y = 0; y < OH; y++)
        for (x = 0; x < OW; x++) {
            uint32_t col = x / SW;                    /* 0..15 */
            uint32_t bank = (y < SH) ? 0u : 16u;
            fb[y * OW + x] = (uint8_t)(bank + col);
        }

    OSD_SIZE = OSD_EN | (OH << 16) | OW;
    return 16;
}
