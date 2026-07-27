#!/usr/bin/env python3
"""Build a minimal FAT16 SD-card image with JPEG files in the root directory,
for the ct952emu SD-host-controller model. Usage:
    ./mkfatimg.py <out.img> <file1.jpg> [file2.jpg ...]
Default geometry: 512 B/sector, 4 sectors/cluster (2 KB clusters), FAT16,
volume label "PHOTOS". Sized to hold the given files with slack."""
import sys, os, struct, time

SECTOR = 512
SEC_PER_CLUS = 4
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 512          # 512 * 32 = 16 KB root dir = 32 sectors
MEDIA = 0xF8

def fat16_build(out, files):
    clus_bytes = SECTOR * SEC_PER_CLUS
    # data clusters needed
    data_clusters = 0
    for f in files:
        sz = os.path.getsize(f)
        data_clusters += max(1, (sz + clus_bytes - 1) // clus_bytes)
    data_clusters += 16                     # slack
    total_data_clusters = max(data_clusters, 4096)   # keep it FAT16 (>4085)
    # FAT size: entries = total_data_clusters + 2, 2 bytes each
    fat_bytes = (total_data_clusters + 2) * 2
    sec_per_fat = (fat_bytes + SECTOR - 1) // SECTOR
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    data_start_sec = RESERVED + NUM_FATS * sec_per_fat + root_sectors
    total_sectors = data_start_sec + total_data_clusters * SEC_PER_CLUS

    img = bytearray(total_sectors * SECTOR)

    # ---- boot sector / BPB ----
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', bs, 11, SECTOR)          # bytes/sector
    bs[13] = SEC_PER_CLUS
    struct.pack_into('<H', bs, 14, RESERVED)        # reserved sectors
    bs[16] = NUM_FATS
    struct.pack_into('<H', bs, 17, ROOT_ENTRIES)    # root dir entries
    if total_sectors < 0x10000:
        struct.pack_into('<H', bs, 19, total_sectors)   # total sectors (16b)
    else:
        struct.pack_into('<H', bs, 19, 0)
        struct.pack_into('<I', bs, 32, total_sectors)   # total sectors (32b)
    bs[21] = MEDIA
    struct.pack_into('<H', bs, 22, sec_per_fat)     # sectors/FAT
    struct.pack_into('<H', bs, 24, 63)              # sectors/track
    struct.pack_into('<H', bs, 26, 255)             # heads
    struct.pack_into('<I', bs, 28, 0)               # hidden sectors
    bs[36] = 0x80                                    # drive number
    bs[38] = 0x29                                    # ext boot sig
    struct.pack_into('<I', bs, 39, 0x12345678)      # volume id
    bs[43:54] = b'PHOTOS     '                       # volume label (11)
    bs[54:62] = b'FAT16   '
    struct.pack_into('<H', bs, 510, 0xAA55)
    img[0:SECTOR] = bs

    # ---- FATs ----
    fat = bytearray(sec_per_fat * SECTOR)
    struct.pack_into('<H', fat, 0, 0xFF00 | MEDIA)  # entry 0
    struct.pack_into('<H', fat, 2, 0xFFFF)          # entry 1 (EOC)
    # cluster chains assigned below as we place files

    root = bytearray(root_sectors * SECTOR)
    # volume-label dir entry
    root[0:11] = b'PHOTOS     '
    root[11] = 0x08

    def short_name(i, path):
        base = os.path.basename(path).upper()
        stem, dot, ext = base.partition('.')
        stem = ''.join(c for c in stem if c.isalnum())[:8].ljust(8)
        ext = ''.join(c for c in ext if c.isalnum())[:3].ljust(3)
        return (stem + ext).encode('ascii', 'replace')

    next_clus = 2
    entry = 1                                        # root entry index (0 = label)
    dt = struct.pack('<H', (12 << 11) | (0 << 5) | 0)   # 12:00:00
    dd = struct.pack('<H', ((2020 - 1980) << 9) | (1 << 5) | 1)  # 2020-01-01
    for f in files:
        data = open(f, 'rb').read()
        nclus = max(1, (len(data) + clus_bytes - 1) // clus_bytes)
        first = next_clus
        # write data + chain FAT
        for k in range(nclus):
            c = first + k
            off = (data_start_sec + (c - 2) * SEC_PER_CLUS) * SECTOR
            chunk = data[k * clus_bytes:(k + 1) * clus_bytes]
            img[off:off + len(chunk)] = chunk
            nxt = 0xFFFF if k == nclus - 1 else (c + 1)
            struct.pack_into('<H', fat, c * 2, nxt)
        # dir entry
        e = entry * 32
        root[e:e + 11] = short_name(entry, f)
        root[e + 11] = 0x20                          # archive
        root[e + 13] = 0                             # create time tenths
        root[e + 14:e + 16] = dt                     # create time
        root[e + 16:e + 18] = dd                     # create date
        root[e + 18:e + 20] = dd                     # access date
        root[e + 22:e + 24] = dt                     # write time
        root[e + 24:e + 26] = dd                     # write date
        struct.pack_into('<H', root, e + 26, first)  # first cluster low
        struct.pack_into('<I', root, e + 28, len(data))  # file size
        entry += 1
        next_clus += nclus

    # place FATs and root into image
    fat_off = RESERVED * SECTOR
    img[fat_off:fat_off + len(fat)] = fat
    fat_off2 = (RESERVED + sec_per_fat) * SECTOR
    img[fat_off2:fat_off2 + len(fat)] = fat
    root_off = (RESERVED + NUM_FATS * sec_per_fat) * SECTOR
    img[root_off:root_off + len(root)] = root

    open(out, 'wb').write(img)
    print("wrote %s: %d sectors (%d KB), FAT16, %d files, %d sec/FAT, data@sec %d"
          % (out, total_sectors, total_sectors * SECTOR // 1024,
             len(files), sec_per_fat, data_start_sec))

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    fat16_build(sys.argv[1], sys.argv[2:])
