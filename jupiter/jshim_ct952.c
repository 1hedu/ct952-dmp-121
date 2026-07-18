/*
 * JupiterSDK on CT952 -- platform shim implementation.
 *
 * Maps the Jupiter SDK platform layer onto the DVD firmware's services:
 *   video  -> GDI OSD region 0, 8bpp, full screen, at the media-mode OSD
 *            frame buffer (DS_OSDFRAME_ST_MM). The OSD engine scans the
 *            buffer directly, so rendering is single-buffered.
 *   audio  -> the HAL raw-PCM pipe (HAL_AM_AUDIO_TYPE 7), the same
 *            mechanism HAL_PlayTone uses for the key-click tone.
 *   input  -> IR/panel KEY_* codes mapped to JBTN_* masks.
 *   time   -> OS_GetSysTimer (eCos counter, SYSTEM_TICK ms units).
 *
 * NOTE: like the built-in OSD games, the Jupiter app must own the screen
 * (media playback stopped) before jvid_open() -- the OSD buffer and the
 * audio buffer it borrows are otherwise in use by the decoders.
 */
#include "Winav.h"
#include "gdi.h"
#include "hal.h"
#include "hdecoder.h"
#include "chips.h"      /* VOLUME_MAX */
#include "input.h"

/* HAL_PlayTone (hal.c) uses HAL_AM_ABUF0_ADR/LEN, which come from the
 * eCos install's headers (not in this source tree). hdecoder.h leaves
 * opcodes 0x1F..0x22 unassigned immediately before PCMBUF0_ADR (0x23),
 * matching the ABUF0/ABUF1 address+length pairs. UNVERIFIED against the
 * original header -- if the real build provides these, its values win. */
#ifndef HAL_AM_ABUF0_ADR
#define HAL_AM_ABUF0_ADR  0x1f
#endif
#ifndef HAL_AM_ABUF0_LEN
#define HAL_AM_ABUF0_LEN  0x20
#endif

/* From osd.c (osd.h drags in the whole OSD resource world; this is the
 * only symbol we need from it). */
extern void OSD_SetRegion(BYTE bRegion, BYTE bClearRegion,
                          GDI_REGION_INFO *RegionInfo);

#include "jshim.h"
#include "jrgb2yuv.h"

/* ---- Video ---- */

static GDI_REGION_INFO _JupRegionInfo;

uint8_t *jvid_fb(void)
{
    return (uint8_t *)DS_OSDFRAME_ST_MM;
}

void jvid_open(uint8_t bg_index)
{
    uint8_t *fb = jvid_fb();
    DWORD i;

    _JupRegionInfo.wWidth = JVID_W;
    _JupRegionInfo.wHeight = JVID_H;
    _JupRegionInfo.bColorMode = GDI_OSD_8B_MODE;
    _JupRegionInfo.dwTAddr = DS_OSDFRAME_ST_MM;

    GDI_DeactivateRegion();

    /* Pre-fill before the region goes live so the first field shown is
     * already the background color, not stale decoder data. */
    for (i = 0; i < (DWORD)JVID_PITCH * JVID_H; i++)
        fb[i] = bg_index;

    OSD_SetRegion(0, FALSE, &_JupRegionInfo);
    GDI_SetMixRatio(GDI_GENERAL_MIX_RATIO);
    GDI_ActivateRegion(0);
}

void jvid_close(void)
{
    GDI_DeactivateRegion();
}

void jvid_load_palette(uint8_t base, const uint32_t *argb, int count)
{
    int i;
    for (i = 0; i < count; i++)
        /* Tag the value with GDI_VALUE_YUV (top byte 0x5A) so
         * GDI_ChangePALEntry stores our BT.601 YUV directly. Without the
         * tag it treats the word as RGB and runs COMUTL_RGB2YUV again --
         * a double conversion that corrupts every colour (gdi.c:886-901,
         * the 0xFE000000==GDI_VALUE_YUV check). */
        GDI_ChangePALEntry((BYTE)(base + i),
                           GDI_VALUE_YUV | (DWORD)jup_argb_to_yuv(argb[i]),
                           FALSE);
    GDI_WaitPaletteComplete();
}

/* ---- Audio ---- */

/* The MM-mode DSP audio bitstream buffer we borrow for raw PCM.
 * DS_AD0BUF_ST_MM..DS_AD0BUF_END_MM is 22 KDW (88 KB) in the 16M map;
 * stay well inside it. */
#define JSND_BUF_ADDR    DS_AD0BUF_ST_MM
#define JSND_MAX_SAMPLES 20000u   /* 40 KB, ~0.45 s at 44.1 kHz */

/* Scratch for rendering audio, in the same borrowed DSP buffer region,
 * right after the submission buffer (2x40 KB inside the 88 KB region). */
int16_t *jsnd_scratch(uint32_t *max_samples)
{
    if (max_samples)
        *max_samples = JSND_MAX_SAMPLES;
    return (int16_t *)(JSND_BUF_ADDR + JSND_MAX_SAMPLES * 2);
}

void jsnd_play(const int16_t *samples, uint32_t num_samples)
{
    volatile WORD *dst = (volatile WORD *)JSND_BUF_ADDR;
    uint32_t i;

    if (num_samples == 0 || samples == NULL)
        return;
    if (num_samples > JSND_MAX_SAMPLES)
        num_samples = JSND_MAX_SAMPLES;

    for (i = 0; i < num_samples; i++)
        dst[i] = (WORD)samples[i];

    /* Same submission sequence as HAL_PlayTone (hal.c) */
    HAL_WriteAM(HAL_AM_ABUF0_ADR, JSND_BUF_ADDR);
    HAL_WriteAM(HAL_AM_ABUF0_LEN, num_samples);
    HAL_WriteAM(HAL_AM_PCMBUF_ADR, DS_PCMBUF_ST);
    HAL_WriteAM(HAL_AM_PCMBUF_LEN, (DS_PCMBUF_END - DS_PCMBUF_ST) >> 11);
    HAL_WriteAM(HAL_AM_CHANNEL_MODE, HAL_TONE_CH_ALL);
    HAL_WriteAM(HAL_AM_PCM_SCALE, VOLUME_MAX);
    HAL_WriteAM(HAL_AM_AUDIO_TYPE, 7);          /* raw PCM */
    HAL_Reset(HAL_RESET_AUDIO);
    HAL_SetAudioDAC(AUDIO_FREQ_44K);
    HAL_WriteAM(HAL_AM_PLAY_COMMAND, 1);
    HAL_IOMute(FALSE);
    HAL_WriteAM(HAL_AM_AUDIO_TYPE, 0);
}

/* ---- Input ---- */

uint32_t jinp_map_key(uint8_t key)
{
    switch (key) {
    case KEY_UP:        return JBTN_UP;
    case KEY_DOWN:      return JBTN_DOWN;
    case KEY_LEFT:      return JBTN_LEFT;
    case KEY_RIGHT:     return JBTN_RIGHT;
    case KEY_ENTER:
    case KEY_PLAY:      return JBTN_A;
    case KEY_RETURN:    return JBTN_B;
    case KEY_PAUSE:     return JBTN_START;
    case KEY_MENU:      return JBTN_SELECT;
    default:            return 0;
    }
}

/* ---- Time ---- */

uint32_t jtime_ms(void)
{
    /* One OS_GetSysTimer() unit is SYSTEM_TICK*SLICE_TICK ms (= 2 ms) */
    return (uint32_t)OS_GetSysTimer() * (SYSTEM_TICK * SLICE_TICK);
}
