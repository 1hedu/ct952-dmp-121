#!/usr/bin/env python3
"""
ctkap.py -- CheerTek CT909/CT952A flash-image + UPG952A.AP tool.

Reverse-engineered from dp700wd.bin (the CT952A firmware): the section-table
layout, the per-section 16-bit byte-sum checksums, and the CT909-AP update
container header (magic, chip code, size, APPacker version, body checksum).
Every field and checksum below is verified against the firmware's own loader
(validator @0x4170, checksum routine @0x40320) -- see CT952A_FLASH_FORMAT.md.

  ctkap.py sections  <flash.bin>            # dump the section table + verify ALL checksums (UZIP too)
  ctkap.py unpack    <flash.bin> <NAME> <out.bin>   # extract + UZIP-decompress a section
  ctkap.py repack    <body.bin> <out.uz>    # UZIP-compress a section body (LZMA1 lc0/lp1/pb2)
  ctkap.py apinfo    <UPG952A.AP>           # parse + validate a CT909-AP update file
  ctkap.py apfix     <UPG952A.AP>           # recompute size(0x0c)+checksum(0x2e) in place
  ctkap.py apwrap    <body.bin> <out.AP> [--code 0x41]   # wrap a raw AP body into a .AP

WARNING: reflashing is irreversible-ish and can brick hardware. This tool builds
and *validates* images; it does not talk to a device. Always `apinfo` a file and
confirm every field before it ever touches a real unit.
"""
import sys, struct, lzma

def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def sum16(b):  return sum(b) & 0xFFFF          # the firmware's only checksum kind

# ---- UZIP section compression -----------------------------------------------
# UZIP is plain LZMA1 (props byte 0x63 => lc=0, lp=1, pb=2) inside a 13-byte
# XOR-obfuscated container. Verified: the firmware decoder (flash 0x20b4) and
# Python's lzma FORMAT_RAW decode identical bytes; system liblzma FORMAT_ALONE
# corroborates. Decoder uses the output buffer as its window, so any dict_size
# >= the uncompressed size is safe. See CT952A_FLASH_FORMAT.md.
UZIP_KEY   = 0x5A5A5A5A
UZIP_PROP  = 0x63          # (pb=2)*45 + (lp=1)*9 + (lc=0)
UZIP_MAGIC = (UZIP_PROP << 24) | 0x80    # deobfuscated header word0 = 0x63000080
_UZIP_LC, _UZIP_LP, _UZIP_PB = 0, 1, 2

def _uzip_filt(dict_size):
    return [{"id": lzma.FILTER_LZMA1, "lc": _UZIP_LC, "lp": _UZIP_LP,
             "pb": _UZIP_PB, "dict_size": dict_size}]

def uzip_unwrap(packed):
    """(uncompressed_size, raw_lzma_stream) from a UZIP section blob."""
    w = [struct.unpack_from(">I", packed, 4*k)[0] ^ UZIP_KEY for k in range(3)]
    size = (((w[1] >> 16) & 0xff) | (w[1] & 0xff00)
            | ((w[1] & 0xff) << 16) | (w[2] & 0xff000000))
    return size, packed[0x0D:]

def uzip_decompress(packed):
    size, stream = uzip_unwrap(packed)
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=_uzip_filt(1 << 26))
    out = dec.decompress(stream, size)
    if len(out) != size:
        raise ValueError("UZIP: decoded %d, header says %d" % (len(out), size))
    return out

def uzip_compress(data):
    """Compress to a UZIP section blob (13-byte obfuscated header + LZMA1)."""
    size = len(data)
    ds = 1 << max(12, (size - 1).bit_length())          # dict >= size
    enc = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=_uzip_filt(ds))
    stream = enc.compress(data) + enc.flush()
    w0 = UZIP_MAGIC
    w1 = ((size & 0xff) << 16) | (((size >> 8) & 0xff) << 8) | ((size >> 16) & 0xff)
    w2 = ((size >> 24) & 0xff) << 24
    hdr = struct.pack(">III", w0 ^ UZIP_KEY, w1 ^ UZIP_KEY, w2 ^ UZIP_KEY) + b"\x5a"
    return hdr + stream

