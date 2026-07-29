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
  ctkap.py mkflasher <out.AP> --write DST:img [--write ...] [--code 0x41] [--stub f]
                                            # build a self-flashing AP: a SPARC stub
                                            # that loops WriteSPF over carried images

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

# ---- AP_INFO update-container header (512 bytes), all big-endian -------------
# Field layout is the firmware's own `AP_INFO` struct (aploader.h), and every
# requirement is the loader's own source (AP_Identify/AP_Loader in aploader.c),
# cross-checked against dp700wd.bin: chip code 0x41 (IC_VERSION_952A, Winav.h) is
# the constant the binary's validator compares; the AP body is copied to
# DS_AP_CODE_AREA = 0x4009a000 (verified: 13 sethi hits in the binary; the repo's
# dvd_dram_16m.h 0x4008b000 is a different build variant); the checksum is
# HAL_CheckSum (hsystem.c) -- an additive byte-sum into a 16-bit WORD.
AP_MAGIC0 = 0x43543930   # "CT90"  (CT909AP_IDENTIFY1)
AP_MAGIC1 = 0x392d4150   # "9-AP"  (CT909AP_IDENTIFY2) -> first 8 bytes = "CT909-AP"
AP_HDR_LEN   = 0x200      # sizeof(AP_INFO)
AP_MAX_SIZE  = 0x166000   # reserved AP code area (DRAM end - DS_AP_CODE_AREA)
OFF_APTYPE   = 0x08       # u32 dwAP_Type       (AP id; AP_AUTO_UPGRADE = 1)
OFF_SIZE     = 0x0c       # u32 dwAP_Size       (total, incl. header; <= AP_MAX_SIZE, 4-aligned)
OFF_EXTFLAG  = 0x10       # u32 dwExternalFlag  (TRUE => body checksum verified + section-loaded)
OFF_PKVER    = 0x14       # u32 dwVersionAP     (APPacker version, must be > 4)
OFF_CHIP     = 0x18       # u32 dwChipVersion   (== IC_VERSION_ID 0x41=952A, or 0x01 reserved)
OFF_CKSUM    = 0x2e       # low WORD of u32 dwCheckSum @0x2c = HAL_CheckSum(body[0x200:size])
OFF_AP_SP    = 0x30       # u32 dwAP_SP         (stack pointer for the loaded AP; NOT an entry)
OFF_UNZIP    = 0x34       # u32 dwAP_UNZIP_BUF  (UZIP work buffer for section decompression)

# ---- self-flashing AP body --------------------------------------------------
# NOTE ON FORMAT (per aploader.c): a REAL loader-accepted AP body is a *section-
# table image* -- AP_Info(0x200) + [image header 0x10][section table] + compressed
# sections. AP_Loader copies it to DS_AP_CODE_AREA (0x4009a000), checksums it, then
# ROMLD_BOOT_LoadSectionAndRun DECOMPRESSES the AP's sections into DRAM and runs it
# (so the AP's flash driver runs from DRAM -- the "DRAM-driver relocation" is inherent
# to this format). The `mkflasher` body below is instead a RAW code stub for the
# emulator's direct-jump harness (--aprun / --apflash), which proves the flash
# mechanism but is NOT what the on-device loader section-loads. A loader-compatible
# AP needs the section-table wrapper (future `mksectionap`).
#
# APSTUB_BIN is a tiny SPARC V8 (big-endian) stub (source: apstub.S) whose _start
# sits at the body base. It reads a descriptor at body+0x100 and loops the firmware's
# XIP WriteSPF (flash 0x3d0fc) over a carried image, 64 KB/call, then signals + halts.
#   +0x100  u32 nchunks
#   +0x104  nchunks * { u32 dstFlashAddr; u32 srcBodyOff; u32 size }
#   +....   image bytes (srcBodyOff is a byte offset within the body)
AP_BODY_DRAM = 0x4009a000
STUB_MAX     = 0x100      # descriptor starts here; stub code must fit below it

