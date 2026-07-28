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

### Header = the firmware's `AP_INFO` struct (512 bytes = `0x200`, big-endian)
Field names/semantics are the firmware's own source (`aploader.h` `AP_INFO`,
`aploader.c` `AP_Identify`/`AP_Loader`), each cross-checked against `dp700wd.bin`.

| off | field (`AP_INFO`) | requirement | source ↔ binary |
|---|---|---|---|
| `0x00` | `dwIdentify[0]` | `0x43543930` (`"CT90"`) | `CT909AP_IDENTIFY1` ↔ `0x4260` |
| `0x04` | `dwIdentify[1]` | `0x392d4150` (`"9-AP"`) → `CT909-AP` | `CT909AP_IDENTIFY2` ↔ `0x4274` |
| `0x08` | `dwAP_Type` | AP id; `1` = `AP_AUTO_UPGRADE` | `aploader.h` |
| `0x0C` | `dwAP_Size` | `≤ 0x166000`, 4-byte aligned | `AP_Identify` ↔ `0x42f0` |
| `0x10` | `dwExternalFlag` | `TRUE` ⇒ checksum verified + section-loaded | `AP_Loader` STEP4 |
| `0x14` | `dwVersionAP` | `> 4` | `AP_Identify` ↔ `0x4324` |
| `0x18` | `dwChipVersion` | **`0x41`** (`IC_VERSION_952A`) or `0x01` | `Winav.h` ↔ `0x41dc` (both = `0x41`) |
| `0x1C`–`0x2B` | `dwDescription[4]` | free text | — |
| `0x2C` | `dwCheckSum` | low WORD = `HAL_CheckSum(body[0x200:size])` | `hsystem.c` ↔ `0x40320` |
| `0x30` | `dwAP_SP` | loaded-AP stack pointer (**not** an entry) | `AP_Loader` STEP6 |
| `0x34` | `dwAP_UNZIP_BUF` | UZIP work buffer for section decompress | `AP_Loader` STEP6 |
| `0x38`–`0x1FF` | `dwReserved[114]` | — | — |

### The two checksums — both `HAL_CheckSum` (16-bit additive byte-sum, NOT CRCs)
`HAL_CheckSum` (`hsystem.c`) sums every byte over `[start,end)` into a 16-bit WORD,
stepping by DWORD (so the range must be 4-byte aligned):
1. **Per flash section**: over the *unpacked* section bytes, in section-table entry `+0x14`.
2. **AP body**: over `body[0x200 .. dwAP_Size]`, low WORD of `dwCheckSum` (`0x2C`),
   verified when `dwExternalFlag == TRUE`. The `0x2E` u16 `ctkap` reads is exactly
   that low word (big-endian `dwCheckSum` at `0x2C`; high word 0).

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
reprogram itself) — `apwrap` only builds/validates the wrapper and checksums. The
safest source of a body is the OEM `UPG952A.AP` (validate it first), or an AP
rebuilt from this firmware's own sections; then edit + `apfix`.

---

## 4. Building a self-flashing body (`mkflasher`) + serial-flash write path

### The flash-write driver (reversed from the firmware)
`WriteSPF` @ `0x3d0fc` (XIP, always callable): `WriteSPF(flashAddr, srcBuf, size)`
asserts `flashAddr` is 64 KB-aligned and `size <= 0x10000`, **erases** the 64 KB
sector (one 64 KB erase, or 16×4 KB via `0x4001fea8` when `[0x40033723]==4`), then
**programs** it via `0x4002003c(src, src+size, flashAddr)`. The erase/program
routines live in the decompressed `TEXT` (lma `0x4001d000`): they mask interrupts
(PSR PIL=15), cache-flush (`0x40020578`), gate on `[0x40033720]==0x6655`, and drive
the **SPI/PROM controller @ `0x80002800`** (base ptr `[0x8000006c]`; per-channel
data `+0x22c`, status/trigger `+0x224`, cmd `+0x220`, result `+0x230`; channel byte
`[0x400238d3]`; opcodes at `0x40033720..0x40033728`) via issue+wait `0x4001fe20`
and trigger/poll `0x40020324`.

### `ctkap.py mkflasher` — a self-flashing AP
```sh
python3 jupiter/tools/ctkap.py mkflasher out.AP --write 0x0:newimage.bin
python3 jupiter/tools/ctkap.py mkflasher out.AP --write 0x1a0000:sector.bin --code 0x41
```
Builds a body = a small SPARC stub (`apstub.S`, embedded) at the loader's copy
target `0x4009a000` + a descriptor at `body+0x100`
(`u32 nchunks; {u32 dstFlash, u32 srcBodyOff, u32 size}*`) + the carried image, then
wraps a valid CT909-AP (entry `0x30` = `0x4009a000`, size + body checksum). The stub
loops `WriteSPF` over each 64 KB chunk, then signals + halts.