# ---- CT909-AP update-container header (512 bytes), all big-endian ------------
AP_MAGIC0 = 0x43543930   # "CT90"
AP_MAGIC1 = 0x392d4150   # "9-AP"   -> first 8 bytes = "CT909-AP"
AP_HDR_LEN   = 0x200
AP_MAX_SIZE  = 0x166000   # reserved AP code area
OFF_FORCE    = 0x08       # u32: 1 => allow oversize + reboot-on-fail
OFF_SIZE     = 0x0c       # u32: total AP size (incl. header), <= 0x166000, 4-aligned
OFF_CKEN     = 0x10       # u32: 1 => body checksum is verified
OFF_PKVER    = 0x14       # u32: APPacker version, must be >= 5
OFF_CHIP     = 0x18       # u32: chip/auto-upgrade code, 0x41 (952A) or 0x01 (universal)
OFF_CKSUM    = 0x2e       # u16: sum16(body[0x200:size])

def cmd_sections(path):
    d = open(path, "rb").read()
    print("reset=%08x  cfg_off=0x%x  boot_entry=0x%x" % (be32(d,0), be32(d,8), be32(d,0xc)))
    print("  name  lma        rma        lsz        rsz        cksum flags  chk")
    off = 0x10
    while off + 24 <= len(d):
        nm = d[off:off+4]
        if not all(32 <= c <= 126 for c in nm):
            break
        lma, rma, lsz, rsz = (be32(d, off+4), be32(d, off+8),
                              be32(d, off+12), be32(d, off+16))
        cks, flags = be16(d, off+20), be16(d, off+22)
        note = "raw" if rsz >= lsz else "UZIP"
        if cks == 0:
            chk = "n/a"                               # mutable sector (SETD/COPY): no checksum
        elif note == "raw" and rma + rsz <= len(d):   # raw: checksum is over flash bytes
            got = sum16(d[rma:rma+lsz])
            chk = "OK" if got == cks else "BAD(%04x)" % got
        else:                                          # UZIP: decompress, sum the unpacked bytes
            try:
                got = sum16(uzip_decompress(d[rma:rma+rsz]))
                chk = "OK" if got == cks else "BAD(%04x)" % got
            except Exception as e:
                chk = "ERR(%s)" % e
        print("  %-4s  %08x   %08x   %8x   %8x   %04x  %04x   %s"
              % (nm.decode('latin1'), lma, rma, lsz, rsz, cks, flags, chk))
        off += 24
    print("[note] raw-section checksums are the 16-bit byte-sum of the (unpacked==flash) "
          "data; compressed sections' checksums cover the *unpacked* bytes.")

def _find_section(d, name):
    off = 0x10
    while off + 24 <= len(d):
        nm = d[off:off+4]
        if not all(32 <= c <= 126 for c in nm):
            break
        if nm == name.encode()[:4].ljust(4)[:4] or nm.decode('latin1').strip() == name:
            return dict(off=off, name=nm.decode('latin1'), lma=be32(d,off+4), rma=be32(d,off+8),
                        lsz=be32(d,off+12), rsz=be32(d,off+16),
                        cks=be16(d,off+20), flags=be16(d,off+22))
        off += 24
    return None

def cmd_unpack(flash_path, name, out_path):
    d = open(flash_path, "rb").read()
    s = _find_section(d, name)
    if not s:
        print("no section %r" % name); return 1
    blob = d[s["rma"]:s["rma"]+s["rsz"]]
    data = uzip_decompress(blob) if s["rsz"] < s["lsz"] else blob[:s["lsz"]]
    open(out_path, "wb").write(data)
    got = sum16(data)
    print("unpack %s: %d bytes -> %s  checksum %04x (table %04x) %s"
          % (s["name"], len(data), out_path, got, s["cks"],
             "OK" if (s["cks"] in (0, got)) else "MISMATCH"))
    return 0

def cmd_repack(in_path, out_path):
    """Compress a section body to a UZIP blob and print its size + table checksum."""
    data = open(in_path, "rb").read()
    blob = uzip_compress(data)
    assert uzip_decompress(blob) == data, "UZIP round-trip failed"
    open(out_path, "wb").write(blob)
    print("repack: %d -> %d bytes (UZIP) -> %s" % (len(data), len(blob), out_path))
    print("  section-table fields: lsz=0x%x rsz=0x%x cksum16=0x%04x flags=0x0005"
          % (len(data), len(blob), sum16(data)))
    return 0

def _ap_fields(d):
    return dict(magic0=be32(d,0), magic1=be32(d,4), force=be32(d,OFF_FORCE),
                size=be32(d,OFF_SIZE), cken=be32(d,OFF_CKEN), pkver=be32(d,OFF_PKVER),
                chip=be32(d,OFF_CHIP), cksum=be16(d,OFF_CKSUM))