# ---- section-table AP (loader-compatible) -----------------------------------
# A real AP the on-device loader accepts: AP_INFO(0x200) + [AP image header 0x10]
# + [section table, 32 SECTION_ENTRY] + sections. AP_Loader copies it to
# AP_BODY_DRAM, ROMLD_MoveSectionTable relocates the table to AP_TABLE_ADDRESS
# (adding src-dest to every dwRMA), then ROMLD_BOOT_LoadSectionAndRun (@0x4bc ->
# 0x528) loads the Load-flagged sections to their LMAs and jumps to the
# Load|ProgEntry section -- so the flasher runs from DRAM (the reloc, for free).
AP_TABLE_ADDRESS = 0x40000800   # aploader.h; verified 6x in the binary
AP_IMG_HDR   = 0x10             # ROMLD_SECTION_TABLE_ADDR: table starts image+0x10
NSEC         = 32               # ROMLD_SECTION_TABLE_SIZE
SEC_ENTRY    = 24               # sizeof(SECTION_ENTRY)
SEC_TBL_OFF  = AP_HDR_LEN + AP_IMG_HDR              # 0x210 (file offset of the table)
CONTENT_OFF  = SEC_TBL_OFF + NSEC * SEC_ENTRY       # 0x510 (sections start after 32 entries)
FLSH_LMA     = 0x40500000       # runtime-free high DRAM; flasher app runs here
FLSH_AP_SP   = 0x405f0000       # dwAP_SP for the loaded AP (below IMAG_LMA_BASE)
RUN_AP_SP    = 0x407f0000       # dwAP_SP for a run-AP (transient; start_app.S sets its own)
IMAG_LMA_BASE= 0x40600000       # decompressed --image section(s) land here
FLSH_UNZIP_BUF = 0x40780000     # dwAP_UNZIP_BUF: UZIP work buffer for section decompression
SEC_FLAG_LOAD, SEC_FLAG_PROGENTRY, SEC_FLAG_ZIP = 1, 2, 4
# the section flasher app (source: apstub_sec.S), linked/loaded at FLSH_LMA
APSTUB_SEC_BIN = bytes.fromhex(
    "21101400e2042100a404210480a460000280002901000000e604a000d004a004"
    "ea04a008a8020010031000cd82106323c208400080a060041280000f01000000"
    "ac102000912da00c9004c0080310007f821062a89fc0400001000000ac05a001"
    "80a5a01006bffff8010000001080000701000000901000130310007f821062a8"
    "9fc0400001000000901000149205001594100013031000808210603c9fc04000"
    "01000000a404a00ca2a4600112bfffdb010000000320001f821063f4053037b4"
    "8410a00dc4204000a1480000a02c2020818c2000010000000100000001000000"
    "91d0200001000000")
APSTUB_BIN = bytes.fromhex(
    "21100268e2042100a404210480a460000280000e01000000d004a000d204a004"
    "d404a00892024010030000f4821060fc9fc0400001000000a404a00ca2a46001"
    "12bffff6010000000320001f821063f4053037b48410a00dc420400091d020000"
    "1000000")

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
    return dict(magic0=be32(d,0), magic1=be32(d,4), aptype=be32(d,OFF_APTYPE),
                size=be32(d,OFF_SIZE), extflag=be32(d,OFF_EXTFLAG), pkver=be32(d,OFF_PKVER),
                chip=be32(d,OFF_CHIP), cksum=be16(d,OFF_CKSUM), cksum_hi=be16(d,0x2c),
                ap_sp=be32(d,OFF_AP_SP), unzip=be32(d,OFF_UNZIP))

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
    chk(f["chip"] in (0x41, 0x01), "dwChipVersion 0x%x (want 0x41=IC_VERSION_952A, or 0x01 reserved)" % f["chip"])
    chk(0 < size <= AP_MAX_SIZE, "dwAP_Size 0x%x (want 1..0x%x)" % (size, AP_MAX_SIZE))
    chk(size % 4 == 0, "dwAP_Size 4-byte aligned (HAL_CheckSum steps by DWORD)")
    chk(f["pkver"] > 4, "dwVersionAP %d (want > 4)" % f["pkver"])
    print("  dwAP_Type=%d (auto-upgrade=1) dwExternalFlag=%d dwAP_SP=0x%08x dwAP_UNZIP_BUF=0x%08x"
          % (f["aptype"], f["extflag"], f["ap_sp"], f["unzip"]))
    if f["extflag"] and size and size <= len(d):
        body = sum16(d[AP_HDR_LEN:size])
        chk(body == f["cksum"] and f["cksum_hi"] == 0,
            "dwCheckSum 0x%04x @0x2e (HAL_CheckSum computed 0x%04x over [0x200:0x%x); hi word 0x%04x)"
            % (f["cksum"], body, size, f["cksum_hi"]))
    elif not f["extflag"]:
        print("  --   dwExternalFlag=0: body checksum not verified by the loader")
    else:
        print("  FAIL body checksum: size 0x%x exceeds file length %d" % (size, len(d))); ok = False
    print("=> %s" % ("VALID" if ok else "INVALID -- the device would reject this"))
    return 0 if ok else 1

