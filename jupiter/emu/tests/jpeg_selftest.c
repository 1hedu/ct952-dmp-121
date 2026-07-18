/* Decode a JPEG file to a PPM using the emulator's picojpeg path. */
#include "../emujpeg.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *buf, *rgb;
    long len;
    int w, h, rc;

    if (argc < 3) { fprintf(stderr, "usage: %s in.jpg out.ppm\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len) { fprintf(stderr, "read\n"); return 1; }
    fclose(f);

    rc = emu_jpeg_decode(buf, (unsigned int)len, &rgb, &w, &h);
    if (rc) { fprintf(stderr, "decode failed rc=%d\n", rc); return 1; }

    f = fopen(argv[2], "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
    fprintf(stderr, "decoded %dx%d -> %s\n", w, h, argv[2]);
    return 0;
}
