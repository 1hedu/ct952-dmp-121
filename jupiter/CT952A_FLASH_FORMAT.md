# CT952A flash-image & `UPG952A.AP` update format

Reverse-engineered from `jupiter/emu/dp700wd.bin` (CheerTek CT952A DVD firmware,
big-endian SPARC V8, 2 MB XIP NOR flash). Every field and checksum here is
**verified against the firmware's own code** — the section loader
(`rom_load_pass` in the emulator mirrors the mask-ROM), the AP-header validator
at flash `0x4170`, and the checksum routine at `0x40320`. Two independent
disassembly passes agreed field-for-field, and the magic/checksum/chip-code
instructions were re-verified by hand.

> ⚠️ Reflashing a real unit is delicate and can brick it. Use `ctkap.py apinfo`
> to validate any `UPG952A.AP` and confirm **every** field before it touches
> hardware. The good news: the updater only rewrites the AP area (≤ `0x166000`),
> leaving the boot loader intact, so a bad AP is normally recoverable.

Tool: `jupiter/tools/ctkap.py` (`sections` / `apinfo` / `apfix` / `apwrap`).

---

## 1. Full-flash image layout (`dp700wd.bin`)

### Image header (16 bytes @ `0x0`)
| off | value in this image | meaning |
|---|---|---|
| `0x00` | `0x81C02310` | reset vector (mask-ROM entry) |
| `0x04` | `0x01000000` | version/flags |
| `0x08` | `0x00000F6C` | → offset of the boot-ROM config block (`@CTKDRAM`/`@CTKPROM`) |
| `0x0C` | `0x000004BC` | → offset of a boot code entry (`save` prologue) |

### Section table (24-byte entries @ `0x10`, ends at first non-ASCII name)
```
+0x00  char name[4]      ROMV TEXT DATA ENGL SFAT TEX2 RODA SETD UZIP LOGO CUST ...
+0x04  u32  lma          load/run address (BE). >=0x40000000 => staged into DRAM; else XIP/data
+0x08  u32  rma          flash offset of the section's bytes (BE)
+0x0C  u32  lsz          unpacked size
+0x10  u32  rsz          packed size in flash.  rsz < lsz  =>  UZIP-compressed; else raw copy
+0x14  u32  = (cksum16 << 16) | flags16
```
* **cksum16** = `(Σ bytes of the *unpacked* section) & 0xFFFF`. For raw sections the
  unpacked bytes == the flash bytes; for compressed sections it is over the
  decompressed image. `0x0000` = "no checksum" (the mutable `SETD` settings sector
  and the `COPY` region).
* **flags16**: `0x0000` raw, `0x0004`/`0x0005`/`0x0007` = UZIP-compressed variants.

Then the boot-ROM config block (`@CTKDRAM 0108011b`, `@CTKPROM` + the register
init writes that print as `<P1 Booting> Write X to Y`), followed by each
section's payload at its `rma`.

The boot ROM loads sections whose `lma >= 0x40000000` into DRAM (decompressing
when `rsz < lsz`), leaves XIP/data sections in place, and jumps to `ROMV`'s `lma`.

### UZIP section compression (`rsz < lsz`)

**UZIP is plain LZMA1** (props `0x63` ⇒ **lc=0, lp=1, pb=2**) inside a custom
**13-byte XOR-obfuscated container**. The decompressor is at flash `0x2C50`
(wrapper) → parser `0x2028` → LZMA range-decoder `0x20b4`. Independently
confirmed: Python `lzma`/system liblzma decode the firmware's stream to the
identical bytes, and a re-encode reproduces near-OEM size.

Container (the `rsz` bytes stored at `rma`):
```
+0x00  u32 word0 ^ 0x5A5A5A5A   deobf = 0x63000080  (high byte 0x63 = LZMA props)
+0x04  u32 word1 ^ 0x5A5A5A5A   \  uncompressed size, 32-bit, byte-shuffled:
+0x08  u32 word2 ^ 0x5A5A5A5A   /   size = ((w1>>16)&0xff) | (w1&0xff00)
                                          | ((w1&0xff)<<16) | (w2&0xff000000)
+0x0C  byte 0x5A                 (unused)
+0x0D  ...                       raw LZMA1 stream (no .lzma/xz framing)
```
The decoder uses the **output buffer as its window**, so re-compressing with any
`dict_size >= uncompressed_size` (and no end marker; it stops at the header size)
produces a stream the firmware accepts. `ctkap.py` does exactly this via Python
`lzma` FORMAT_RAW, and `ctkap.py sections` decompresses every UZIP section and
checks its 16-bit byte-sum against the section table.