def cmd_apfix(path):
    d = bytearray(open(path, "rb").read())
    size = len(d)
    if size % 4:                                   # HAL_CheckSum steps by DWORD
        d += b"\x00" * (4 - size % 4); size = len(d)
    struct.pack_into(">I", d, OFF_SIZE, size)
    struct.pack_into(">I", d, OFF_EXTFLAG, 1)      # dwExternalFlag=TRUE => checksum verified
    struct.pack_into(">I", d, 0x2c, sum16(d[AP_HDR_LEN:size]))  # dwCheckSum (hi word 0)
    open(path, "wb").write(d)
    print("apfix: dwAP_Size=0x%x dwCheckSum=0x%04x written to %s" % (size, be16(d, OFF_CKSUM), path))

def cmd_apwrap(body_path, out_path, code=0x41):
    body = open(body_path, "rb").read()
    d = _ap_finalize(bytearray(AP_HDR_LEN) + bytearray(body), code, 0)
    if len(d) > AP_MAX_SIZE:
        print("WARNING: size 0x%x exceeds reserved 0x%x (dwAP_Size check will refuse it)"
              % (len(d), AP_MAX_SIZE))
    open(out_path, "wb").write(d)
    print("apwrap: wrote %s (0x%x bytes, dwChipVersion=0x%x, dwCheckSum=0x%04x)"
          % (out_path, len(d), code, be16(d, OFF_CKSUM)))
    print("NOTE: a loader-accepted body is a section-table image (see the format header);"
          " this only builds the AP_INFO container + checksum.")

def _ap_finalize(d, code, ap_sp):
    """Fill AP_INFO header (magic/type/flag/ver/chip/sp), pad to 4, set size + checksum."""
    struct.pack_into(">I", d, 0x00, AP_MAGIC0)
    struct.pack_into(">I", d, 0x04, AP_MAGIC1)
    struct.pack_into(">I", d, OFF_APTYPE, 1)       # dwAP_Type = AP_AUTO_UPGRADE
    struct.pack_into(">I", d, OFF_EXTFLAG, 1)      # dwExternalFlag = TRUE (checksum + section-load)
    struct.pack_into(">I", d, OFF_PKVER, 5)        # dwVersionAP (> 4)
    struct.pack_into(">I", d, OFF_CHIP, code)      # dwChipVersion (0x41 = IC_VERSION_952A)
    struct.pack_into(">I", d, OFF_AP_SP, ap_sp)    # dwAP_SP (loaded-AP stack pointer)
    if len(d) % 4:
        d += b"\x00" * (4 - len(d) % 4)
    struct.pack_into(">I", d, OFF_SIZE, len(d))
    struct.pack_into(">I", d, 0x2c, sum16(d[AP_HDR_LEN:len(d)]))  # dwCheckSum (hi word 0)
    return d

