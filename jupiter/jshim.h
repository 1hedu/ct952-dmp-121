/*
 * JupiterSDK on CT952 -- platform shim contract
 *
 * This is the CT952 equivalent of the Jupiter SDK's platform layer
 * (the video_, audio_, input_ and timer_ families in jupiter.h).
 * Implemented by jshim_ct952.c against the DVD firmware's GDI/OSD, HAL
 * audio, IR key and eCos timer services. The renderers (jnes/jgb),
 * jaudio and jdraw are platform-independent and sit on top of this.
 *
 * Mapping summary (V3s SDK -> CT952 firmware):
 *   video init/swap/commit  -> jvid_open/jvid_close (one live OSD
 *                              region; the OSD hardware scans the buffer
 *                              directly, so there is no swap -- draw
 *                              into jvid_fb())
 *   DE2 hardware palette    -> jvid_load_palette (ARGB->YUV, GDI palette)
 *   audio quickstart/update -> jsnd_play (one-shot PCM via HAL raw-PCM
 *                              path, the mechanism HAL_PlayTone uses)
 *   input poll / BTN masks  -> jinp_map_key (IR KEY_ -> JBTN_ bitmask;
 *                              the superloop delivers keys by call, not
 *                              by polling, so the app feeds keys in)
 *   timer read / ticks_to_ms -> jtime_ms (OS_GetSysTimer, ~2 ms units)
 */
#ifndef JSHIM_H
#define JSHIM_H

#include "jup_types.h"

/* ---- Video: one 8bpp palette-indexed OSD region ---- */

/* Fixed geometry of the Jupiter OSD region (the firmware's full-screen
 * media-mode region: 616 wide, 440 high NTSC / 540 PAL, 1 byte/pixel,
 * pitch == width). */
#define JVID_W      616
#define JVID_H      440   /* NTSC; PAL builds may use 540 */
#define JVID_PITCH  JVID_W

/* Take over OSD region 0 for Jupiter: configure it as 8bpp full-screen
 * at the media-mode OSD buffer, clear it to palette index bg_index,
 * and activate it. Call once when the app takes the screen. */
void jvid_open(uint8_t bg_index);

/* Release the region (deactivate OSD). The next UI owner (menus, media
 * UI) reconfigures the region itself, same as after the built-in games. */
void jvid_close(void);

/* Live framebuffer pointer (1 byte per pixel, JVID_PITCH bytes per row).
 * The OSD hardware scans this buffer directly -- no present/swap call. */
uint8_t *jvid_fb(void);

/* Convert count ARGB8888 colors to YUV and load them into the OSD
 * hardware palette starting at entry base. */
void jvid_load_palette(uint8_t base, const uint32_t *argb, int count);

/* ---- Audio: one-shot PCM playback via the HAL raw-PCM path ---- */

/* Play a mono 16-bit 44.1 kHz buffer once through the DVD DSP's raw-PCM
 * pipe (the same mechanism the firmware's key-click tone uses). The
 * samples are copied to the DSP audio buffer, so `samples` may live
 * anywhere. num_samples is capped by the DSP buffer size (~40 KB). */
void jsnd_play(const int16_t *samples, uint32_t num_samples);

/* DRAM scratch area for rendering audio before jsnd_play (the CT952's
 * linker RAM window is too small for large static buffers; big buffers
 * live in the borrowed AV DRAM pools). Returns the buffer and writes
 * its capacity in samples to *max_samples. */
int16_t *jsnd_scratch(uint32_t *max_samples);

/* ---- Input: IR/panel key -> Jupiter button mask ---- */

#define JBTN_A       (1u << 0)   /* KEY_ENTER / KEY_PLAY */
#define JBTN_B       (1u << 1)   /* KEY_RETURN */
#define JBTN_START   (1u << 4)   /* KEY_PAUSE  */
#define JBTN_SELECT  (1u << 5)   /* KEY_MENU   */
#define JBTN_UP      (1u << 6)
#define JBTN_DOWN    (1u << 7)
#define JBTN_LEFT    (1u << 8)
#define JBTN_RIGHT   (1u << 9)

/* Map a firmware KEY_* code to a JBTN_* mask (0 if unmapped). The
 * superloop pushes keys via JUPITER_ProcessKey, so unlike the SDK's
 * polled controllers, buttons arrive as press events. */
uint32_t jinp_map_key(uint8_t key);

/* ---- Time ---- */

/* Milliseconds since boot (from OS_GetSysTimer, ~2 ms granularity). */
uint32_t jtime_ms(void);

#endif /* JSHIM_H */
