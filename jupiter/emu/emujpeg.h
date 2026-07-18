/*
 * emujpeg.h -- decode a baseline JPEG buffer to a packed RGB888 raster.
 * Returns 0 on success (caller frees *out_rgb), negative on error.
 */
#ifndef CT952EMU_EMUJPEG_H
#define CT952EMU_EMUJPEG_H

int emu_jpeg_decode(const unsigned char *jpeg, unsigned int len,
                    unsigned char **out_rgb, int *out_w, int *out_h);

#endif