def cmd_mkflasher(out_path, writes, code=0x41, stub_path=None):
    """Build a self-flashing UPG952A.AP: stub + descriptor + carried image(s).

    `writes` is a list of (dstFlashAddr, image_path). Each image is written to
    flash starting at dstFlashAddr (64 KB-aligned), split into <=64 KB chunks at
    consecutive 64 KB sectors -- exactly what the stub feeds to WriteSPF.
    """
    stub = open(stub_path, "rb").read() if stub_path else APSTUB_BIN
    if len(stub) > STUB_MAX:
        print("stub too big (0x%x > 0x%x)" % (len(stub), STUB_MAX)); return 1

    # Flatten every --write into 64 KB WriteSPF chunks.
    chunks = []                      # (dstFlashAddr, data)
    for dst, ipath in writes:
        if dst & 0xFFFF:
            print("--write dst 0x%x not 64 KB-aligned" % dst); return 1
        data = open(ipath, "rb").read()
        for off in range(0, len(data), 0x10000):
            chunks.append((dst + off, data[off:off + 0x10000]))

    n = len(chunks)
    img_off = STUB_MAX + 4 + n * 12               # image bytes follow the table
    table, image = bytearray(), bytearray()
    touches_boot = False
    for dst, data in chunks:
        table += struct.pack(">III", dst, img_off + len(image), len(data))
        image += data
        if dst < 0x10000:
            touches_boot = True

    body = bytearray(STUB_MAX)
    body[:len(stub)] = stub
    body += struct.pack(">I", n) + table + image

    d = _ap_finalize(bytearray(AP_HDR_LEN) + body, code, AP_BODY_DRAM)
    if len(d) > AP_MAX_SIZE:
        print("WARNING: size 0x%x exceeds reserved AP area 0x%x (force flag set; a real "
              "loader may still refuse)" % (len(d), AP_MAX_SIZE))
    open(out_path, "wb").write(d)
    print("mkflasher: wrote %s (0x%x bytes, chip=0x%x, entry=0x%08x, checksum=0x%04x)"
          % (out_path, len(d), code, AP_BODY_DRAM, be16(d, OFF_CKSUM)))
    print("  %d WriteSPF chunk(s):" % n)
    for dst, data in chunks:
        print("    flash[0x%06x .. 0x%06x)  %u B" % (dst, dst + 0x10000, len(data)))
    if touches_boot:
        print("  NOTE: sector 0 (0x0..0x10000) is rewritten -- it holds the reset vector"
              " + section table; this is expected for a full-image reflash.")
    print("  verify with:  ctkap.py apinfo %s" % out_path)
    return 0

