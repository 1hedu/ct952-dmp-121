/*
 * JupiterSDK on CT952 -- codec layer implementation.
 *
 * Every sequence here is the firmware's own idiom, parameterized:
 *   - canvas          <- GDI_FBDrawDot addressing (gdi.c:3548-3572) +
 *                        the logo display enable (utl.c:715-719)
 *   - mpeg still      <- UTL_ShowLogo MPEG branch (utl.c:617-740)
 *   - jpeg show       <- UTL_ShowLogo JPEG branch (utl.c:756-860)
 *   - jpeg encode     <- the photo-save call (mm_play.c:2760-2772)
 *   - jpu scale-copy  <- _DigestFrameCopy (digest.c:75-137)
 *
 * HARDWARE-UNVERIFIED: compile-checked against the real firmware
 * headers; not executed on silicon in this effort.
 */
#include "Winav.h"
#include "ctkav.h"      /* register maps: REG_DISP_*, REG_JPU_* */
#include "hal.h"
#include "hdecoder.h"
#include "disp.h"
#include "utl.h"
#include "initial.h"
#include "jpegdec.h"
#include "jpegenc.h"

#include "jcodec.h"

/* Playback-state globals the logo idiom manipulates (owned by cc/utl) */
extern BYTE  __bModePlay;
extern BYTE  __bAttrPlay;
extern BYTE  __bAttrPlayNew;
extern BYTE  __bThumbnailMode;
extern DWORD __dwMMJPEGPreview;

/* GDI<->JPU arbitration mutex (gdi.c / digest.c) */
extern MUTEX_T __mutexGDIIdle;

/* ---- Video-plane canvas ---- */

int jcodec_canvas_open(jfb_t *fb, uint32_t frame_idx)
{
    DWORD dwStrip;

    if (frame_idx >= 4 || fb == NULL)
        return -1;

    /* Configure the frame buffers through the firmware's own path
     * (same call the MPEG logo makes) so __DISPFrameInfo is valid. */
    UTL_BFRModeSet(FALSE);
    UTL_Config_FrameBuffer(UTL_FRAMEBUF_MOTION);

    /* Strip derivation exactly as GDI_FBDrawDot (gdi.c:3553-3557):
     * REG_DISP_STRIPE unit is bytes on CT909P -> convert to DWORDs. */
    dwStrip = (REG_DISP_STRIPE & 0xFF) << 8;
#if defined(CT909P_IC_SYSTEM) || defined(CT909G_IC_SYSTEM)
    dwStrip /= 4;
#endif

    fb->y_base = (uint8_t *)__DISPFrameInfo[frame_idx].dwFY_Addr;
    fb->c_base = (uint8_t *)__DISPFrameInfo[frame_idx].dwFC_Addr;
    fb->strip = (uint32_t)dwStrip;
    fb->w = 704;   /* GDI_FBFillRect's FB_WIDTH/FB_HEIGHT clamp */
    fb->h = 480;
    return 0;
}

void jcodec_canvas_show(uint32_t frame_idx)
{
    DISP_Display((DWORD)frame_idx, DISP_MAINVIDEO);
    DISP_DisplayCtrl(DISP_MAINVIDEO, TRUE);
}

void jcodec_canvas_hide(void)
{
    DISP_DisplayCtrl(DISP_MAINVIDEO, FALSE);
}

/* ---- MPEG still from memory (UTL_ShowLogo MPEG branch) ---- */

int jcodec_mpeg_show_still(const uint32_t *es, uint32_t size_dw)
{
    DWORD dwCnt, dwStart;

    /* MPEG decoder thread must exist (logo idiom, utl.c:621-640) */
    if (!(OS_PeekFlag(&__fThreadInit) & INIT_DEC_THREAD_MPEG_DONE)) {
        INITIAL_ThreadInit(THREAD_MPEG_DECODER);
        OS_TimedWaitFlag(&__fThreadInit, INIT_DEC_THREAD_MPEG_DONE,
                         FLAG_WAITMODE_AND, COUNT_50_MSEC);
        if (!(OS_PeekFlag(&__fThreadInit) & INIT_DEC_THREAD_MPEG_DONE))
            return -1;
    }

    UTL_BFRModeSet(TRUE);
    UTL_Config_FrameBuffer(UTL_FRAMEBUF_MOTION);
    HAL_Reset(HAL_RESET_VIDEO);

    __bModePlay = MODE_PLAYUNKNOW;
    __bAttrPlay = ATTR_NONE;

    HAL_FillVideoBuffer(HAL_VIDEOBUF_NORMAL, (DWORD *)es, size_dw);
    HAL_ResetVideoDecoder(HAL_VIDEO_DECODER1 | HAL_VIDEO_DECODER2);

    HAL_PlayCommand(COMMAND_V_CLEAR_STILL, 0);
    HAL_PlayCommand(COMMAND_PLAY, 0);

    dwCnt = 0;
    dwStart = OS_GetSysTimer();
    while ((OS_GetSysTimer() - dwStart) < COUNT_2_SEC) {
        HAL_ReadInfo(HAL_INFO_STILL, &dwCnt);
        if (dwCnt) {
            HAL_ReadInfo(HAL_INFO_DECFRAME, &dwCnt);
            HAL_PlayCommand(COMMAND_STOP, 0);
            DISP_Display(dwCnt, DISP_MAINVIDEO);
            DISP_DisplayCtrl(DISP_MAINVIDEO, TRUE);
            HAL_PlayCommand(COMMAND_V_CLEAR_STILL, 0);
            return 0;
        }
        OS_YieldThread();
    }
    return -1;   /* decode timeout */
}

