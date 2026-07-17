/*
 * JupiterSDK on CT952 -- codec layer (the Cedar analogue)
 *
 * On the V3s, the Jupiter SDK's Cedar layer exposed the hardware codec
 * (H.264 encode/decode + pixel-format conversion). The CT952's
 * equivalents, wrapped here:
 *
 *   CedarVE H.264 decode  -> MPEG-1/2 still/stream decode silicon,
 *                            fed FROM MEMORY via HAL_FillVideoBuffer
 *                            (the boot-logo idiom, utl.c:617-740)
 *   libcedarjpeg decode   -> hardware JPEG decoder (REG_JPG VLD + JPU
 *                            + DEQ), fed from memory via the JPEG-logo
 *                            idiom (utl.c:756-860)
 *   cedar H.264 encode    -> software libjpeg (encoder.a) reading an
 *                            arbitrary framebuffer region
 *                            (jpegenc.h, the photo-save idiom)
 *   argb<->nv12 helpers   -> jfb.h tiled-YUV canvas + jcodec_canvas_*
 *   (no analogue)         -> JPU hardware scale-copy between
 *                            framebuffer regions (the digest idiom,
 *                            digest.c:75-137)
 *
 * Firmware-only (implemented in jcodec_ct952.c against HAL/UTL/DISP).
 * HARDWARE-UNVERIFIED: these follow the firmware's own call sequences
 * line for line, but have not run on a real CT952 in this effort.
 *
 * Preconditions (same as the built-in games / logo path): the media
 * pipeline is stopped and the Jupiter app owns the screen.
 */
#ifndef JCODEC_H
#define JCODEC_H

#include "jup_types.h"
#include "jfb.h"

/* ---- Video-plane canvas ----
 * Bind a jfb canvas to hardware frame buffer `frame_idx` (0..3): fills
 * the descriptor from __DISPFrameInfo[] + REG_DISP_STRIPE, after
 * configuring the frame buffers via the firmware's own path. */
int  jcodec_canvas_open(jfb_t *fb, uint32_t frame_idx);

/* Show the canvas: select the frame on the main video plane and enable
 * it (DISP_Display + DISP_DisplayCtrl -- the logo display idiom). */
void jcodec_canvas_show(uint32_t frame_idx);

/* Hide the main video plane again. */
void jcodec_canvas_hide(void);

/* ---- MPEG still decode from memory (boot-logo idiom) ----
 * es/size_dw: an MPEG-1/2 still elementary stream in DRAM (DWORD
 * units). Decodes through the hardware VLD/IDCT/MC and displays the
 * decoded frame on the video plane. Returns 0 on success, -1 on
 * decode timeout (2 s). */
int  jcodec_mpeg_show_still(const uint32_t *es, uint32_t size_dw);

/* ---- JPEG decode from memory (JPEG-logo idiom) ----
 * jpeg/size_dw: a JFIF stream in DRAM (DWORD units). Runs the hardware
 * JPEG pipeline and displays the result. Returns the UTL status. */
int  jcodec_jpeg_show(const uint32_t *jpeg, uint32_t size_dw);

/* ---- JPEG encode of a framebuffer region (photo-save idiom) ----
 * Compresses a tiled-YUV source region (y/uv addresses + geometry, the
 * same values a jfb canvas holds) into `mempool`. Returns the JPEG
 * size in bytes, or 0 on failure. quality 1..100 (auto-steps down if
 * the result exceeds max_bytes). */
uint32_t jcodec_jpeg_encode(uint32_t mempool, uint32_t mempool_len,
                            uint32_t y_addr, uint32_t uv_addr,
                            uint16_t width, uint16_t height,
                            uint16_t strip,
                            uint16_t quality, uint32_t max_bytes);

/* ---- JPU hardware scale-copy (digest idiom) ----
 * Scales the luma+chroma of a tiled-YUV source rect into a destination
 * rect, both described by base addresses (DWORD-unit hardware
 * addresses as the JPU expects), strips and sizes. Busy-waits on
 * JPU_GO completion like all firmware users. */
void jcodec_jpu_scale(uint32_t src_y_addr, uint32_t src_c_addr,
                      uint16_t src_w, uint16_t src_h, uint16_t src_strip,
                      uint32_t dst_y_addr, uint32_t dst_c_addr,
                      uint16_t dst_w, uint16_t dst_h, uint16_t dst_strip);

#endif /* JCODEC_H */