def cmd_mksectionap(out_path, writes, images, code=0x41, stub_path=None):
    """Build a LOADER-COMPATIBLE self-flashing AP (a section-table image).

    The flasher app (apstub_sec.S) is section FLSH (Load|ProgEntry) at FLSH_LMA; it
    carries a chunk descriptor and reflashes via the resident DRAM driver. Each
    --write DST:img is a small raw payload embedded in FLSH; each --image DST:img is
    a LARGE payload shipped as its own UZIP-compressed section (IMGn, Load|ZIP) that
    the loader decompresses to a DRAM LMA -- so a full image fits under the AP cap
    and the flasher writes the decompressed bytes. Chunk src offsets are relative to
    FLSH_LMA (the flasher adds FLSH_LMA), so a chunk pointing into IMGn uses
    (IMGn_LMA - FLSH_LMA) + offset.
    """
    stub = open(stub_path, "rb").read() if stub_path else APSTUB_SEC_BIN
    if len(stub) > STUB_MAX:
        print("flasher app too big (0x%x > 0x%x)" % (len(stub), STUB_MAX)); return 1

    # flatten --write into raw 64 KB chunks (bytes embedded in FLSH content)
    raw_chunks = []
    for dst, ip in writes:
        if dst & 0xFFFF:
            print("--write dst 0x%x not 64 KB-aligned" % dst); return 1
        data = open(ip, "rb").read()
        for off in range(0, len(data), 0x10000):
            raw_chunks.append((dst + off, data[off:off + 0x10000]))

    # each --image becomes its own IMGn section (compressed), decompressed to imag_lma
    img_specs, imag_lma = [], IMAG_LMA_BASE
    for dst, ip in images:
        if dst & 0xFFFF:
            print("--image dst 0x%x not 64 KB-aligned" % dst); return 1
        data = open(ip, "rb").read()
        img_specs.append((dst, imag_lma, data))
        imag_lma = (imag_lma + len(data) + 0xFFFF) & ~0xFFFF   # next IMGn LMA (64 KB-aligned)

    nchunks = len(raw_chunks) + sum((len(d) + 0xFFFF) // 0x10000 for _, _, d in img_specs)
    raw_base = STUB_MAX + 4 + nchunks * 12            # raw --write bytes follow the table
    table, raw = bytearray(), bytearray()
    for dst, data in raw_chunks:                       # raw chunks: src within FLSH content
        table += struct.pack(">III", dst, raw_base + len(raw), len(data))
        raw += data
    for dst, ilma, data in img_specs:                  # image chunks: src within IMGn (@ its LMA)
        for off in range(0, len(data), 0x10000):
            n = min(0x10000, len(data) - off)
            table += struct.pack(">III", dst + off, (ilma - FLSH_LMA) + off, n)
    content = bytearray(STUB_MAX); content[:len(stub)] = stub
    content += struct.pack(">I", nchunks) + table + raw
    if len(content) % 4:
        content += b"\x00" * (4 - len(content) % 4)

    # section table (32 entries): entry 0 = FLSH, then one IMGn per --image, then 0.
    # dwRMA is IMAGE-RELATIVE (loader resolves dwRMA + pSecTbl - 0x10 after
    # MoveSectionTable rebases it; same convention as the main image at table 0x10).
    sectbl = bytearray(NSEC * SEC_ENTRY)
    body = bytearray()          # section contents, placed at file offset CONTENT_OFF
    secs = []                   # for the report

    def put_section(idx, name, lma, blob, lsz, cks, flags):
        rma = (CONTENT_OFF + len(body)) - AP_HDR_LEN
        struct.pack_into(">IIIIII", sectbl, idx * SEC_ENTRY, name, lma, rma, lsz, len(blob),
                         (cks << 16) | flags)
        secs.append((idx, name, lma, rma, lsz, len(blob), flags))
        body.extend(blob)
        if len(body) % 4:
            body.extend(b"\x00" * (4 - len(body) % 4))

    put_section(0, 0x464C5348, FLSH_LMA, content, len(content), sum16(content),
                SEC_FLAG_LOAD | SEC_FLAG_PROGENTRY)                          # FLSH (raw)
    for i, (dst, ilma, data) in enumerate(img_specs):
        comp = uzip_compress(data)
        name = int.from_bytes(("IMG%d" % i).encode(), "big")
        put_section(1 + i, name, ilma, comp, len(data), sum16(data),
                    SEC_FLAG_LOAD | SEC_FLAG_ZIP)                            # IMGn (UZIP)

    d = bytearray(AP_HDR_LEN + AP_IMG_HDR + NSEC * SEC_ENTRY) + body
    d[SEC_TBL_OFF:SEC_TBL_OFF + len(sectbl)] = sectbl
    d = _ap_finalize(d, code, FLSH_AP_SP)
    struct.pack_into(">I", d, OFF_UNZIP, FLSH_UNZIP_BUF)   # dwAP_UNZIP_BUF (header; not in checksum)
    if len(d) > AP_MAX_SIZE:
        print("WARNING: size 0x%x exceeds reserved AP area 0x%x" % (len(d), AP_MAX_SIZE))
    open(out_path, "wb").write(d)
    print("mksectionap: wrote %s (0x%x bytes, chip=0x%x, dwAP_SP=0x%08x, dwAP_UNZIP_BUF=0x%08x, dwCheckSum=0x%04x)"
          % (out_path, len(d), code, FLSH_AP_SP, FLSH_UNZIP_BUF, be16(d, OFF_CKSUM)))
    for idx, name, lma, rma, lsz, rsz, flags in secs:
        fl = "|".join(f for b, f in ((1, "Load"), (2, "ProgEntry"), (4, "ZIP")) if flags & b)
        print("  sec[%d] %-4s lma=0x%08x rma=0x%06x lsz=0x%x rsz=0x%x flags=%s"
              % (idx, name.to_bytes(4, "big").decode("latin1"), lma, rma, lsz, rsz, fl))
    print("  %d WriteSPF chunk(s):" % nchunks)
    print("  run through the real loader:  ct952emu dp700wd.bin --rom-load --apload %s" % out_path)
    return 0

def cmd_mkrunap(out_path, payload_path, lma=FLSH_LMA, code=0x41):
    """Build a NON-DESTRUCTIVE run-AP: a section-table AP whose single ProgEntry
    section IS the payload (e.g. MicroPython), UZIP-compressed. The on-device
    loader decompresses it to `lma` and jumps in -- it RUNS from DRAM and writes
    NOTHING to flash, so it's brick-safe and a reboot restores stock firmware.
    The payload must have its entry at offset 0 of `lma` (start_app.S does)."""
    data = open(payload_path, "rb").read()
    comp = uzip_compress(data)
    zipped = len(comp) < len(data)
    blob = comp if zipped else data
    flags = SEC_FLAG_LOAD | SEC_FLAG_PROGENTRY | (SEC_FLAG_ZIP if zipped else 0)
    rma = CONTENT_OFF - AP_HDR_LEN                     # image-relative (loader adds pSecTbl-0x10)
    sectbl = bytearray(NSEC * SEC_ENTRY)
    struct.pack_into(">IIIIII", sectbl, 0,
                     0x4d505920,                        # 'MPY '
                     lma, rma, len(data), len(blob), (sum16(data) << 16) | flags)
    d = bytearray(AP_HDR_LEN + AP_IMG_HDR + NSEC * SEC_ENTRY)
    d[SEC_TBL_OFF:SEC_TBL_OFF + len(sectbl)] = sectbl
    d += blob
    d = _ap_finalize(d, code, RUN_AP_SP)
    struct.pack_into(">I", d, OFF_UNZIP, FLSH_UNZIP_BUF)
    if len(d) > AP_MAX_SIZE:
        print("WARNING: AP size 0x%x exceeds reserved 0x%x -- the loader will refuse it"
              % (len(d), AP_MAX_SIZE))
    open(out_path, "wb").write(d)
    print("mkrunap: wrote %s (0x%x bytes, chip=0x%x, dwAP_SP=0x%08x, dwCheckSum=0x%04x)"
          % (out_path, len(d), code, RUN_AP_SP, be16(d, OFF_CKSUM)))
    print("  sec[0] MPY  lma=0x%08x rma=0x%06x lsz=0x%x rsz=0x%x flags=%s"
          % (lma, rma, len(data), len(blob),
             "Load|ProgEntry" + ("|ZIP" if zipped else "")))
    print("  NON-DESTRUCTIVE: runs from DRAM, writes no flash. Verify:")
    print("    ct952emu dp700wd.bin --rom-load --apload %s" % out_path)
    return 0

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
    if cmd == "mkrunap":
        code, lma, args = 0x41, FLSH_LMA, []
        i = 0
        while i < len(rest):
            if rest[i] == "--code": code = int(rest[i+1], 0); i += 2
            elif rest[i] == "--lma": lma = int(rest[i+1], 0); i += 2
            else: args.append(rest[i]); i += 1
        if len(args) < 2:
            print("usage: mkrunap <out.AP> <payload.bin> [--lma 0x40500000] [--code 0x41]")
            return 2
        return cmd_mkrunap(args[0], args[1], lma, code)
    if cmd == "mkflasher":
        code, stub, writes, out = 0x41, None, [], None
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--code":   code = int(rest[i+1], 0); i += 2
            elif a == "--stub": stub = rest[i+1]; i += 2
            elif a == "--write":
                dst, ip = rest[i+1].split(":", 1)
                writes.append((int(dst, 0), ip)); i += 2
            elif out is None:   out = a; i += 1
            else:               print("unexpected arg %r" % a); return 2
        if not out or not writes:
            print("usage: mkflasher <out.AP> --write DST:img [--write ...] [--code C] [--stub f]")
            return 2
        return cmd_mkflasher(out, writes, code, stub)
    if cmd in ("mksectionap", "mksecap"):
        code, stub, writes, images, out = 0x41, None, [], [], None
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--code":   code = int(rest[i+1], 0); i += 2
            elif a == "--stub": stub = rest[i+1]; i += 2
            elif a == "--write":
                dst, ip = rest[i+1].split(":", 1)
                writes.append((int(dst, 0), ip)); i += 2
            elif a == "--image":
                dst, ip = rest[i+1].split(":", 1)
                images.append((int(dst, 0), ip)); i += 2
            elif out is None:   out = a; i += 1
            else:               print("unexpected arg %r" % a); return 2
        if not out or not (writes or images):
            print("usage: mksectionap <out.AP> [--write DST:img] [--image DST:img] [--code C] [--stub f]")
            return 2
        return cmd_mksectionap(out, writes, images, code, stub)
    print("unknown command %r" % cmd); return 2

if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
