#!/usr/bin/env python3
"""
ctkap.py -- CheerTek CT909/CT952A flash-image + UPG952A.AP tool.

Reverse-engineered from dp700wd.bin (the CT952A firmware): the section-table
layout, the per-section 16-bit byte-sum checksums, and the CT909-AP update
container header (magic, chip code, size, APPacker version, body checksum).
Every field and checksum below is verified against the firmware's own loader
(validator @0x4170, checksum routine @0x40320) -- see CT952A_FLASH_FORMAT.md.

  ctkap.py sections  <flash.bin>            # dump the section table + verify raw checksums
  ctkap.py apinfo    <UPG952A.AP>           # parse + validate a CT909-AP update file
  ctkap.py apfix     <UPG952A.AP>           # recompute size(0x0c)+checksum(0x2e) in place
  ctkap.py apwrap    <body.bin> <out.AP> [--code 0x41]   # wrap a raw AP body into a .AP

WARNING: reflashing is irreversible-ish and can brick hardware. This tool builds
and *validates* images; it does not talk to a device. Always `apinfo` a file and
confirm every field before it ever touches a real unit.
"""
import sys, struct

def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def sum16(b):  return sum(b) & 0xFFFF          # the firmware's only checksum kind

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
        else:
            chk = "(unpacked)"                        # zipped: checksum is over unpacked data
        print("  %-4s  %08x   %08x   %8x   %8x   %04x  %04x   %s"
              % (nm.decode('latin1'), lma, rma, lsz, rsz, cks, flags, chk))
        off += 24
    print("[note] raw-section checksums are the 16-bit byte-sum of the (unpacked==flash) "
          "data; compressed sections' checksums cover the *unpacked* bytes.")

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