/* ---- JPEG from memory (UTL_ShowLogo JPEG branch) ---- */

int jcodec_jpeg_show(const uint32_t *jpeg, uint32_t size_dw)
{
    __bModePlay = MODE_PLAYUNKNOW;
    __bAttrPlayNew = ATTR_JPG;
    __bThumbnailMode = 0;
    __dwMMJPEGPreview = 0;

    UTL_SetPlayMode(MODE_PLAYVIDEO);

    HAL_FillVideoBuffer(HAL_VIDEOBUF_MM, (DWORD *)jpeg, size_dw);
    HAL_ResetVideoDecoder(HAL_VIDEO_DECODER1 | HAL_VIDEO_DECODER2);

    return (int)UTL_ShowJPEG_Slide(JPEG_PARSE_TYPE_NORMAL, 0);
}

/* ---- JPEG encode of a framebuffer region ---- */

uint32_t jcodec_jpeg_encode(uint32_t mempool, uint32_t mempool_len,
                            uint32_t y_addr, uint32_t uv_addr,
                            uint16_t width, uint16_t height,
                            uint16_t strip,
                            uint16_t quality, uint32_t max_bytes)
{
    WORD wStatus;

    if (mempool_len < JPEGENC_REQUIRED_MEMPOOL_SIZE)
        return 0;
    if (max_bytes == 0)
        max_bytes = JPEGENC_COMPRESSION_FILESIZE_MAX;

    wStatus = JPEGENC_StartCompression(mempool, mempool_len,
                                       y_addr, uv_addr,
                                       width, height, strip,
                                       0, 0,   /* PicCoordH/V */
                                       quality,
                                       JPEGENC_COMPRESSION_QUALITY_DEFSTEP,
                                       max_bytes);
    if (wStatus != JPEGENC_COMPRESSION_SUCCESSED)
        return 0;

    return (uint32_t)JPEGENC_GetCompressedSize(mempool);
}

/* ---- JPU scale-copy (digest idiom, parameterized) ----
 * Addresses are the DWORD-unit hardware addresses the JPU expects
 * (byte address / 8 * 8 as used by digest.c). Destination rects must
 * start on tile boundaries (the digest sub-tile write-offset fields
 * are programmed 0 here). */

void jcodec_jpu_scale(uint32_t src_y_addr, uint32_t src_c_addr,
                      uint16_t src_w, uint16_t src_h, uint16_t src_strip,
                      uint32_t dst_y_addr, uint32_t dst_c_addr,
                      uint16_t dst_w, uint16_t dst_h, uint16_t dst_strip)
{
    DWORD dwHStep, dwVStep;

    if (src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2)
        return;

    OS_LockMutex(&__mutexGDIIdle);

    /* Luma pass (digest.c:98-124, sub-tile offsets zero) */
    REG_JPU_CTRL = (1 << 4) | (0 << 3);
    REG_JPU_HEIWID_SRC = ((DWORD)src_w << 12) | src_h;
    REG_JPU_HEIWID_DST = ((DWORD)dst_w << 12) | dst_h;
    dwHStep = (((DWORD)src_w - 1) * 0x2000) / ((DWORD)dst_w - 1);
    dwVStep = (((DWORD)src_h - 1) * 0x2000) / ((DWORD)dst_h - 1);
    REG_JPU_HVSC_FACTOR = (dwVStep << 16) | dwHStep;
    REG_JPU_STRIPE_RW = ((DWORD)dst_strip << 16) | src_strip;
    REG_JPU_ADDR_R_ST = src_y_addr;
    REG_JPU_ADDR_W_ST = dst_y_addr;
    REG_JPU_CTRL |= JPU_GO;
    while (REG_JPU_CTRL & 1)
        OS_YieldThread();

    /* Chroma pass (digest.c:126-134) */
    REG_JPU_CTRL = (1 << 4) | (1 << 3);
    REG_JPU_ADDR_R_ST = src_c_addr;
    REG_JPU_ADDR_W_ST = dst_c_addr;
    REG_JPU_CTRL |= JPU_GO;
    while (REG_JPU_CTRL & 1)
        OS_YieldThread();

    OS_UnlockMutex(&__mutexGDIIdle);
}
