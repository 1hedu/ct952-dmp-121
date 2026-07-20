#!/usr/bin/env python3
# modemap.py — dump the retail OSD UI-mode handler table from a live snapshot.
# The table is runtime-built in DRAM (0x40024c20..0x40024e00); records are 0x20
# bytes: {id(mode), +4 enter, +8 exit, +c common, +14 key/draw, ...}. OSD_ChangeUI
# (0xafd8) looks up record[0]==mode via 0xae50 and calls record+4. See §12.16.
#
# Usage: python3 modemap.py [seg.snap] [dp700wd.bin]
import struct, sys
snap = sys.argv[1] if len(sys.argv) > 1 else 'seg.snap'
rom  = sys.argv[2] if len(sys.argv) > 2 else 'dp700wd.bin'
b = open(rom, 'rb').read()
s = open(snap, 'rb').read()
# calibrate DRAM base: OSDSS_Monitor ptr 0x591b4 lives at DRAM 0x40020e18
base = [i for i in range(0, len(s)-3)
        if struct.unpack('>I', s[i:i+4])[0] == 0x591b4][0] - 0x20e18
def rd32(a): return struct.unpack('>I', s[base+(a-0x40000000):base+(a-0x40000000)+4])[0]
def cstr(off):
    e = off
    while e < len(b) and 0x20 <= b[e] < 0x7f and e-off < 80: e += 1
    return b[off:e].decode('ascii', 'replace')
def strings_in(fn, span=0x500):
    out, hi, i = [], {}, fn
    if not (0x1000 <= fn < 0xd0000): return out
    while i < min(fn+span, len(b)):
        w = struct.unpack('>I', b[i:i+4])[0]; op = w >> 30
        if op == 0 and ((w>>22)&7) == 4: hi[(w>>25)&0x1f] = (w&0x3FFFFF)<<10
        elif op == 2 and ((w>>19)&0x3f) == 2 and ((w>>13)&1) == 1:
            rs1 = (w>>14)&0x1f
            if rs1 in hi:
                a = hi[rs1] | (w&0x1fff)
                if 0x30000 <= a < 0xf8000:
                    t = cstr(a)
                    if len(t) >= 6 and sum(c.isalpha() for c in t) >= 4: out.append(t)
        if w == 0x81c7e008: break
        i += 4
    return out[:5]
print("DRAM base raw = 0x%x" % base)
print("mode  enter    exit     +c       key/draw  strings")
for a in range(0x40024c20, 0x40024e00, 0x20):
    idw = rd32(a)
    if idw == 0 or idw > 0x100: continue
    print("0x%02x  %08x %08x %08x %08x  %s" % (
        idw, rd32(a+4), rd32(a+8), rd32(a+0xc), rd32(a+0x14),
        strings_in(rd32(a+4))))
