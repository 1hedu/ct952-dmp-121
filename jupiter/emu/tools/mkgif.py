#!/usr/bin/env python3
"""Assemble an animated GIF from anim_run's dump.

Input file layout (little-endian):
    u32 w, u32 h, u32 nframes, 256*3 RGB palette, nframes * (w*h) 8bpp indices

Usage: mkgif.py <anim.bin> <out.gif> [delay_centiseconds]

The frames are already palette-indexed (the CT952 OSD plane), so they map
straight onto a GIF global colour table -- no quantisation needed.
"""
import struct
import sys


def lzw_encode(indices, min_code_size):
    clear = 1 << min_code_size
    eoi = clear + 1
    code_size = min_code_size + 1
    table = {(i,): i for i in range(clear)}
    next_code = eoi + 1

    out = bytearray()
    cur = 0        # bit accumulator
    ncur = 0       # bits in accumulator

    def emit(code):
        nonlocal cur, ncur
        cur |= code << ncur
        ncur += code_size
        while ncur >= 8:
            out.append(cur & 0xFF)
            cur >>= 8
            ncur -= 8

    emit(clear)
    prefix = (indices[0],)
    for idx in indices[1:]:
        nxt = prefix + (idx,)
        if nxt in table:
            prefix = nxt
        else:
            emit(table[prefix])
            table[nxt] = next_code
            next_code += 1
            if next_code == (1 << code_size) and code_size < 12:
                code_size += 1
            if next_code > 4095:
                emit(clear)
                table = {(i,): i for i in range(clear)}
                next_code = eoi + 1
                code_size = min_code_size + 1
            prefix = (idx,)
    emit(table[prefix])
    emit(eoi)
    if ncur > 0:
        out.append(cur & 0xFF)
    return bytes(out)


def block_ify(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i+255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    delay = int(sys.argv[3]) if len(sys.argv) > 3 else 6   # centiseconds
    d = open(src, 'rb').read()
    w, h, nf = struct.unpack_from('<III', d, 0)
    off = 12
    pal = d[off:off+768]; off += 768
    frames = []
    for _ in range(nf):
        frames.append(d[off:off+w*h]); off += w*h

    g = bytearray()
    g += b'GIF89a'
    g += struct.pack('<HH', w, h)
    g += bytes([0xF7, 0, 0])            # global table, 256 entries, 8bpp
    g += pal
    # NETSCAPE loop-forever
    g += b'\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00'
    for fr in frames:
        # graphic control extension (delay, no transparency)
        g += b'\x21\xF9\x04\x00' + struct.pack('<H', delay) + b'\x00\x00'
        # image descriptor
        g += b'\x2C' + struct.pack('<HHHH', 0, 0, w, h) + b'\x00'
        mcs = 8
        g += bytes([mcs])
        g += block_ify(lzw_encode(fr, mcs))
    g += b'\x3B'
    open(dst, 'wb').write(g)
    print('wrote %s (%dx%d, %d frames, %.1fKB)' %
          (dst, w, h, nf, len(g)/1024.0))


if __name__ == '__main__':
    main()
