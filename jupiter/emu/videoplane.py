#!/usr/bin/env python3
# videoplane.py — de-tile the CT952 video plane (macroblock-tiled YUV 4:2:0) from a
# live snapshot and render it to PNG. This is what the JPU decode wrote to the video
# framebuffer (Y@0x40065000, C@0x400B3C00, strip 0x2D00) — the pixels the scan-out
# DAC reads. See §12.12 (tiling) / §12.19.
#
# Usage: python3 videoplane.py [seg.snap] [WxH] [out.png]
import struct, sys
from PIL import Image
snap = sys.argv[1] if len(sys.argv) > 1 else 'seg.snap'
W, H = (int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else '640x360').split('x'))
out  = sys.argv[3] if len(sys.argv) > 3 else 'videoplane.png'
s = open(snap, 'rb').read()
# calibrate DRAM base via the OSDSS_Monitor pointer 0x591b4 @ DRAM 0x40020e18
base = [i for i in range(0, len(s)-3)
        if struct.unpack('>I', s[i:i+4])[0] == 0x591b4][0] - 0x20e18
def R(a, n): o = base + (a - 0x40000000); return s[o:o+n]
Y = R(0x40065000, 0x50000); C = R(0x400B3C00, 0x28000)
strip = 0x2D00
frac = sum(1 for x in Y[:0x10000] if x) / 0x10000
print("Y-plane nonzero = %.0f%%" % (100*frac))
clamp = lambda v: 0 if v < 0 else 255 if v > 255 else v
img = Image.new('RGB', (W, H)); px = img.load()
for y in range(H):
    for x in range(W):
        yo = (y>>4)*strip + (x>>2)*64 + (y&15)*4 + (x&3)
        Yv = Y[yo] if yo < len(Y) else 0
        cx, cy = x>>1, y>>1
        co = (cy>>4)*strip + (cx>>3)*256 + ((cx&7)>>2)*64 + (cy&15)*4 + (cx&3)
        U = (C[co] if co < len(C) else 128) - 128
        V = (C[co+128] if co+128 < len(C) else 128) - 128
        px[x, y] = (clamp(Yv + ((91881*V)>>16)),
                    clamp(Yv - ((22554*U + 46802*V)>>16)),
                    clamp(Yv + ((116130*U)>>16)))
img.save(out)
print("de-tiled %dx%d video plane -> %s" % (W, H, out))