A modified section may be shipped **raw** (flags `0x0000`, `rsz==lsz`) or
**re-UZIP'd** (`ctkap.py repack`, flags `0x0005`) — raw is simplest, UZIP keeps
the image within the `0x166000` AP-area cap.

---

## 2. `UPG952A.AP` update container

`UPG952A.AP` is **not** a raw flash image — it is a **RAM-boot AP**: a `CT909-AP`
header + an AP body. The running firmware opens `/UPG952A.AP` at the root of a
mounted **FAT USB or SD** volume (`OpenUpgradeFile @0x254a4`; menu-driven, via the
update mode — not a background insert scan), hands it to the **AP loader
(`0x3e48`)** — the same routine the boot uses — which validates the header, copies
the body to DRAM `0x4009a000`, verifies the body checksum, and **jumps into the
AP**. That loaded AP performs the persistent serial-flash reprogram of the AP
area (driver primitives at `0x3d0fc` / `0x3d1cc`, 64 KB-aligned erase-then-program).

### Header (512 bytes = `0x200`, big-endian) — what the loader enforces
| off | size | field | requirement | proof |
|---|---|---|---|---|
| `0x00` | u32 | magic0 | `0x43543930` (`"CT90"`) | `0x4260`–`0x426c` |
| `0x04` | u32 | magic1 | `0x392d4150` (`"9-AP"`) → 8 bytes = `CT909-AP` | `0x4274`–`0x4280` |
| `0x08` | u32 | force flag | `1` ⇒ allow oversize + reboot-on-fail | `0x4304` |
| `0x0C` | u32 | AP total size | `≤ 0x166000`, 4-byte aligned | `0x42f0`–`0x42fc` |
| `0x10` | u32 | checksum-enable | `1` ⇒ body checksum verified | `0x4054`–`0x405c` |
| `0x14` | u32 | APPacker version | `≥ 5` (`> 4`) | `0x4324`–`0x432c` |
| `0x18` | u32 | chip / auto-upgrade code | **`0x41`** (952**A**, `'A'`) or `0x01` (universal) | `0x41dc`–`0x41e8`, helper `0x414c` |
| `0x2E` | u16 | body checksum | `(Σ bytes[0x200 .. size]) & 0xFFFF` | `0x4090`–`0x4098`, routine `0x40320` |
| `0x30`,`0x34` | u32 | AP entry / launch params | consumed at jump | `0x412c`/`0x4138` |
| `0x1C`–`0x2D`, `0x36`–`0x1FF` | — | copied but not read by this layer | undetermined | — |

Device-side (not file fields): DRAM-type detect (`0x40260`) must not be the
`0x50000000` "unknown" sentinel; the "now" chip code compared at `0x18` is the
hard-wired constant `0x41`.

### The two checksums — both plain 16-bit additive byte-sums (NOT CRCs)
1. **Per flash section**: `Σ(unpacked bytes) & 0xFFFF` in section-table entry `+0x14`.
2. **AP body**: `Σ(file bytes 0x200 .. size) & 0xFFFF`, big-endian at header `0x2E`,
   verified only when header `0x10 == 1`. Seed 0, no polynomial, no reflection.
   Word-stepped, so **`size` must be 4-byte aligned**; the 512-byte header is excluded.

### Flash write range / recoverability
The reserved **AP code area is `0x166000` bytes** and the size check bounds the AP to
it, so only the AP region is erased+programmed (64 KB-aligned sectors). The boot
loader / low flash is **preserved**, so a failed AP flash reboots into the loader
and can be retried. (The exact AP start offset + full erase list execute inside the
loaded AP, which lives in the UZIP-compressed `TEXT` section and is not
disassemblable in place — labelled uncertain — but every in-place check points to a
partial AP-area update, not a whole-image-from-0 write.)

---

## 3. Building / validating a `UPG952A.AP`

```sh
python3 jupiter/tools/ctkap.py apinfo  UPG952A.AP        # validate every field + checksum
python3 jupiter/tools/ctkap.py apfix   UPG952A.AP        # after editing the body: fix size + checksum
python3 jupiter/tools/ctkap.py apwrap  body.bin out.AP --code 0x41
python3 jupiter/tools/ctkap.py sections dp700wd.bin      # dump + verify a full-flash image
```

The **body must be a valid self-flashing AP** (the application that knows how to
reprogram itself) — the container tool only builds/validates the wrapper and
checksums. The safest source of a body is the OEM `UPG952A.AP` (validate it first),
or an AP rebuilt from this firmware's own sections; then edit + `apfix`.

Place the finished file, named exactly `UPG952A.AP`, in the **root of a FAT USB
stick or SD card**, and trigger the update from the player's update mode.