def cmd_apinfo(path):
    d = open(path, "rb").read()
    if len(d) < AP_HDR_LEN:
        print("FAIL: shorter than the 0x200 header"); return 1
    f = _ap_fields(d)
    sig = d[0:8]
    size = f["size"]
    ok = True
    def chk(cond, msg):
        nonlocal ok
        ok = ok and cond
        print(("  OK   " if cond else "  FAIL ") + msg)
    print("file %s (%d bytes)" % (path, len(d)))
    print("  signature: %r" % sig)
    chk(f["magic0"] == AP_MAGIC0, "magic0 0x%08x (want 0x%08x 'CT90')" % (f["magic0"], AP_MAGIC0))
    chk(f["magic1"] == AP_MAGIC1, "magic1 0x%08x (want 0x%08x '9-AP')" % (f["magic1"], AP_MAGIC1))
    chk(f["chip"] in (0x41, 0x01), "chip/auto-upgrade code 0x%x (want 0x41=952A or 0x01)" % f["chip"])
    chk(0 < size <= AP_MAX_SIZE, "size 0x%x (want 1..0x%x)" % (size, AP_MAX_SIZE))
    chk(size % 4 == 0, "size 4-byte aligned")
    chk(f["pkver"] >= 5, "APPacker version %d (want >= 5)" % f["pkver"])
    print("  force=%d checksum-enabled=%d" % (f["force"], f["cken"]))
    if size and size <= len(d):
        body = sum16(d[AP_HDR_LEN:size])
        chk(body == f["cksum"], "body checksum 0x%04x @0x2e (computed 0x%04x over [0x200:0x%x))"
            % (f["cksum"], body, size))
    else:
        print("  FAIL body checksum: size 0x%x exceeds file length %d" % (size, len(d))); ok = False
    print("=> %s" % ("VALID" if ok else "INVALID -- the device would reject this"))
    return 0 if ok else 1

def cmd_apfix(path):
    d = bytearray(open(path, "rb").read())
    size = len(d)
    if size % 4:                                   # checksum reads whole words
        d += b"\x00" * (4 - size % 4); size = len(d)
    struct.pack_into(">I", d, OFF_SIZE, size)
    struct.pack_into(">I", d, OFF_CKEN, 1)
    struct.pack_into(">H", d, OFF_CKSUM, sum16(d[AP_HDR_LEN:size]))
    open(path, "wb").write(d)
    print("apfix: size=0x%x checksum=0x%04x written to %s" % (size, be16(d, OFF_CKSUM), path))

def cmd_apwrap(body_path, out_path, code=0x41):
    body = open(body_path, "rb").read()
    hdr = bytearray(AP_HDR_LEN)
    struct.pack_into(">I", hdr, 0x00, AP_MAGIC0)
    struct.pack_into(">I", hdr, 0x04, AP_MAGIC1)
    struct.pack_into(">I", hdr, OFF_FORCE, 1)
    struct.pack_into(">I", hdr, OFF_CKEN, 1)
    struct.pack_into(">I", hdr, OFF_PKVER, 5)
    struct.pack_into(">I", hdr, OFF_CHIP, code)
    d = bytearray(hdr) + bytearray(body)
    if len(d) % 4:
        d += b"\x00" * (4 - len(d) % 4)
    struct.pack_into(">I", d, OFF_SIZE, len(d))
    struct.pack_into(">H", d, OFF_CKSUM, sum16(d[AP_HDR_LEN:len(d)]))
    if len(d) > AP_MAX_SIZE:
        print("WARNING: size 0x%x exceeds reserved 0x%x (needs force flag / will be refused)"
              % (len(d), AP_MAX_SIZE))
    open(out_path, "wb").write(d)
    print("apwrap: wrote %s (0x%x bytes, chip=0x%x, checksum=0x%04x)"
          % (out_path, len(d), code, be16(d, OFF_CKSUM)))
    print("NOTE: the body must be a valid self-flashing AP; this only builds the container.")

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    cmd, rest = argv[1], argv[2:]
    if cmd == "sections": return cmd_sections(rest[0])
    if cmd == "unpack":   return cmd_unpack(rest[0], rest[1], rest[2])
    if cmd == "repack":   return cmd_repack(rest[0], rest[1])
    if cmd == "apinfo":   return cmd_apinfo(rest[0])
    if cmd == "apfix":    return cmd_apfix(rest[0])
    if cmd == "apwrap":
        code = 0x41
        if "--code" in rest:
            code = int(rest[rest.index("--code")+1], 0); rest = rest[:rest.index("--code")]
        return cmd_apwrap(rest[0], rest[1], code)
    print("unknown command %r" % cmd); return 2

if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