### Verifying in the emulator (no hardware)
```sh
ct952emu dp700wd.bin --aprun out.AP --flash-out after.bin --instr 5000000   # CT952_FLASHWRITE auto-on
```
`--aprun` copies the AP body to DRAM `0x4009a000` and jumps to the header entry —
as the loader (`0x3e48`) does — under a **`WriteSPF`-contract flash-write model**
(`CT952_FLASHWRITE`): it intercepts `WriteSPF` at its entry and applies the reversed
erase+program to the emulated flash. `--flash-out` dumps the result to diff. This
is a *contract-level* model (the reversed boundary). Verified: a targeted sector
write touches only that sector (boot region intact); a 4-sector reflash keeps the
header + section table intact and the reflashed image **boots identically to the
original**.

For the *gate-level* alternative — driving the firmware's OWN DRAM-resident flash
driver against a modeled `0x80002800` SPI controller (no `WriteSPF` shortcut):
```sh
ct952emu dp700wd.bin --rom-load --spitest 0x1a0000:0x1000 --flash-out after.bin
```
This boots normally (populating the driver config + decompressing the driver into
DRAM), arms the controller model, and CALLs the real `WriteSPF` — whose real
`SE`/`PP` helpers issue real SPI commands (WREN/RDSR/WRSR/SE/streaming-PP) that the
model services against `m->flash`. Verified byte-exact against a staged sentinel
(16 erase + 16 program ops, surrounding flash untouched). See §12.89 in
`DP700WD_HW_REFERENCE.md`.

And the FULL update path — a self-flashing AP body driving that real driver through
the gate-level controller (loader-style body load → real `WriteSPF` → real
erase/program → controller → flash), every layer firmware code except the modeled
controller:
```sh
ct952emu dp700wd.bin --rom-load --apflash out.AP --flash-out after.bin
```
Boots, arms the controller, copies the AP body to DRAM `0x4009a000`, jumps to the
header entry (as loader `0x3e48` does), and lets the body reflash. Verified: 16 erase
+ 16 program ops, target sector matches the payload, and the boot region + the XIP
`WriteSPF` sector are untouched. See §12.90.

And a LOADER-COMPATIBLE AP — a section-table image the real ROM loader accepts and
runs (the OEM format), so the whole update path executes as the device would:
```sh
ctkap.py mksectionap out.AP --write 0x1a0000:new.bin      # AP_INFO + section table + flasher app
ct952emu dp700wd.bin --rom-load --apload out.AP --flash-out after.bin
```
`--apload` stages the AP at `0x4009a000`, replicates `ROMLD_MoveSectionTable`, then
CALLs the binary's `ROMLD_BOOT_LoadSectionAndRun` (`0x4bc`) — which loads the flasher
*section* to its DRAM LMA (`0x40500000`), checksum-verifies it, and jumps in. The
flasher reflashes via the resident DRAM driver + the gate-level controller. This is
the DRAM-driver relocation for real: the flasher runs from a decompressed DRAM section,
never from the flash it erases. Verified byte-exact (16 erase + 16 program; boot region
+ XIP `WriteSPF` sector untouched). See §12.91–92. `dwRMA` in the section table is
IMAGE-RELATIVE (the loader resolves `dwRMA + pSecTbl − 0x10`).

### Two constraints on a *hardware-ready* body (do not skip)
1. **Size / compression.** A full `0..0x166000` image cannot be carried **raw**
   inside an AP that must itself be `<= 0x166000` — the OEM body is UZIP-compressed
   and decompresses on the fly. The raw stub suits **targeted/sector** updates; a
   full reflash needs the compressed-section payload (`repack`).
2. **DRAM-resident driver.** On hardware, calling the XIP `WriteSPF` to rewrite the
   sector that *holds* `WriteSPF` erases the running code. The OEM body runs a
   **DRAM copy** of the flash driver (its own `TEXT`/`DATA`). The emulator's
   entry-intercept hides this, so the stub verifies flashing **logic**, not
   hardware-safe driver placement + the exact AP start offset — safest lifted from
   an OEM `UPG952A.AP`.

---

## 5. Deploying

Place the finished file, named exactly `UPG952A.AP`, in the **root of a FAT USB
stick or SD card**, and trigger the update from the player's update mode.
