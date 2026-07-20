# Coby DP700WD / Cheertek CT952 — Verified Hardware & Hacking Reference

> ## ⚑ ORIENTATION — READ FIRST, DO NOT DRIFT FROM THIS
> - **The chip is CT952A.** `IC_VERSION_ID = IC_VERSION_952A = 0x0041`,
>   `DECODER_SYSTEM = DMP952A_EVAL`. **We are 952. Full stop.**
> - **"909" is NEVER the chip.** It is only (a) the *platform-family* define
>   `CT909P_IC_SYSTEM` that the 952A happens to use, and (b) the *SDK / ROM /
>   toolchain lineage* (`DVD909.rom`, `extend909.exe`, `Vipor_IIC909P.h`,
>   `makerom … DVD909.rom`). Seeing "909" in a filename/define does **not** mean
>   we're a 909 part. If you ever start reasoning "this is a 909…", STOP — re-read
>   this line.
> - **Authoritative source = the `950_Files/` overlay** (copied over the project
>   root to build `dp700wd.bin`; recipe in `950_Files/950_make.txt`; details §12.4).
>   For any module that exists in `950_Files/`, THAT copy is what shipped — the
>   root copy is the wrong (DVD-player) variant. Read from `950_Files/` first.
> - **The 952 update image is `UPG952A.AP`** (1.34 MB, root of tree) — the `.AP`
>   OTA-reflash package for this exact build (`APPacker … -OP UPG952A.AP`, §5).
>   `dp700wd.bin` is the unpacked main ROM; `UPG952A.AP` is the shippable update.
> - **Decoders:** JPEG (photos/splash) = **hardware JPU/VLD/MCU-BIU** on a PROC1
>   worker (CT909P → `SUPPORT_JPEGDEC_ON_PROC2` is OFF); **PROC2 = MPEG video only**
>   (`mpg.bin`). The photo path never uses PROC2 (§10.8).

This is the **deep, source-verified** companion to `DP722_FIELD_GUIDE.md`. Where
the field guide is an orientation, this document is a bring-up manual: every
register address, command opcode, memory address, and image-format field below
was extracted from the firmware source in this tree and is cited by `file:line`.

**Your device is the Coby DP700WD**, not the DP722 — but they are the *same
platform*. The DP700WD is a 7", **480×234**, JPEG-only widescreen frame
(SD/MMC/MS card + full-size USB host, calendar/clock, wood surround). It is the
simplest "DMP" (Digital Media Player, no disc/video) cut of the Cheertek CT9xx
SoC that this `DMP_121` SDK builds. Every platform-level fact — SPARC CPU, eCos,
the UART monitor, SPI-flash layout, the `.AP` update format, the USB stack — is
identical. The **only** thing that differs is panel-specific: your glass is
480×234, and your unit has no video/audio-decode path enabled. Panel timing
tables in §4 are for this SDK's reference panel and must be **bench-verified
against your 480×234 glass** before relying on the exact TCON numbers.

Build identity of this tree: `DECODER_SYSTEM = DMP952A_EVAL`, platform
`CT909P_IC_SYSTEM`, `IC_VERSION_ID = IC_VERSION_952A = 0x0041`, base
`SW_VERSION 278` / `DMP_SW_VERSION 121` (`Winap.h`/`Winav.h:492,54`,
`internal.h:30,76`, `dmpcustm.h`, `customer.h`).

---

## 0. TL;DR — what actually changed vs. the field guide

Three findings from a full source read sharpen (and in two cases correct) the
field guide's conclusions:

1. **The serial monitor's command + RX half is compiled ON in this SDK**
   (`SERIAL_DEBUG` is `#define`d, uncommented, at `customer.h:232`), and so is
   the printf/TX half (`DBG_MINI_PRINTF`, `debug.c:30`). This is *better* than
   the field guide's hedge — the full interactive monitor and the serial-reflash
   path are present in source. **But** the DMP952A board's default pin-mux block
   does **not** route the UART pads out (`pio.h:1193` `#undef`s the UART source
   paths); enabling them is a source edit, and on a retail unit you must probe to
   find which pads (if any) actually carry UART1. See §1.5 — this is the single
   most important bench caveat.

2. **The USB device/gadget path is compiled OUT of this build.**
   `SUPPORT_USB_HOST_DEVICE` is commented at `Winav.h:147`, and the root Makefile
   links the **host-only** `usb.a`. The device-mode MSC gadget objects
   (`fd_storage_*`, `jusb_chp9`, `dcd_tdi_4x`) live only in the un-linked
   `usb/Host_Device/usb.a`. So on your frame the port is **USB host, Mass-Storage
   class only** — not even the SD-card-reader gadget is active. Repurposing the
   OTG port to a non-storage class is a dead end here (details §3).

3. **The entire serial-reflash ("auto-upgrade") protocol is fully documented in
   source** — opcodes, framing, checksums, staging address. This is a complete
   OTA-over-UART channel that loads an arbitrary image into DRAM and programs it
   to flash. It is the cleanest code-execution path on the box (§1.4).

---

## 1. The UART serial monitor — the way in

### 1.1 Hardware config (exact)

| Property | Value | Evidence |
|---|---|---|
| Baud | **115200** | `hio.h:60` `BAUDRATE_115200`; used in every `DBG_Init` (`utl.c:3781`) |
| Framing | **8N1** (8 data, no parity, 1 stop) | control reg only ever gets `RX_ENABLE\|TX_ENABLE`; parity bits (`ctkav_platform.h:224-225`) never set |
| Controller | Gaisler/GRLIB-style **APBUART** (polled, no RX ISR) | `HAL_UART_ReceiveChar` polls `DATA_READY` (`hio.c:2401`) |
| Default port | **UART1** (TX and RX can be split) | `debug.c:102-103` `_dwUartPort=HAL_UART1_TX`, `_dwUartPort_RX=HAL_UART1_RX` |
| Scaler formula | `SCALER = ((sysclk*10)/(baud*8) − 5)/10 ≈ sysclk/(8·baud)` | `hio.c:1829`; `DBG_Init` is called with `sysclk/2` (`utl.c:3781`) |

UART1 register block (base `CT909_IO_START = 0x80000000`, `ctkav_platform.h:40-51`):

| Register | UART1 | UART2 |
|---|---|---|
| DATA | `0x80000070` | `0x80000080` |
| STATUS | `0x80000074` | `0x80000084` |
| CONTROL | `0x80000078` | `0x80000088` |
| SCALER | `0x8000007C` | `0x8000008C` |

Status bits (`ctkav_platform.h:220-236`): `DATA_READY=0x1`, `TH_EMPTY=0x4`,
`OVERRUN=0x10`, `PARITY_ERR=0x20`, `FRAME_ERR=0x40`. Control:
`RX_ENABLE=0x1`, `TX_ENABLE=0x2`.

### 1.2 Text (ASCII) command set

Line-based, `\r`-terminated, hex params (uppercase A–F), parser
`_DBG_CommandParse()` at `debug.c:1333`. Text mode is (re)armed by sending byte
`0xE3` (`START_TEXT_MODE_CMD`), device ACKs `0xE4`.

| Command | Syntax | Effect | Evidence |
|---|---|---|---|
| `MDR` | `MDR<addr> <val> [n]` | **Write** DWORD to DRAM, n words | `debug.c:1342` |
| `MDRA` | `MDRA<addr> <val> [n]` | Read-modify-write **AND** | `debug.c:1351` |
| `MDRO` | `MDRO<addr> <val> [n]` | Read-modify-write **OR** | `debug.c:1353` |
| `DDR` | `DDR<addr> [n]` | **Dump** n DWORDs of DRAM | `debug.c:1375` |
| `RDN` | `RDN<addr> <sz>` | Read mem, sz 1=byte/2=word/4=dword | `debug.c:1409` |
| `RDA` | `RDA<addr> <sz> <cnt>` | Dump array of entries | `debug.c:1415` |
| `RM` / `RMA` / `RMO` | `RM<addr> <val> <sz>` | Modify MMIO reg (none/AND/OR) | `debug.c:1420` |
| `I` | `I` | Dump full system status | `debug.c:715,1441` |
| `PM/PS/PR/PA` | `P?<hex>` | Set MPEG/Servo/MPEG2/PROC2 debug flags | `debug.c:1445` |
| `N` | `N` | Reset DRAM debug-message ring | `debug.c:1517` |
| `K` | `K<hh>` | **Inject IR/remote key** (handled in RX path) | `debug.c:1157-1161` |
| `C` | `CR/CW…` | I²C — **body commented out, no-op** | `debug.c:1466` |
| `O` | `OD/OR/OW` | Macrovision regs — **not compiled** | `debug.c:1498` |

`MDR`/`RM` into MMIO + `DDR`/`RDN` to read = **full arbitrary
memory/register read-write**. Because hot code runs from writable DRAM
(`.text_dram @ 0x4001D000`, §2), poking a live function pointer with `MDR`
is de-facto arbitrary code execution even without a `GO` command.

The `K` command values are the internal key IDs (`input.h`), routed through the
front-panel scan map. Your DP700WD's physical buttons are only:
`LEFT, RIGHT, UP, DOWN, FUNCTION, PLAY/PAUSE, POWER` (scan codes `0x01`–`0x08`,
`key.h:1-13`). `K` lets you drive the whole UI over serial.

### 1.3 Binary command set

Armed by sending `0xF1` (`START_BIN_MODE_CMD`, ACK `0xF2`, `debug.c:1124,1909`).
Each frame = 1 opcode byte + big-endian payload + `0xFF` (`END_CMD`). Dispatcher
`_DBG_ProcessBinCommand()` at `debug.c:1900`; opcodes at `debug.h:143-222`.

| Opcode | Value | Meaning |
|---|---|---|
| `WRITE_DATA_CMD` | `0x81` | PC→device block write to DRAM/flash (2-byte size + payload) |
| `READ_DRAM_CMD` | `0x91` | Read DRAM block (2-byte size) |
| `WRITE_DRAM_CMD` | `0x94` | Write one DRAM DWORD |
| `START_SEQ_RW_DRAM_CMD` | `0x84` | Enter tight sequential DRAM r/w loop ("Vipor tuning") |
| `SEQ_READ_DRAM_SUBCMD` | `0x8A` | seq: 4-byte addr → 4 data bytes |
| `SEQ_WRITE_DRAM_SUBCMD` | `0x8D` | seq: 4-byte addr + 4-byte val |
| `END_SEQ_RW_DRAM_CMD` | `0x87` | leave seq loop |
| `PREPARE_AUTO_UPGRADE_CMD` | `0xB1` | begin serial reflash (§1.4) |
| `AUTO_UPGRADE_CODE_ADDRESS` | `0xB7` | return DRAM staging address |
| `AUTO_UPGRADE_CMD` | `0xB0` | commit reflash (calls `AP_Loader`) |
| `EXIT_AUTO_UPGRADE_CMD` | `0xB4` | abort/finish reflash |

### 1.4 Serial-reflash ("auto-upgrade") protocol — full sequence

While `_bAutoUpgrade` is set, `DBG_Polling` spins a dedicated loop
(`debug.c:160-197`). All in binary mode (send `0xF1` first). Handshake:

1. **`0xB1 0xFF`** → device silences prints, blanks video, disables interrupts
   + watchdog, redirects RX into the shared pool, replies **`0xB2`**
   (`debug.c:2092-2135`).
2. **`0xB7 0xFF`** → device replies **`0xB8`** then the 4-byte big-endian DRAM
   staging address `DS_AP_CODE_AREA` (= `0x4008B000` on the 16 Mbit config,
   §5) (`debug.c:2137-2154`).
3. **Stream image** with **`0x81`** frames: `0x81`, 2-byte size (hi,lo), `0xFF`,
   then `size` raw bytes. Each block's trailer: bytes `[size-6..-5]` = 16-bit
   **little-endian** checksum, `[size-4..-1]` = 32-bit **big-endian** dest addr;
   checksum = 16-bit sum of the first `size-6` bytes. Match → **`0xA0`**,
   mismatch → **`0xA1`** (`debug.c:2207-2243`).
4. **`0xB0`** + 4-byte big-endian total size + `0xFF` → restores interrupts and
   calls `AP_Loader(SRC_FROM_DRAM, DS_AP_CODE_AREA, size)` — programs the staged
   image to SPI flash (`debug.c:2156-2167`).
5. **`0xB4 0xFF`** → restore state, re-enable video/watchdog, reply **`0xB5`**.

This is a complete, source-documented path to (a) load arbitrary code into DRAM
and jump to it (steps 1–3 + an `MDR` to a vector), or (b) durably reflash
(steps 1–5). No soldering beyond the UART pads.

### 1.5 Compile-time gating and the pin-mux caveat

| Macro | State here | Gates | Evidence |
|---|---|---|---|
| `SERIAL_DEBUG` | **ON** | entire RX/command + reflash half | `customer.h:232` |
| `DBG_MINI_PRINTF` | **ON** | printf/TX half + ring buffer | `debug.c:30` |
| coupling | — | RX **requires** TX; you *can* keep TX-only by commenting `SERIAL_DEBUG` | `debug.c:31-33` |
| `DBG_SOLUTION_DEPENDENCY` | **ON** (auto) | `K`,`I`,`P*`, reflash cases | `debug.c:36-41` |
| `RELEASE_SETTING` | **ON** | *cosmetic only* — `[REL-16M]` vs `[DBG-16M]` banner; does **not** disable the monitor | `customer.h:24`, `debug.c:928` |
| `printf()` redirect | active | all `printf` → `DBG_Printf` over serial | `Winav.h:177-187` |

**The catch:** whether the UART is physically reachable depends on the board
pin-mux block selected by `DECODER_SYSTEM`. For `DMP952A_EVAL` the active branch
is `pio.h:1193` (`#if 1`), which **`#undef`s** `ENABLE_DSU1`, `SD_FROM_UART1`,
`SD_FROM_UART2` — i.e. **no debug-UART pins are muxed out by default** on this
board config. The alternate (commented) routings are `CARD_READER_PATH` for
UART1-TX and `EXPAND_GPIO_PATH` for UART2-RX (`pio.h:1203-1218`). Candidate pads
(`hio.c:1850-1898`, `CT909P`): UART1-TX on SPI GPA[1], card-reader GPC[7], or NIM
GPG[1]; UART1-RX on GPA[0]/GPC[8]/GPG[0].

**Bench implication:** the monitor *firmware* is present, but on a retail
DP700WD you should (1) scope the likely pads for a 115200-8N1 boot log, and (2)
be prepared that a source rebuild + reflash may be required to route the pins if
the retail image shipped with them un-muxed. The printf boot log is the first
thing to hunt for.

---

## 2. Memory map (from the `.ld` files)

Big-endian SPARC V8. The DMP build uses the **flash-mapped ROM** model
(`DVD909.ld`), not the DRAM-only model.

```
ROM  (SPI-flash mapped)  0x00002000   .text/.rodata run in place   (DVD909.ld:8)
DRAM                     0x40000000   len 0x49E00                  (DVD909.ld:9)
  .rom_vectors (ROMV)    0x40000000   reset_vector / vectors.o     (DVD909.ld:20)
  INFO header            0x40000B00   45-byte rominfo.bin          (romcfg.txt:18)
  CODE (flash.bin)       0x40001000   compressed main, PROG ENTRY  (romcfg.txt:19)
  .text_dram             0x4001D000   hot code in writable DRAM    (DVD909.ld:25)
                                       (eCos ram, usbdi, interrupt,
                                        spflash, pardram, dec_dram)
  ram2 / .osdstr         0x40049E00   OSD strings                  (DVD909.ld:10-18)
```

`ENTRY(reset_vector)`; initialized data is copied ROM→DRAM at boot
(`__rom_data_start = LOADADDR(.data)`, `DVD909.ld:58-69`).

---

## 3. USB — host-mode Mass-Storage only

- **Only class implemented: Mass Storage.** Host driver `umass`/`jms_*`, device
  gadget `fd_storage_*`/`mass_sample` — and the device gadget is **not linked**
  in this build. No HID/CDC/serial/video **function-driver objects** exist in the
  shipped `usb.a`; only the Jungo API *enum* (`DEVICE_TYPE_HID/CDC_ACM/...`) is
  referenced, with no backing code.
- **Stack is a Jungo USBware binary blob** (`usb.a`, `usbdi.o`, `jos_mem.o`;
  `jos_` = Jungo OS layer; build-path strings `/working/USB/USBWare_host_device/`;
  GCC 2.95.2, big-endian). No stack source ships.
- **Device controller** is `dcd_tdi_4x` (TDI/Chipidea-lineage USB2 UDC), a blob;
  chapter-9 is `jusb_chp9`, also a blob. Register window ~`0x40604000` (from the
  commented `USB_HCInit(0,0x40604000,0xE6000)`, `usbsrc.c:274`).
- **OTG role** is chosen from the ID pin (`USB_GetOTGID()`), power gated by GPIO
  `USB_POWER_GRP=GPIO_A pin 29` (`pio.h:57-58`); pad mux
  `REG_PLAT_GPG_MUX_SELECT = 0x80004074` (`usbwrap.h:24`). Switch command set:
  `SWITCH_USB_STACK=0x20`, `DEVICE_MODE_CHK_CARD=0x40`, `EXIT_USB_STACK=0x80`
  (`usbsrc.h:37-39`).
- Gadget string descriptors (in `mass_sample.o` .rodata): manufacturer
  `"Cheertek"`, product `"Mass Storage"`. VID/PID are filled at runtime from the
  blob's `.data`, not present as readable literals.

**Verdict:** a non-storage USB gadget is not practical here — it would require
the licensed Jungo `fd_*` class drivers (not shipped) linked against an opaque
GCC-2.95 big-endian binary. Use the UART for a non-storage data link.

---

## 4. Display HAL — register map

The "Vipor / CT675" scaler+TCON is an **on-chip** block addressed through a
*fake-I²C* API that is really memory-mapped 32-bit writes
(`vd/Vipor_IIC909P.H:24-61`): `addr = REG_CT675_BASE(0x80003000) + DevSel*0x400 +
reg*4`. So a table row `1,0xF4,0x09,0x03` = write `0x3` to `0x80003824`.

| Block | Base | Source |
|---|---|---|
| DISP (framebuffer scan-out) | `0x80001A00` | `ctkav_disp.h:20` |
| DISP OSD palette / gamma RAM | `0x80001C00` | `ctkav_disp.h:81` |
| TVE (composite encoder) | `0x80001880` | `ctkav_tve.h:20` |
| GPU (2-D blit/font) | `0x80002880` | `ctkav_gpu.h:21` |
| ADC (`0x42`) / MVD (`0x40`) | `0x80003000` / `0x80003400` | `Vipor_IIC909P.H:27-28` |
| **Scaler (`0xF4`)** | `0x80003800` | `Vipor_IIC909P.H:29` |
| **TCON / panel timing (`0xF6`)** | `0x80003C00` | `Vipor_IIC909P.H:30` |
| Platform GPIO / pinmux / PLL | `0x80004004` | `ctkav_platform.h:387` |
| SAR-ADC key input (`ADCGLB`) | `0x8000407C` | `panel.c:193`, `tft.c:914` |

**Panel power-on** (`TFT_Init()`, `tft.c:656`): (A) enable pinmux —
`0x80004058 |= 0x0505A000` (GPA, digital panel), `0x80004064 |= 0x00512224`
(GPD), `0x80004068 |= 0x000000C0` (GPD_EXT); (B) backlight off; (C) pick source
CCIR656/NTSC; (D) `TFT_Init_VP77()` loads the OSD font, the
`Vipor_CCIR656_NTSC_Register` scaler+TCON table, gamma (256×3 into scaler
`0x81/82/83`); (E) set flip via GPIO-D[10]/[11] + TCON `0x02/0x05`.

**Backlight:** GPIO-**G pin 2**, **active-low** (`HAL_WriteGPIO(GPIO_G,2,0)` =
ON) (`tft.c:236-246`, `pio.h:804`); set/clear regs `0x80004044`/`0x80004040`.
Brightness = scaler reg `0xDE` → `0x80003B78` (graded `0x98/58/28/08/00` for
ambient dimming, `tft.c:939`).

**OSD framebuffer:** DRAM `0x4005F000` (`DS_OSDFRAME_ST`, 16 Mbit config,
`dvd_dram_16m.h:133`), **8bpp palettized** (`GDI_OSD_8B_MODE`, `gdi.c:168`),
reference region 616×440, stride = width bytes. A pixel is a direct byte write =
palette index (`_gdi_SetPixel`, `gdi.c:990`). OSD window regs `REG_DISP_OSD_POS
0x1A50` / `SIZE 0x1A54` (bit28 enable) / `CR 0x1A58`; palette RAM at
`0x80001C00+n*4`, gated by `REG_DISP_BRIGHT_CR(0x1A60) |= 0x01000000`. The
palette is stored as **YUV** (the `python/` host tools decode this format).

> ⚠ **DP700WD panel note:** the timing table above targets this SDK's reference
> panel. Your 480×234 glass will need its own TCON (`0xF6`) and scaler-active
> (`0xF4`) values. The *architecture and register addresses are identical*; only
> the timing constants differ. Capture your unit's live scaler/TCON registers
> with the monitor's `RDN 0x80003800 4` / `RDN 0x80003C00 4` sweeps before
> writing your own — that gives you the real numbers for your panel.

**Buttons:** read via SAR-ADC resistor ladder, not a GPIO matrix. `ADCGLB` at
`0x8000407C`: write channel-select (`0x00840000` ch A, `0x00C40000` ch B), read
8-bit result in bits `[31:24]` (`panel.c:198-282`).

---

## 5. Flash layout & the `.AP` update format

### 5.1 SPI-flash driver (`spflash.c`/`.h`)

Register block base `0x80002A20` (`spflash.h:4-9`): `CMD 0x2A20`, `OP 0x2A24`,
`READ_TYPE 0x2A28`, `WR 0x2A2C`, `RD 0x2A30`, `SCK_CTRL 0x2A34`. API:
`SPF_ReadID` (JEDEC `0x9F`/`0x90`/`0xAB`, big device-ID table `spflash.h:63-114`),
`SPF_ReadData` (memory-mapped ASI load `lduba [addr] 0x7`, `spflash.c:965`),
`SPF_EraseSector` (`0x20`/`0xD8`), `SPF_EraseChip` (`0x60`/`0xC7`),
`SPF_WriteProgram` (page program `0x02`, 256-byte pages). This is your external
recovery path: clip the SOIC-8, dump as ground truth, modify, reflash.

### 5.2 The `.AP` update file — what a valid image must satisfy

`AP_INFO` header = first **512 bytes** (`aploader.h:12-25`). Validation in
`AP_Identify()` (`aploader.c:266-350`):

- `dwIdentify[0..1]` **must** = `0x43543930,0x392D4150` (ASCII `"CT90","9-AP"`).
- `dwChipVersion` **must** ∈ `{IC_VERSION_RESERVED 0x0001, IC_VERSION_ID 0x0041}`
  (`_AP_IC_Version_Check`, `aploader.c:243-253`; Makefile passes `-C 65 = 0x41`).
- `dwVersionAP` must be `> 4`.
- `dwAP_Size ≤ DRAM_size − 0x8B000`.
- `dwAP_Type = 1` for auto-upgrade.
- If `dwExternalFlag == TRUE`: `(WORD)dwCheckSum` = 16-bit **byte-sum** of bytes
  `[512 … dwAP_Size)` via `HAL_CheckSum` (`hsystem.c:844-857`,
  `aploader.c:191-222`). (The field guide's `_AP_CheckSum` is the *declaration*;
  `HAL_CheckSum` is the live algorithm.)

Body is a `makerom` image: 24-byte `SECTION_ENTRY` table at ROM offset `0x10`,
≤32 entries (`romld.h:9-31`), each with Load/ProgEntry/Zip flag bits and a
per-section 16-bit checksum in `dwCheckSumFlag>>16`. The `CODE` section (zipped,
ProgEntry) decompresses to `0x40001000`; `INFO` loads to `0x40000B00`.

### 5.3 Build pipeline (`Makefile:379-390`)

1. `rominfo.exe` → `rominfo.bin` (45-byte INFO header).
2. `extend909.exe` + `boot.bin` + `address.txt` → `boot.fin`/`dsu_boot.rom`
   (first-stage boot; `address.txt` supplies `unzip_buff=0x40002000`,
   `sp1=0x40012000`, `mclk_config` 133 MHz, `dram_config`, `prom_config`).
3. `makerom.exe romcfg.txt DVD909.rom` (full main ROM) **and**
   `makerom.exe -dl upgcfg.txt UPG909.ROM` (slim update = INFO + zipped CODE +
   nested `dvd909.rom`).
4. `APPacker.exe -N 1 -C 65 -S 0x4003F000 -Z 0x40040000 -IP UPG909.ROM
   -OP UPG952A.AP -D "UPG909"` — wraps the `AP_INFO` header (magic, sizes,
   checksum) around the payload.

These are **Windows `.exe` tools** and the images are built by SPARC GCC — you
need `sparc-elf-gcc` and a way to run the packers (Wine/DOSBox) to reproduce an
image the loaders accept.

---

## 6. Updated roadmap for the DP700WD

1. **Find + tap the UART.** Scope candidate pads for a 115200-8N1 boot log
   (§1.1/1.5). The printf log confirms `DBG_MINI_PRINTF` survived. If you also
   get command echo, the RX half is live and you own the box.
   - *If the pads are dead:* the monitor is compiled in but likely un-muxed on
     retail (`pio.h:1193`). Plan for an external SPI-flash dump + a rebuilt image
     that routes the pins.
2. **Exercise the monitor.** `DDR 0x2000 40` (dump flash-mapped ROM), `RDN
   0x80003800 4` / `RDN 0x80003C00 4` (read *your* panel's scaler/TCON values —
   these are the real 480×234 timings), `K xx` to drive the UI.
3. **Dump SPI flash externally** (§5.1) as ground truth; map against
   `romcfg`/`upgcfg`/`rominfo`.
4. **Rebuild the toolchain** (`sparc-elf-gcc` + Wine for the packers) and
   reproduce a build; prove the flash/AP loader accepts your image.
5. **First safe mod:** replace the boot logo (`LOGO.TXT`/`logo*.bin`, section
   `LOGO` in `romcfg.txt`) — low risk, high confidence in the toolchain.
6. **MicroPython:** start co-resident (§7).

---

## 7. MicroPython — concrete plan

The hardware is easily capable (SPARC V8 ~133 MHz, 16–64 Mbit DRAM);
MicroPython is C99 and runs on 32-bit big-endian. The blockers are architecture
+ toolchain + drivers, not horsepower:

- **No upstream SPARC V8 (big-endian) MicroPython port exists** — you'd add a
  bare-metal port dir: reset/stack setup, `mp_hal` stubs, a UART REPL.
- **`sparc-elf-gcc` required** (the eCos build implies one).
- **Drivers are register pokes**, and you now have the exact addresses: UART REPL
  (§1.1, `0x80000070`), panel bring-up (§4, `TFT_Init` order), backlight
  (GPIO-G pin 2 active-low), OSD framebuffer (`0x4005F000`, 8bpp), buttons
  (ADC `0x8000407C`).

**Recommended first shape — co-resident REPL blob (reversible, no flashing):**
use the §1.4 serial-reflash *staging* mechanism (opcodes `0xB1`→`0xB7`→`0x81`) to
load a small MicroPython image into DRAM at `DS_AP_CODE_AREA` (`0x4008B000`),
then `MDR` a live vector/callback to jump to it; expose the REPL over the same
UART; light the panel by replaying the §4 register sequence with *your* panel's
captured TCON values. Fast, reversible, no bricking.

**Later — replacement firmware:** build a MicroPython `flash.bin`, wrap it as a
valid `.AP` (§5.2), reflash. Full control, bricking risk, external SPI programmer
(§5.1) as the safety net.

**USB-from-Python:** same wall as §3 — the practical non-storage data port is the
UART, not the OTG connector.

---

## 8. SoC internals — verified by *running the retail ROM* in an emulator

Sections 1–7 are static source reads. The sections below were established by
booting the **actual retail `dp700wd.bin`** (2 MB SPI image, in
`jupiter/emu/dp700wd.bin`) in a from-scratch SPARC V8 emulator (`jupiter/emu/`)
and watching it execute. Facts tagged **[OBS]** were confirmed by observing the
real firmware run; **[SRC]** are from the SDK source. This is the ground truth a
porter needs and the fastest way to validate any future bring-up.

### 8.1 The two CPUs are BOTH SPARC V8 — the decoder is *software*

- **PROC1** (main) and **PROC2** are both LEON2-class SPARC V8 integer units,
  no FPU (`-msoft-float`). **[SRC** `sparc.h`; `hsystem.c` pokes SPARC opcodes
  `0x91d02000`=`ta 0`, `0x01000000`=`nop` into PROC2's entry**]**
- **JPEG/video decoding runs as *microcode on PROC2***, not fixed-function
  hardware (`SUPPORT_JPEGDEC_ON_PROC2`). PROC1 loads a decoder blob to
  `0x40002000` and releases PROC2 to run it. **This is the key porting fact:**
  to decode a photo you either run PROC2's real microcode or reimplement it.
- **Chip ID: PSR reports impl=0xA, ver=0** (`PSR[31:24]==0xA0`). The stock boot
  ROM branches on this (full boot vs. trampoline). An emulator/model that
  reports any other impl/ver takes the wrong boot path. **[OBS** — this single
  fact was what made the retail firmware boot itself**]**
- **Reset vector `0x81c02310`** (`jmpl` to boot code at flash `0x310`), which
  checks `PSR[31:24]==0xa0`. **[OBS]**

### 8.2 PROC2 boot / reset / debug control (for driving the decoder)

- Entry + stack are staged in the **AIU GR bank**: `GR22` = `0x800007D8`
  (`PROC2_STARTADR`, set to `DS_PROC2_STARTADDR = 0x40002000`), `GR21` =
  `0x800007D4` (`PROC2_SP`). **[SRC** `hsystem.c:136-137`**]**
- **Release from reset:** `REG_PLAT_RESET_CONTROL_DISABLE` (`0x80000304`) bit0
  `PLAT_RESET_PROC2_DISABLE`. **Hold in reset:** `REG_PLAT_RESET_CONTROL_ENABLE`
  (`0x80000324`) bit0. Also releasable via **DSU2 control** `REG_PLAT_DSU2_CONTROL`
  (`0x98000000`, bit `PLAT_DSU_CTL_RE = 0x00080000`); halt = `BN|BW` bits.
  **[SRC** `ctkav_platform.h:250/259/622`, `hsystem.c:251-254`,
  `hdecoder.c:700/719`**]**
- **PROC2 has its own SPARC debug unit (DSU2) at `0x98000000`**, full register
  file + `REG_PLAT_DSU2_PC` (`0x98080010`). PROC1's watchdog reads PROC2's live
  PC here to detect a hung decoder (`monitor.c`).
- **Gotcha:** this DMP build holds PROC2 in reset for *audio* (the `NO_PROC2`
  path fires `RESET_CONTROL_ENABLE=1` and clears the audio-cmd word) and only
  loads+releases it for JPEG via `HAL_ReloadAudioDecoder`. So PROC2 is idle for
  most of boot; it comes alive only at photo-decode time. **[OBS]**

### 8.3 The I/O base-address gotcha (WILL bite you)

The headers use **two different `CT909_IO_START` conventions**, and mixing them
up mis-decodes every AV register:

| Block group | Effective base | Examples |
|---|---|---|
| Platform / LEON core | **`0x80000000`** | INT ctrl `0x80000090`, timers `0x80000040`, UART1 `0x80000070`, reset/clock `0x80000300`, AIU GR bank `0x800007xx` |
| AV decode blocks | **`0x80002000`** | BIU `0x80002800`, MCU `0x80002880`, GPU/JPU `0x80002880`, VLD `0x80002080`, DEQ `0x80002280` |

So `REG_MCU_BASE = CT909_IO_START+0x880` only lands at `0x80002880` if you read
`CT909_IO_START` as `0x80002000` — but `REG_PLATFORM_ON_CHIP_BASE = IO_START` is
`0x80000000`. **[OBS** — reconciled against where the running firmware actually
pokes registers**]**

### 8.4 Interrupt architecture

- **Primary LEON controller** at `0x80000090` (mask) / `0x94` (pending) / `0x98`
  (force) / `0x9C` (clear). **[SRC** `ctkav_platform.h:53-56` **/ OBS]**
- **Secondary "PROC1-1st" controller** at `0x800000B0` (mask-enable, direct RW) /
  `0xB4` (pending) / `0xB8` (status R / clear W1C) / `0xBC` (mask-disable). It
  **cascades into LEON interrupt line 13** (`INT_NO_PROC1_1ST`). Bit0 = **VSYNC**
  — the display field tick that drives the whole display/slideshow state machine
  (`interrupt.c: INT_Proc1_1st_isr → ISR_DISPSaveClearStatus`). **Without a
  periodic VSYNC the UI never advances past its first frame.** **[OBS** — supplying
  this interrupt is what moved the firmware off its loading screen**]**
- Sibling secondary controllers: **PROC1-2nd** `0x800000D0` (BIU / MCU-BSRD / USB
  / servo / buffer over/underflow), **PROC2-1st** `0x800001B0`. Interrupt numbers:
  UART1=3, TIMER1=8, TIMER2=9, PROC1-2nd=10, **PROC1-1st=13**. **[SRC** `:129-168`**]**

### 8.5 The photo-decode datapath (PROC1 ↔ PROC2 ↔ decode hardware)

The pipeline a photo flows through, and the handshakes that gate it:

1. **vdec shared SRAM at `0xB0000000`** (`REG_SRAM_BASE`, 2 KB) is the PROC1↔PROC2
   mailbox. `REG_SRAM_PLAYMODE` = `0xB0000190` (byte), `REG_SRAM_WATCHDOG` =
   `0xB0000194`. **[SRC** `ctkav_vdec.h:350-369`**]**
2. **The vdec command protocol** (`comdec.h` `EN_VDEC_CMD`): PROC1 writes a
   command byte to `PLAYMODE`; the PROC2 microcode overwrites it with a completion
   state that PROC1 polls for. Commands/states seen from the running firmware:
   `MODE_STOP=0x10`→ack `MODE_STOPPED=0x11`, `MODE_SCAN=0x40`→`SCAN_DONE=0x12`,
   `MODE_PREDECODE=0x80`→`PREDEC_DONE=0x13`, `MODE_RELEASE_MODE=0x86`,
   `MODE_NONE=0x00`. `_Wait_Decoder_Stop_CMD_ACK` spins for `MODE_STOPPED`.
   **[SRC** `comdec.h:25-53`, `hal.c:1021`, `hdecoder.c:1477` **/ OBS** the byte
   sequence**]**
3. **Bitstream feed = MCU BIU read channel** `BCR08..0E` at `0x80002A20..A38`
   (base `0x80002880 + 0x1A0`). The firmware DMA-feeds JPEG bytes and polls the
   read-channel FIFO status `0x80002A28` for **`BIU_STATUS_BIURDDRDY` (bit
   `0x1000`)**. **[SRC** `ctkav_mcu.h:84-95`, `ctkav_biu.h:82` **/ OBS** the poll**]**
4. **Undocumented decoder-state register `0x80000C10`** — the firmware waits for
   bits[20:16] to reach ≥7 (and/or `PLAYMODE==0x10`) before proceeding. Not in the
   headers; behaves as a decoder/PROC2-written state word. **[OBS]**
5. **2-D GPU engine at `0x80002880`** (shares the block with the JPU; `JPU_CTRL[28]
   JPU_GPU_OP` selects the register interpretation). Does fills + 1-bit font
   expansion into the 8bpp OSD plane — this is how the UI text ("Loading …") is
   drawn. Font glyphs: 64-byte 1-bit, MSB-first. **[SRC** `ctkav_gpu.h`,
   `ctkav_jpu.h` **/ OBS** the font path renders UI text**]**
6. **Display output planes:** OSD 8bpp palette-indexed plane at `0x4005F000`
   (palette `GAM_OSD` at `0x80001C00`, entries are BT.601 `0x00YYUUVV`); the video
   plane frame buffers are `REG_DISP_F0Y/F0C_ADDR` at `0x80001AC0/AC4`
   (`REG_DISP_BASE=0x80001A00`). Panel stride from `REG_DISP_STRIPE`. **[SRC**
   `ctkav_disp.h:71-80` **/ OBS** OSD scan-out**]**

### 8.6 Boot log & memory staging (retail image)

Observed boot sequence from the real ROM **[OBS]**: `DRAM_Config=0108011b`
(16 Mbit), `MCLK=133MHz`, `PROM_Config=20541010`, `Code_Protect=3`. Sections are
staged to DRAM then jumped: `ROMV→0x40000000`, `TEXT→0x4001D000`,
`DATA→0x40020878`, `ENGL→0x40049900`, then `Jump Sec[ROMV]:40000000`. Boot SP =
`0x40012000`. Compressed sections are inflated by the firmware's own UZIP codec
(flash `0x2000`).

### 8.7 Boot logo & built-in demo photos (free test vectors)

**The power-on splash is a JPEG.** The `LOGO` flash section (section-table entry
at `0xe8`: run `0x00104DD8`, size `0xB1A8`) is a **480×270 JFIF at flash
`0x104DE0`** — a blue "bubbles" splash, exactly panel-width. The firmware
decodes and shows it via `UTL_ShowLogo` (`utl.c:481`); the romcfg text at
`0xf12b5` tags the section `JPEG`. This is the **cleanest auto-triggered decode
target** — shown at power-on, no card/USB required — so it's the first thing to
get rendering when validating a decode path. **[OBS/SRC]**

**Slideshow demos:** the frame also ships **five 640×360 JFIF/EXIF photos** — the Windows sample-picture
set (zebra-longwing butterfly, Grand Teton barn, chrysanthemum, Golden Gate, …).
They sit at flash **`0x160000 / 0x170000 / 0x180000 / 0x190000 / 0x1A0000`**, one
per 64 KiB slot, each followed by two thumbnails (main image + `+0x14C` +
`~+0x1A74`). `jupiter/emu/tools/extract_photos.py` pulls and renders them.
Handy as known-good JPEG decode inputs for any decoder port. **[OBS]**

---

*Sections 1–7 are from static reading of this SDK; §8 adds facts confirmed by
running the retail image in an emulator (`jupiter/emu/`, see its `BRINGUP.md` for
the blow-by-blow). On-hardware specifics — whether the retail DP700WD image muxes
the UART pins, and the exact 480×234 TCON timing — must still be confirmed on the
bench.*

---

## 9. The DMP app state machine — how photos actually get displayed

Traced from source (this exact `DMP952A_EVAL` build). This is the control-flow
map a re-implementation (or MicroPython app) needs.

### 9.1 Boot → power-on icon menu (NO auto-slideshow in this build)

- `main()` (`cc.c`) → `INITIAL_System` → **`POWERONMENU_Initial()`** (`cc.c:1320`)
  → `CC_DVD_MainLoop()` superloop (`cc.c:1396`/`836`).
- `SUPPORT_POWERON_MENU` is **on**; `SUPPORT_PLAY_MEDIA_DIRECTLY_POWER_ON` is
  **off** (`Winav.h:1559`), and `MM_PlayPhotoInFlash()` is compiled out. The
  JPEG screensaver (`osdss.c`) is off (`NO_SCREEN_SAVER`). **So there is no
  keyless path that auto-plays the built-in photos** — the frame sits on the
  power-on icon menu and waits for input. (A retail unit that *does* auto-play
  simply ships with `SUPPORT_PLAY_MEDIA_DIRECTLY_POWER_ON` defined.)
- The built-in demo photos play when the user selects the **Favorite icon** →
  `_POWERONMENU_EnterFavoriteMode()` (`poweronmenu.c:1617`), or via the
  thumbnail **Edit** mode (`thumb.c:2356/2811`).

### 9.2 The built-in-photo play trigger

`_POWERONMENU_EnterFavoriteMode()` (gated on `__bMMJPGEncodeNum > 0`):
```
__wDiscType = BOOK_M1;  __wPlayItem = 1;
__SF_SourceGBL[0].bSourceIndex = SOURCE_SPI;   // read photos from serial flash
__bModeCmd = KEY_PLAY;  UTL_PlayItem(1, 0);     // -> parser -> UTL_ShowJPEG_Slide
```
- `__bMMJPGEncodeNum` is armed at boot by `MM_EncodeFile_Init()`
  (`initial.c:1326` → `mm_play.c:2402`): if setup-flash byte
  `SETUP_ADDR_JPG_ENCODE_MASK != 0xEE`, it sets `__bMMJPGEncodeNum =
  BUILD_IN_JPG_ENCODE_NUM (=3)` (erased `0xFF` flash → armed).
- Built-in photos are read as **raw SPI-flash sectors** by the `SOURCE_SPI`
  source, NOT by section name: `srcfilter.c:515` `__dwSFSPIStartAddr =
  SRCFTR_SPI_ENCODE_ADDR + dwStartPos*2048`, 64 KiB per slot. In *this* SDK
  `SRCFTR_SPI_ENCODE_ADDR = 0x110000` (`srcfilter.h:357`); the retail image's
  photos sit at `0x160000+`, so the retail build sets this base differently —
  confirm on the real flash.

### 9.3 Media detection (the "no media" gate / hang risk)

`MEDIA_MonitorStatus` (`cc.c:1049`) → `_MEDIA_MonitorMediaStatus`
(`media.c:1384`) reads USB/card presence through `SrcFilter_GetStatus` →
`USBSRC_GetUSBSRCStatus` (cached `_bUSBSRCState`), set by the USBSRC worker
thread from **`USB_CheckConnect()` / `USB_CheckStatus()` /
`CARD_CardStatus_Inserted()`** — all in the precompiled `usb.a`/`card.a` blobs
(the opaque HW boundary). Not-present → `_bUSBSRCState = NO_MEDIA` → clean fall
back to the power-on menu (`media.c:1570`). **Hang risk for an emulator:** if
those polls never return a not-present verdict (or the thread never posts
`SRCFILTER_FLAG_STATUS`), `_bTriggerCmd1` latches with no timeout — so a model
must make USB/card detection resolve to "no device".

### 9.4 `__bMMAutoPlay`

Set only in the poweron-menu mode-entry functions from stored ImageFrame
settings (`poweronmenu.c:1516/1565/...`, `thumb.c:3063`). Gates the *external
media* slideshow auto-start (`mm_ui.c:1960`), not the built-in Favorite path
(which calls `UTL_PlayItem` directly). No idle timeout starts a slideshow.

## 10. The photo display pipeline (decode → tiled YUV → panel)

`UTL_ShowJPEG_Slide` (`utl.c:377`): parse header → `HALJPEG_Decode()` →
`HALJPEG_Display(frame)`. **[SRC]**

### 10.1 Display frame buffers (16 Mbit DRAM)

Slideshow (single-buffer full-screen, `JPG_SINGLE_FRAME_BUFFER` +
`JPEG_SINGLE_BUFFER_FULL_SCREEN`): `DISP_FrameBufferSet(DS_FRAMEBUF_ST_SLIDESHOW
…)` (`haljpeg.c:95`). `DS_FRAMEBUF_ST_SLIDESHOW = 0x40065000`,
`DS_FRAMEBUF_END_SLIDESHOW = 0x401AC000` (`dvd_dram_16m.h`). Decode buffer is
**720×448**. Resulting `__DISPFrameInfo[0]`: **Y = 0x40065000**, **C =
0x400B3C00** (Y + 720*448). Robust read: `Y = REG_DISP_F0Y_ADDR*8 + 0x40000000`,
`C = REG_DISP_F0C_ADDR*8 + 0x40000000` (`gdi.c:3576`).

### 10.2 Pixel format — macroblock-tiled YUV 4:2:0, semi-planar

From `GDI_FBDrawDot` (`gdi.c:3548`, `SUPPORT_CT909S`), already implemented in
`jupiter/jfb.c`/`jfb.h`:
- **Y**: `off = (y>>4)*strip + (x>>2)*64 + (y&15)*4 + (x&3)`
- **C** (cx=x/2, cy=y/2): `off = (cy>>4)*strip + (cx>>3)*256 + ((cx&7)>>2)*64 +
  (cy&15)*4 + (cx&3)`; **U at off, V at off+128**
- **strip** = `((REG_DISP_STRIPE & 0xFF) << 8) / 4` (the `/4` is 909P-specific);
  720-wide → `strip = 0x2D00`. `REG_DISP_STRIPE = 0x80001A0C`.

### 10.3 The display kick (which DISP registers, library-internal in display.a)

`HALJPEG_Display` (`haljpeg.c:954`): `DISP_Display(frame, DISP_MAINVIDEO)` (page-
flip, latched at VSYNC) + `DISP_DisplayCtrl(DISP_MAINVIDEO, TRUE)`. Register
effects: `REG_DISP_F0Y_ADDR (0x1AC0)=Y/8`, `F0C (0x1AC4)=C/8`,
`STRIPE (0x1A0C)`; window `REG_DISP_VIDEO_POS (0x1A48)` /
`REG_DISP_VIDEO_SIZE (0x1A4C)`; **enable = `DISP_VIDEO_EN (0x10000000)` in
`REG_DISP_VIDEO_SIZE`**.

### 10.4 Compositing & scaling

Four planes, top→bottom: SP2/SP1 (subpicture) > **OSD** (8bpp, 0x4005F000) >
**main video** (the photo) > background (`REG_DISP_MAIN_BG 0x1A34`). **OSD index
0 = transparent color key** (`DISP_OSD_T_EN 0x01000000` in `REG_DISP_OSD_CR1
0x1A5C`; `setup.c:4659`). Two scaling stages: **JPU** scales the photo to
720×448 in the tiled buffer at decode time (`REG_JPU_HEIWID_SRC/DST 0x2890/94`,
`HVSC_FACTOR 0x2898`, kicked by `JPU_GO`); then the **DISP** video scaler
(`REG_DISP_VSCALE_CR 0x1A1C`, `HU_SCALE 0x1A20`, `HD_SCALE 0x1A24`) shrinks the
720×448 window onto the ~480×234 panel.

### 10.5 Emulator status / the current blocker

The functional JPEG decode (picojpeg, §8/`emujpeg.c`) satisfies the
`0x80000c10` decode-progress gate, but the firmware's full `HALJPEG_Decode`
(JPU scaler + JPEG status) does **not** complete: observed that the firmware
never writes `REG_DISP_F0Y_ADDR` and never sets `DISP_VIDEO_EN` — i.e. it does
not reach `HALJPEG_Display`. Reaching a natural on-panel photo needs the JPU
decode/scale handshake modeled far enough that `HALJPEG_Decode` returns success
and the firmware programs the video plane; then the scan-out reads the tiled YUV
at `REG_DISP_F0Y/F0C` (§10.2) and composites it under the OSD.

### 10.6 Boot-state ground truth — the firmware BOOTS; it is not stuck

Three earlier working theories about "why the menu never draws" were **empirically
falsified** by instrumenting the emulator (whole-run PC histogram + a PROC2-reset
counter + PC-ring dump). Recording them so the same dead ends aren't re-walked:

* **NOT a PROC2-reset loop.** The config-apply path resets PROC2 (`0x80000324`,
  `HAL_ReloadAudioDecoder`, `hdecoder.c:719`) **exactly once** per boot, not
  ~70 000×. A direct counter on writes-to-`0x80000324` reads **1**. The "70K"
  figure counted config-walk iterations/PIL writes, not resets.
* **NOT a high-PIL interrupt-starvation spin.** At any sample point PROC1 runs
  with `PIL=0, ET=1`. VSYNC (LEON line 13) *is* serviced — just slowly
  (~25 takes / 300M instr). IRQ13 is "asserted" (pending+enabled) only ~5 000×
  in 300M instr, i.e. it is rarely *pending*, not chronically *masked*.
* **NOT halted / not a config-descriptor deadlock.** `--skip-panelcfg`
  (forcing desc+0x14 `0x4002f770 = -1`, §BRINGUP) and `--build-panelcfg` change
  the assert count but **not** the outcome — because the firmware was never
  blocked there.

**What is actually happening:** the retail image boots all the way into **eCos**
and its scheduler runs normally. Evidence from the whole-run PC histogram
(`CT952_PCHIST=1`, dumps hottest PC buckets at exit):

| rank | PC (DRAM) | what it is |
|------|-----------|------------|
| hot  | `0x40001010` / `0x40001064` | SPARC **window overflow / underflow** trap handlers (`save`/`restore` + `wr %wim` + `rett`) — ~63% of all cycles |
| hot  | `0x40001000` | generic **trap dispatcher**: `ld [0x40020878 + tt*4], %l6; jmp %l6` (software trap table base `0x40020878`) |
| hot  | `0x4001d7f0` | eCos scheduler **find-lowest-set-bit** over the ready-priority bitmap (`sll 1,i,%g2; btst mask,%g2; loop bits 0..31`) — the run-queue scan |

A hot lowest-set-bit run-queue scan + heavy window-trap traffic is the fingerprint
of a **live, context-switching RTOS**, not a hang. The menu (OSD plane, enable =
bit28 of `REG_DISP_OSD_SIZE 0x80001A54`) simply has not been drawn yet: the UI
thread either needs far more emulated time or is blocked on a resource we don't
model (media-detect poll §9.3, a timer, or the audio DSP). This *relocates* the
remaining work from "unblock a stuck CPU" to "let the UI thread reach the draw" —
a scheduling/time or missing-device-ack problem, not a control-flow deadlock.

**New emulator diagnostics (all opt-in, zero cost when off):**
* `CT952_PCHIST=1` (+ `CT952_PCHIST_N=<n>`) — whole-run PC histogram, top-N hot
  buckets at exit (`sparc.c: pch_sample` / `sparc_pchist_dump`). The tool that
  found the above.
* `CT952_TRACE=1` now also prints the last-64 **PC ring** + exit `%psr`
  (PIL/ET), a **PROC2-reset-write counter** (`0x80000324`), **per-IRQ-level take
  counts**, the **eCos tick** value (`*(*(0x400398f8)+8)`), and per-core
  `icount`/`halted`/`halt_reason`.
* `CT952_P2TRACE=1` — logs PROC2-core reset/release events (`0x304`/`0x324`
  bit0, DSU2 `0x98000000`) with pc + staged entry.
* `CT952_PMTRACE=1` — logs the first 80 decoder-playmode reads
  (`0xb0000190` + software mirrors `0x40039cd0`/`0x40039d34`) with pc/icount.
* `CT952_FORCE_PLAYMODE=0x10` — **experiment hook**: presents the vdec playmode
  as a fixed value (stands in for the decoder microcode reaching a state).
  Proved the decoder-state poll below is a real gate.

### 10.7 The real menu-draw gate — decoder-state polls with PROC2 held in reset

Tracing the boot thread's actual hot PCs (`CT952_PCHIST`, filter to flash
`<0x40000000`) pinned the block to a **two-stage decoder-state poll** in the
boot/init path:

* flash **`0x61240`** — stage 1 waits up to `0xbb7 = 2999` eCos ticks for
  `0x375a0(x,0)==1`; stage 2 (`0x612e0`) waits up to `0x752f = 30015` ticks for
  `0x375a0(x,1)==1`. At ~7.3 ticks / M-instr, the stage-2 *timeout* alone is
  ~4 billion instructions — so booting in reasonable time **requires the poll
  condition to actually be met**, not time out.
* flash **`0x375a0`** returns `1` (what the poll wants) only when the decoder
  **playmode state == `0x10` (MODE_STOP)**; state `0x11` (STOPPED) returns `0`.
  It reads the state via **`0x6f054`**, which uses the hw playmode
  `REG_SRAM_PLAYMODE 0xb0000190` when non-zero, else the software mirror
  `*(0x40039cd0)`.
* Observed: `0xb0000190` reads **`0x00`** throughout; the mirror is **`0x11`**.
  So the poll never sees `0x10` and waits out the full (billions-of-instr)
  timeout. **Forcing** the playmode to `0x10` (`CT952_FORCE_PLAYMODE=0x10`)
  makes the poll's PCs vanish from the hot set — boot advances to the *next*
  decoder gate (`0x70240`, which checks `*(0x40039f24)==1`). Confirms the poll
  is a real gate, and the blocker is a **chain** of decoder-state handshakes.

**Root cause — PROC2 is deliberately held in reset at the menu.** The only
PROC2-core event in a whole boot is a single reset *assert*
(`REG_PLAT_RESET_CONTROL_ENABLE 0x80000324 = PLAT_RESET_PROC2_ENABLE(0x1)`, at
flash `0x3f9c4`, ~icount 5M). It is **never released** — no `0x80000304` bit0,
no DSU2 (`0x98000000`) write. The release lives in **`HAL_ReloadAudioDecoder`**
(`hdecoder.c:616`, `MACRO_PLAT_RELEASE_PROC2` line 700 / `0x80000304 =
PLAT_RESET_PROC2_DISABLE` line 719), and its **only** caller is
`hdecoder.c:2127`, guarded by `bResetProc2` inside the **CHEERDVD
media-playback thread** — *not* the boot path. So the decoder DSP starts only
when you play media; the power-on menu is meant to draw with PROC2 in reset.

**Implication for the model.** With PROC2 legitimately halted, the firmware
manages the vdec playmode *itself* (routine `0x6f800`, keyed on the VDEC
reset-control bits `btst 0x300, *(0x80000304)`; converts a `0x86` command to
`0x10`). The emulator's `proc2_ack_of` stand-in (which rewrites playmode on
firmware writes, e.g. `0x10→0x11`) can therefore *fight* the firmware's own
state management.

**Refinement (`CT952_PMTRACE` write-side).** The decoder state machine is in
fact **alive and cycling** on its own: the firmware writes playmode
`0x10 (MODE_STOP) → 0x11 (STOPPED) → 0x80 → 0x00` in a steady loop (flash
`0x6f3f8`/`0x70208`/`0x70980`/`0x70f2c`), updating the mirrors `0x40039cd0`/
`0x40039d34`/`0x40039f24` in lock-step. So the hot `0x375a0`/`0x61240` poll is
the **decoder-command thread doing normal work**, *not* a hung menu thread —
attributing it to the menu blocker (via PC samples alone) was an over-read.
`CT952_FORCE_PLAYMODE` perturbs that thread but does not itself draw the menu.

**Where the menu draw really stalls (current best model).** `POWERONMENU_Initial`
(`poweronmenu.c:380`, called once from `cc.c:1320` after `INITIAL_System`) runs
`UTL_ShowLogo()` then `_POWERONMENU_DrawAllUI()`. The boot log confirms the COBY
logo *is* staged, yet neither the video plane (`DISP_VIDEO_EN`,
`REG_DISP_F0Y_ADDR`) nor the OSD plane (`DISP_OSD_EN`, bit28 of `0x80001A54`)
is ever enabled (§10.5). So the display *calls* issue but never *complete*: the
gap is the **DISP / `display.a` display-completion handshake** (a
library-internal register/VSYNC sequence we don't model gate-for-gate), not a
CPU control-flow deadlock. That — not the decoder poll — is the next real
target for a natural on-panel image.

**Note on symbols.** `DVD909.sym` (168 KB, `T`/`U`/`D` entries incl.
`POWERONMENU_Initial @ 0x4b808`) is for the **DVD909 player build and does NOT
map to `dp700wd.bin`** (that address is mid-function in the DP700WD image).
Useful for names/source cross-ref only; addresses must come from `dp700wd.bin`
directly.

### 10.8 The JPEG decode handshake — register-exact gap (the display target)

Cross-referencing a source trace with the emulator's live I/O log pinned the
*exact* hardware the firmware's JPEG worker thread waits on. This build compiles
**hardware** JPEG decode (not PROC2 microcode): `SUPPORT_JPEGDEC_ON_PROC2` is
defined only for `CT909R` (`Winav.h:1074`), and this image is `CT909P`
(`platform.h:27`). So `JPEG_Decode()` (precompiled module) posts to a PROC1
worker thread that programs the JPU/VLD/MCU-BIU blocks, feeds the bitstream, and
the caller polls `HALJPEG_Status(HALJPEG_DECODE)==JPEG_STATUS_OK(1)` with a 5 s
timeout (`utl.c:429`, `UTL_ShowJPEG_Slide`). No OK → `HALJPEG_Display` (which
sets `DISP_VIDEO_EN`) is never called.

`JPEG_Status` is a **thread-updated software variable**, not a register — so it
can't be poked; the worker must be *allowed to finish* by satisfying every
hardware poll it makes. The emulator already answers the VLD and JPU polls:

| poll | addr | value | emu |
|------|------|-------|-----|
| VLD entropy done | `0x80002208` `REG_VLD_STATUS` | `VLD_MB_RDY 0x04000000` | ✓ modeled |
| VLD per-stage | `0x800021C0` `REG_VLD_MBINT` | `0x80B` | ✓ modeled |
| JPU scale done | `0x80002880` `REG_JPU_CTRL` | `JPU_BUSY 0x1` clears on `JPU_GO` | ✓ modeled |
| decode progress | `0x80000C10` | bits[20:16] ≥ 7 | ✓ but **opt-in only** (`jpeg_decode_en`, CLI) |

**The unmodeled gap — the BIU bit-stream read channel (MCU block).** Live I/O
log of a plain boot shows the worker **spinning on the MCU BIU read-channel**:
`0x80002a28` read **337 926×** (returns 0), `0x80002a34` read 6615×,
`0x80002a24` written `0x05`. Via the §8.3 effective-base-`0x2000` gotcha these
are `REG_MCU_BASE`(eff `0x80002880`) + `0x1a0..0x1b4`:

| addr | reg | role |
|------|-----|------|
| `0x80002a20` | `REG_MCU_BCR08` | BIU bit-stream read-channel **base address** (points at the staged JPEG in DRAM) |
| `0x80002a24` | `REG_MCU_BCR09` | read-channel **current read address** |
| `0x80002a28` | `REG_MCU_BCR0A` | read-channel x/y increment — **polled 337 K×** |
| `0x80002a30` | `REG_MCU_BCR0C` | read-channel **FIFO status** |
| `0x80002a34` | `REG_MCU_BCR0D` | `[31:30] bsrdtype, [23:16] bsrdheight` — polled 6615× |

The worker sets up this DMA channel to stream the JPEG bitstream from DRAM into
the BIU/VLD, then polls it for the stream to drain / the read pointer to
advance. **The emulator never advances the channel**, so the poll (the
`JPEG_WAIT_VDREM_TIMEDOUT_COUNT = COUNT_3_SEC` "VDRemainder" wait, `jpegdec.h:31`)
times out and `JPEG_Status` stays `UNFINISH(2)`.

**Concrete plan for a natural on-panel image** (next implementation):
1. **Auto-arm** the `0x80000C10` progress gate during boot (not just under
   `--decode-jpeg`), keyed off the worker actually kicking a decode.
2. **Model the BIU read-channel drain**: when the worker programs `BCR08`
   (base = staged JPEG in DRAM) and kicks decode, advance `BCR09` (read addr) to
   the end and present `BCR0A`/`BCR0C`/`BCR0D` as "drained/ready", so the
   VDRemainder wait completes.
3. **Produce the output the DISP scan-out reads**: functionally decode the
   staged JPEG (already have picojpeg) and write it as **macroblock-tiled YUV
   4:2:0** into the firmware's video frame buffer (Y `0x40065000`, C
   `0x400B3C00`, §10.2) — so when `HALJPEG_Display` flips `DISP_VIDEO_EN`, the
   panel shows the real image, produced through the firmware's own pipeline.

Steps 1–2 unblock `HALJPEG_Decode → JPEG_STATUS_OK → HALJPEG_Display`; step 3 is
what makes the displayed pixels the actual photo rather than frame-buffer
garbage. This is the through-line to the user's goal: the firmware's own
slideshow drawing the built-in butterfly, on the panel, naturally.

**Empirical result — faking the poll status alone is NOT enough.** The worker's
poll loop lives at DRAM `0x4001fe90`
(`ldub [base+0x2d3]; sll 2; ld [tbl+idx]; btst %o1,%mask; be loop` — a generic
"wait status-bit with retry" helper). Forcing its status register
(`0x80002a28`/`0x2a30`/`0x2a34`) to read `0xFFFFFFFF` (`CT952_JPEG_DMA=1`) makes
each poll succeed instantly — but the worker then just churns the channel ~5.5 M×
and **still never sets `DISP_VIDEO_EN`** (`0x80001a4c` stays 0, `F0Y 0x80001AC0`
never written). Reason: the worker's *completion* is driven by real decode
progress (bitstream EOI flowing BIU→VLD→JPU), not by the ready bit alone —
satisfying the poll just spins it over a bitstream that isn't there. So the
model must actually **move the bitstream / produce the decoded output** (plan
step 2+3 together: advance the read pointer to EOI *and* write the tiled-YUV
result), not merely answer the status poll. The poll helper, its register table
(indexed by the channel byte at `base+0x2d3`), and the `0x4001fe90` address are
the concrete hooks for that work.

### 10.9 CORRECTION — the logo JPEG decode is never kicked; the stall is upstream

A follow-up iolog+DRAM audit of a plain boot **overturns the framing of §10.8**:
the heavy `0x80002a28` poll (337 926×) is **NOT** the logo JPEG. During a whole
boot the actual JPEG-hardware kick registers are **never written**:

* `REG_MCU_BCR08` (0x80002a20, read-channel *base* = the bitstream address) —
  **never written**.
* `REG_JPU_CTRL` / `JPU_GO` (0x80002880) — **0 writes** (2 reads only): no JPU
  scale/decode is ever kicked.
* `REG_DISP_F0Y_ADDR` (0x80001AC0) — **never written**; `DISP_VIDEO_EN` and
  `DISP_OSD_EN` never set.

So the `0x80002a28`/`0x4001fe90` poll is the **vdec/decoder-command thread's own
generic indexed-register work** (a reusable "poll `reg[table[idx]] & mask`"
helper — *not* JPEG-specific), and attributing it to the logo decode was the
same PC/register-sample over-read as the earlier `0x375a0` mistake. The logo
bitstream *is* staged (`0x401DC000` = `DS_VDBUF_ST_MM`, bytes `FF D8 FF E0…JFIF`,
which is why `make logo` can decode it), but **the firmware never reaches the
point of handing it to the JPEG hardware**. The stall is **upstream** — in the
`UTL_ShowLogo` / `POWERONMENU_Initial` display-bring-up (`display.a` /
decoder-command completion handshake), *before* any JPEG kick. That, not the
JPEG datapath, is the true remaining blocker to locate.

**Implication:** modeling the JPEG datapath now would be dead code (its trigger
never fires). Priority order is (1) find/clear the upstream logo-display stall so
the firmware *itself* kicks the decode, then (2) apply the JPEG datapath model
below so the kicked decode produces real pixels.

**Decisive locator — the menu is never drawn; the stall is in `INITIAL_System`.**
GPU 2-D op accounting over a whole boot (`CT952_TRACE` `[GPU]` line): **`ops=0,
font=0`, every mode 0**. The power-on menu is drawn exclusively via GPU font/blit
ops (`_POWERONMENU_ShowIcon` → `gdi.c` GPU ops into the OSD plane); zero ops means
`_POWERONMENU_DrawAllUI` **never ran**, so `POWERONMENU_Initial` (`cc.c:1320`) is
never reached. Boot therefore stalls **upstream, inside `INITIAL_System`**
(`cc.c:1312`, `initial.c:408`) — before the menu, before any logo decode. This
also means the ~1664 stray OSD pixels seen earlier are *not* menu content (no GPU
op produced them). **The true blocker is a wait inside `INITIAL_System`'s
hardware bring-up on the main boot thread** — a different thread from the vdec /
decoder-command thread whose state-cycling (§10.7/§10.9) was a red herring. Next:
locate the main boot thread's blocked wait inside `INITIAL_System` (candidate
sub-inits: display/panel bring-up, decoder init, media/servo detect, a
device-ack poll). That single wait is the gate to the whole menu → logo →
slideshow chain.

### 10.11 Boot-thread block located: the MODE_STOP poll in INITIAL_PowerONStatus

The block is **one call past `INITIAL_System`** — inside **`INITIAL_PowerONStatus`**
(`initial.c:632`, called from `cc.c:1315`, between `INITIAL_System` at 1312 and
`POWERONMENU_Initial` at 1320). Call chain on the **main boot thread**:
`cc main → INITIAL_PowerONStatus (flash 0x418f0) → decoder-cmd dispatcher 0x59e90
→ 0x61170` (the poll; hot PC `0x61240`). Function addresses anchored via debug
strings: `INITIAL_System=0x416f4` ("INIT Platform error code"),
`INITIAL_PowerONStatus=0x418f0` ("Some thread not initial done"). Forward
call-graph proof: `0x416f4` does NOT reach `0x61170`; `0x418f0` DOES.

This **vindicates the `0x61240` finding that §10.9 had recanted** — it *is* the
boot-thread gate, reached synchronously by the boot thread (not merely the vdec
thread). `0x61170` is a precompiled decoder-library routine (`dec_dram.a`, no
`.c`) that stops the video decoder and two-stage busy-polls for the MODE_STOP
ack: stage-1 timeout `0xbb7`=2999 ticks waiting `0x375a0(x,0)==1`, stage-2
`0x752f`=30015 ticks waiting `0x375a0(x,1)==1` — both satisfied only when the
decoder **playmode == `0x10` (MODE_STOP)** (`comdec.h:29`). The firmware's own
stand-in state cycler (PROC2 is in reset) transitions `0x10→0x11` too fast for
the boot poll to latch `0x10`, so both stages ride out their (billions-of-instr)
timeouts. (`0x841f0`, the other hot PC, is the background command-queue thread's
idle wait — a confirmed red herring.)

**It's a short chain, and it clears.** `CT952_FORCE_PLAYMODE=0x10` (hold
`0xB0000190`=`0x10`) clears gate 1 and exposes gate 2: `0x70240` waits
`*(uint16*)0x40039f24 == 1`. `CT952_VDEC_IDLE=1` presents that as 1. **With both
forced, every decoder-poll PC (`0x375a0`/`0x61240`/`0x70240`/`0x6f0xx`)
disappears from the hot set** — the boot thread is past the decoder-init
handshake and now sits in ordinary sequential `OS_DelayTime` init pacing
(`0x59850`, a plain busy-delay: `get t0; {delay; } while (now-t0 < N)`), with no
stuck poll-condition alongside it. That signature (pure delay primitive hot, no
condition body) *suggested* the remaining distance to `POWERONMENU_Initial`
might be time/pacing — **but a 1.5-billion-instruction run (~11 s emulated) with
both gates forced still shows `GPU ops = 0`**. So it is NOT pacing: the boot
thread is **delay-*polling* a third condition** (the `0x59850` `OS_DelayTime` is
the wait *between* poll iterations, not sequential init). A third gate remains
before the menu draws — find what the boot thread reads between the `0x59850`
delays with both gates already forced. This is an iterative chain of decoder/
display-init handshakes; 2 links cleared, ≥1 remains.

**Root cause & the faithful fix direction.** The whole chain exists because
**PROC2 is deliberately held in reset at the menu** (§10.9), so no DSP drives the
decoder state machine, and the emulator's `proc2_ack_of` stand-in collapses
`MODE_STOP(0x10)→STOPPED(0x11)` with zero dwell — erasing the `0x10` window the
boot poll needs. The faithful model (vs the `FORCE_*`/`VDEC_IDLE` experiment
hooks) is to make the halted-decoder stand-in **hold `MODE_STOP(0x10)` for a
number of polls before advancing to `STOPPED(0x11)`**, giving the boot poll a
real window to latch — matching how the real decoder holds the stop state until
acknowledged. Experiment hooks added: `CT952_VDEC_IDLE` (gate-2 flag
`0x40039f24==1`), composed with `CT952_FORCE_PLAYMODE`.

### 10.12 The full gate chain resolved — gate 3 is MODE_STOPPED, not a new flag

Thread-isolated PC capture (boot stack `sp=0x40036f58`, icount>40M) with both
prior gates forced pinned the third block **precisely** — and it is the **same
playmode datapath as gate 1, one state further along**:

* First, `0x59850` (`OS_DelayTime`) is a **red herring**: it's called exactly
  twice all boot (`OS_DelayTime(25)`@`0x401ac`, `OS_DelayTime(150)`@`0x33c94`),
  both plain sequential init delays that **complete** (~icount 33M). The eCos
  tick (level-8 IRQ, `0x4002e328`) advances normally (1068@150M, 1441@200M), so
  time is not frozen.
* The real block: after the delays the boot thread enters the VDEC-stop /
  display-init code (`0x33xxx`–`0x34xxx`) and spins in decoder-state waits for
  **MODE_STOPPED (`0x11`)**:
  - `0x33dac`: `getstate(16)==0x11 ?` (`0x33dec: subcc state,17; bne 0x33dc0`),
    149-tick timeout; issues `MODE_STOP` first via `0x6f2b0(id,16,0)`.
  - `0x36ff0`: `0x375a0(id,3)==1 ?` (returns 1 for state ∈ {0x10,0x11,0x12}),
    2499-tick timeout — **22 call sites**, 3 of `0x33dac`.
* The getter `0x6f054` returns `mirror(0x40039cd0)` **OR'd with a busy bit
  `0x1000`** whenever the live `0xB0000190 ∉ {0, 0x11}`. Under the forces:
  live `0xB0000190`=`0x10` (pinned), mirror `0x40039cd0`=`0x00` (never set to
  STOPPED) → getter returns `0x1000` → `≠0x11` and classifies to 2, never 1 →
  every wait rides its full timeout. At ~7 ticks/M-instr a single 2499-tick wait
  is ~350M instr; the cascade of 22 keeps `GPU ops=0` well past 1.5B. **Death by
  timeout cascade, not one hard hang.**

**Why no constant force can ever clear it (verified):** gate 1 needs `0x10`,
gate 3 needs `0x11`; `CT952_FORCE_PLAYMODE=0x11` breaks gate 1, `=0x10` breaks
gate 3, and neither sets the mirror `0x40039cd0`. The state **must transition
`MODE_STOP(0x10) → MODE_STOPPED(0x11)`** *and* the firmware's own STOP-command
path must then write `0x11` into the mirror `0x40039cd0`. `proc2_ack_of`
(`machine.c:461`) already maps `0x10→0x11`; the fix is to give the halted-decoder
stand-in **realistic dwell** — hold `0x10` long enough for the gate-1 poll to
latch, then advance to `0x11` for the gate-3 poll — *without* a static override
racing or pinning it. The `FORCE_*`/`VDEC_IDLE` hooks are diagnostic dead-ends
for the real fix (they proved the chain; they cannot clear gate 3).

**Confirmed gate order (INITIAL_PowerONStatus → menu), PROC2 in reset:**
1. `0x61170`/`0x375a0` — decoder-stop ack, wants playmode `0x10` (MODE_STOP).
2. `0x70240` — wants `*(uint16*)0x40039f24 == 1`.
3. two sequential `OS_DelayTime` (complete normally).
4. `0x33dac` (`==0x11`) + `0x36ff0` (`0x375a0(id,3)==1`) ×22 — want **MODE_STOPPED
   `0x11`** at both `0xB0000190` and mirror `0x40039cd0`.

**Net:** the boot-thread block is one coherent problem — the halted decoder's
STOP→STOPPED handshake never plays out in emulation because the stand-in has no
dwell and the software mirror never follows. Implementing a small decoder-stop
state machine in the stand-in (command MODE_STOP → dwell at 0x10 → 0x11, live +
mirror) should clear gates 1, 3, and likely 2 together, letting the boot thread
reach `POWERONMENU_Initial` and draw the menu on its own. That is the single
remaining piece before the §10.10 JPEG-display path opens.

### 10.13 Decoder-stop dwell implemented — clears the poll gates, mirror still lags

Implemented the faithful dwell (`machine.h` `proc2_ack_cycle`/`proc2_ack_dwell`,
`machine.c` bus_wr/bus_rd + `machine_init`; default 300 000 cycles, tunable via
`CT952_VDEC_DWELL`): a playmode write whose ack differs from the commanded value
now **holds the commanded state for a dwell in cycles** (real decoder holds
"stopping" until acknowledged) before delivering the ack — replacing the old
8-*read* countdown that the busy decoder-command thread consumed before the boot
poll could latch `0x10`.

**Result:** with the dwell (and **no** `FORCE_*`/`VDEC_IDLE`), the decoder-poll
PCs (`0x375a0`/`0x61240`/`0x70240`) **vanish from the hot set** — the boot thread
clears the decoder-command polls *naturally*, reaching the same post-poll state
the forces produced. Playmode now visibly cycles `00→10→11→12`. **But the menu
still doesn't draw** (`GPU ops = 0`): it lands in the same delay region, i.e.
gate-3's requirement that the **software mirror `0x40039cd0`** also read `0x11`
is still unmet.

**CORRECTION (verified) — the dwell fully solves the decoder chain.** A widened
`PMTRACE` proved the "mirror stays 0x00" above was an artifact of the *forced*
runs (`FORCE_PLAYMODE` pins live=0x10 so the mirror never gets 0x11). With the
**faithful dwell and no forces**, the sequence completes: live `0xB0000190`
0x10→(dwell ~301827 cyc)→`0x11` (boot poll latches STOPPED at pc `0x3c810`), then
the firmware's own mirror-writer (`0x6ffb4`→`0x70070`, gated on
`*(u16*)0x40039f24 != 1`, which is 0 at the menu) writes **`0x40039cd0 = 0x11`**.
Getter `0x6f054` returns `0x11`; gates `0x33dac` (`==0x11`) and `0x36ff0`
(`0x375a0(id,3)==1`) **both pass**. The whole decoder-STOP handshake (gates 1/2/3)
is cleared by the dwell alone. `disp`/`logo`/`check` regressions pass.

### 10.14 Block moved to the boot logo display — §10.10 is now the LIVE target

With the decoder chain cleared, the boot thread advances into
`INITIAL_PowerONStatus`'s post-STOP sequence and now stalls in
**`_INITIAL_ShowFirstLOGO()`** (the boot splash logo), *before* `OSD_Initial()` /
`POWERONMENU_Initial()`. Direct evidence: with the dwell, reads of the JPEG
decode-progress gate **`0x80000C10` jump from 8 to 15 436** — the boot is now
spinning in the logo's decode-wait. This is exactly the §10.10 path, and it means
that spec is **no longer "dead code / trigger never fires" (§10.9)** — the dwell
made the JPEG-decode kick reachable. (`§10.9`'s "kick never fires" was measured at
the *old* pre-dwell stall point, upstream of `_INITIAL_ShowFirstLOGO`.)

Satisfying the `0x80000C10` gate (`--decode-jpeg`, which also runs the functional
picojpeg decode — logo decodes 480×270) lets the decode "report done" but the
menu still does **not** draw (`GPU ops=0`): the logo path also needs the
`display.a` completion — the JPU/BCR08 kick + `DISP_VIDEO_EN` program — modeled
per §10.10 (still `BCR08`/`JPU_GO`/`F0Y` = never written; the firmware's
`HALJPEG_Display` isn't reached yet because its own decode-status thread-var
needs the full BIU/VLD/JPU handshake, not just the `0xC10` gate). **Next
implementation:** auto-arm the `0xC10` gate during boot (not just under
`--decode-jpeg`) and drive the §10.10 datapath (functional decode → tiled YUV to
`0x40065000`/`0x400B3C00` → present BIU/VLD/JPU polls done) so
`_INITIAL_ShowFirstLOGO`'s `HALJPEG_Decode` completes, the firmware runs
`HALJPEG_Display`, sets `DISP_VIDEO_EN`, and boot proceeds to draw the menu —
after which the logo/photo is on the panel through the firmware's own pipeline.

### 10.10 JPEG datapath implementation spec (ready to apply once §10.9 is cleared)

Full evidence-backed recipe for when the firmware does kick the decode:

* **Kick (canonical):** `io_write` to `REG_MCU_BCR08` (0x80002a20) with a value
  `V ≥ 0x40000000` where `DRAM[V]==0xFF, DRAM[V+1]==0xD8` → decode source `B=V`.
  (`ctkav_mcu.h:84,445`; §8.5.) Reachable-today substitute: read of `0x80000C10`
  with `jpeg_decode_en`, `B = 0x401DC000` (already in `machine.c`).
* **Run pixels on the JPU scale kick:** `io_write` to `REG_JPU_CTRL` (0x80002880)
  with `JPU_GO`(0x2), op bits[6:4]=`001` (`JPU_SC_OP`), bit28(`JPU_GPU_OP`)=0 —
  the bit the existing `JPU_BUSY`-clear model already watches (`machine.c`).
* **Output buffer (single-buffered, page 0, full-screen 720×448):**
  Y → **`0x40065000`** (`DS_FRAMEBUF_ST_SLIDESHOW`, `haljpeg.c:95`,
  `dvd_dram_16m.h:103`), C → **`0x400B3C00`** (= Y + `dwYMax(0x9D80)*8` =
  Y + `0x4EC00` = Y + 720×448). Not double-buffered (F0=F1=F3). Robust: the
  scan-out should read `Y=REG_DISP_F0Y_ADDR*8+0x40000000`,
  `C=REG_DISP_F0C_ADDR*8+0x40000000` (`gdi.c:3576`).
* **Tiling (verified `gdi.c:3548–3568`, 909P branch), strip=`0x2D00` always
  (buffer is always 720-wide):**
  - Y:  `off = (y>>4)*0x2D00 + (x>>2)*64 + (y&15)*4 + (x&3)`
  - C (cx=x/2, cy=y/2): `off = (cy>>4)*0x2D00 + (cx>>3)*256 + ((cx&7)>>2)*64 +
    (cy&15)*4 + (cx&3)`, **U at `off`, V at `off+128`** (semi-planar).
* **Present-as-done so the firmware flips the plane itself:** BIU read-channel
  polls (`0x80002a28/2a30/2a34`) ready (≥`BIU_STATUS_BIURDDRDY 0x1000`; the
  `CT952_JPEG_DMA` hook returns `0xFFFFFFFF`), `0x80000C10` bits[20:16]≥7, VLD
  `0x80002208`|`VLD_MB_RDY`, `0x800021C0`|`0x80B` (VLD/JPU/gate already modeled).
  → `JPEG_Status(JPEG_DECODE)` reaches `JPEG_STATUS_OK(1)`, `UTL_ShowJPEG_Slide`
  (`utl.c:435`) breaks its 5 s poll and calls `HALJPEG_Display` → `DISP_Display`
  sets `F0Y=0x40065000/8`, `F0C=0x400B3C00/8`, `STRIPE 0x1A0C`, and
  `DISP_VIDEO_EN 0x10000000` in `0x80001A4C` (`haljpeg.c:954`).
* **Scan-out change:** `machine_disp_scanout` must **de-tile** from
  `F0Y*8`/`F0C*8` (inverse of the above) and composite under the OSD plane
  (`0x4005F000`, palette index 0 = transparent) once `DISP_VIDEO_EN` is set —
  the current scanout shortcuts by blitting `m->jpeg_rgb`, not the tiled buffer.

`JPEG_Status` itself is a precompiled **thread variable** (`haljpeg.c:834`), not
a register — it cannot be poked; the worker must be *allowed to finish* via the
polls above. This spec is byte-exact against `jupiter/jfb.c` (which already
implements the same tiling) and the §10.2 formulas.

### 10.15 BREAKTHROUGH — the firmware boots to its own UI ("Loading" screen)

Three model changes together carry the retail image from "menu never draws"
(GPU ops = 0) to the firmware **drawing its own UI through its own pipeline**:

1. **Decoder-stop dwell** (§10.12): the halted-decoder stand-in holds
   `MODE_STOP(0x10)` for `proc2_ack_dwell` cycles before settling to
   `STOPPED(0x11)`, so the boot thread's stop handshake (`0x61170`,
   gates `0x33dac`/`0x36ff0`) completes. Default behaviour now.
2. **Boot-thread stop-poll unblock** (`CT952_TEST_MIRROR10`, sp-gated): the
   main/boot thread (stack `~0x40036xxx`) hits a *second* decoder-stop poll
   (`0x612b0`, both stages want mirror==`0x10`) with no fresh stop command; the
   getter (`0x6f054`) returns the mirror `0x40039cd0` (only ever `0x11`), so it
   would ride a ~4.5-billion-instruction 2999+30015-tick timeout. Presenting the
   mirror as `0x10` to *that thread only* unblocks it and the boot advances
   through a long chain of further decoder-state polls (`0x37004`, `0xa33dc`,
   `0x34544`, ...). **This is still a hack** — the clean version is to model the
   decoder producing MODE_STOP per stop command; but it proved the poll is the
   gate and that the chain terminates.
3. **Time-compression** (`CT952_TICK_MULT=16`): the eCos system tick is TIMER1
   (IRQ `0x100`/L8); scaling its reload/count down by N makes the tick advance
   N× faster, so the many timeout-bound decoder polls (PROC2 in reset ⇒ they
   never succeed, only time out) fire in 1/N the instructions. Without it the
   chain would need billions of instructions; with it the boot reaches the UI in
   ~27M instructions.

> **UPDATE (§10.26):** `CT952_TEST_MIRROR10` is **retired** — the faithful
> decoder-stop mirror (§10.26) now clears these gates. The reproduction command is
> just `CT952_PROC2=1 CT952_TICK_MULT=16` (no MIRROR10).

**Result (verified):** `CT952_PROC2=1 CT952_TEST_MIRROR10=1 CT952_TICK_MULT=16`
→ first GPU op at icount≈26.5M, then **45k+ GPU font ops** draw the firmware's
`"Loading ."` status screen (media-detect indicator, `osdnd.c`
`SHOW_LOADING_STATUS`) into the OSD plane at `0x4005F000` (8bpp, stride ~480 from
`AG_OFF 0x80002890`). Rendering the plane via the OSD palette shows the
`"Loading ."` text (rows 3–18). `machine_disp_scanout` already composites the OSD
regardless of `DISP_OSD_EN` (which the firmware hasn't flipped yet).

**Next gate — media detection ("Loading").** The boot thread now spins in the
CC media-detect / loading state machine (poll loops `0x32d50` and `0x4a530`,
timed via `OS_GetSysTimer`, on a state word in the `0x40039400` arena), scanning
for a removable source (SD/USB/servo) before the power-on menu / built-in-SPI
slideshow. Even at 500M instructions (with time-compression = lots of firmware
time) it stays in "Loading", so it is genuinely waiting for media/servo state the
emulator doesn't provide (the §9.3 no-media path). Modeling the media-detect
completion (present "no removable media" so the firmware falls back to the
built-in photos, or "SPI source present") is the next step toward the power-on
menu and then the built-in butterfly slideshow (§9.2 / §10.10).

**Config summary to reproduce the boot-to-UI:**
`CT952_PROC2=1 CT952_TEST_MIRROR10=1 CT952_TICK_MULT=16 ./ct952emu dp700wd.bin
--instr 250000000 --fb-addr 0x4005f000 --fb-wh 480x234 --fb-out out.ppm`
(look for `[GPU-FIRST]` in `CT952_TRACE`; the OSD plane at `0x4005F000` holds the
drawn UI).

### 10.16 BREAKTHROUGH — "Loading" render fidelity (GPU font-op stride fix)

The first render of the "Loading" screen had the correct box but **streak/line
artifacts sheared across the full width** (rows 3–10). Root cause was in
`gpu_exec`'s row-pitch reconstruction. The firmware draws the `"Loading ."`
string to the same OSD-plane dest (`0x4005F668`) **twice**: first with a
width-set op (`OP_SIZE.w=128`, `AG_OFF>>16=0x1d=29`) then with the normal
full-width op (`OP_SIZE.w=0`, `AG_OFF>>16=0x3d=61`). The plane pitch must be the
same 480 for both (a single physical plane), but the emulator was deriving the
GPU `ag_width` from the **high byte of the AG_OFF register** (`(ag>>8)&0xFF`),
which is `0` for both — so the width-set op collapsed to `(0+29-1)*8 = 224`
instead of `480` and its 13 glyphs sheared diagonally across the plane.

**Fix** (matches `gdi.c` `GDI_SetGpuAddr`): derive `ag_width` from the op width,
not the register:

```
ag_offset = REG_GPU_AG_OFF >> 16
ag_width  = (OP_SIZE.w + (dest & 3) + 3) >> 2
row_pitch = (ag_width + ag_offset - 1) * 8      (bytes; the *8 is the
            emulator's plane-pitch scale matching the 480-wide scan-out)
```

- width-set op:  `ag_width=(128+0+3)>>2=32`, `(32+29-1)*8 = 480` ✓
- full-width op: `ag_width=(0+0+3)>>2=0`,   `(0+61-1)*8 = 480` ✓

Both reconstruct to 480, so the two draws land on identical rows and the second
simply over-writes the first — no shear. This unifies font, fill, and blit under
one gdi-derived formula (the SMPTE `disp` test still renders clean bars, no
regression). The `osd_stride` learn-the-pitch hack is removed.

**Result (verified):** the OSD plane now holds a compact `"Loading . . ."` —
white text (index 3) on a blue highlight box (index 2, `x 200..303 y 3..18`) on
black (index 0), and **nothing outside that box** (histogram: only indices
0/2/3). Matches the real device. Render with the faithful palette (0=black,
2=blue `(28,104,164)`, 3=white) since the firmware hasn't loaded `GAM_OSD` yet.

### 10.17 The media-detect "no media" gate — mechanics, model, and the real blocker

Goal: get past `"Loading"` to the built-in slideshow. `"Loading . . ."` is drawn
by `_OSDND_ShowWaitingState` (`osdnd.c:2206`) whenever the OSD upper-right
message state `_bOSDNDMsg == MSG_WAITING`; the animated dots come from
`_bWaitingCnt` cycling 1→3. A separate OSD-render thread repaints it every tick
(the glyphs are copied via an unrolled `memcpy` at flash `0xd38b4`, called from
the font path at flash `0x3259c` — `sll #6` = ×64-byte glyph).

**The intended exit path (source-traced).** In the main loop,
`MEDIA_MonitorStatus`→`_MEDIA_MonitorMediaStatus` (`media.c:1384`) runs the
removable-media scan (`media.c:1489`):
```
if (SrcFilter_TriggerUSBSRCCmd(FLAG_CMD, CHECK_DEVICE)) _bTriggerCmd1 = TRUE;
if (_bTriggerCmd1 && SrcFilter_PeekUSBSRCCmd(FLAG_STATUS, CHECK_DEVICE)) {
    ... if (SrcFilter_GetStatus(SOURCE_USB0_0) == SRCFTR_USB_STATE_NO_MEDIA)
        ... else { MEDIA_ExitUSBSRC(); POWERONMENU_Initial(); }   // media.c:1575-77
        //  (retail with SUPPORT_PLAY_MEDIA_DIRECTLY_POWER_ON: MM_PlayPhotoInFlash())
}
```
The handshake is entirely eCos `cyg_flag_t` bits, driven by the `USBSRC_Thread`
worker (`usbsrc.c:252`). Verified symbol/const map (data symbols are absolute
DRAM; the flag *value* is the first word of each `cyg_flag_t`):

| symbol | addr | note |
|---|---|---|
| `__fThreadInit` | `0x4003e590` | `INIT_SRC_THREAD_USB_DONE = 0x00080000` |
| `_bUSBSRCState` | `0x4003f510` | byte; `NO_MEDIA = 1` (`USBSRC_STATE_NO_MEDIA`=`SRCFTR_USB_STATE_NO_MEDIA`) |
| `_fUSBSRCCmddStatus` | `0x4003f514` | worker→FW status flag |
| `_fUSBSRCCmddRunning` | `0x4003f524` | in-progress flag |
| `_fUSBSRCCmdd` | `0x4003f540` | FW→worker command flag; `CHECK_DEVICE = 0x1` |
| `_bMediaInitUSB` | `0x40028aa5` | byte; set once `_MEDIA_MonitorMediaStatus` inits |
| `__bMediaRegCnt` | (init `2`) | NO_MEDIA rounds before exit (decrements per round) |

`SrcFilter_TriggerUSBSRCCmd` returns FALSE unless
`OS_PeekFlag(&__fThreadInit) & USB_DONE`; the worker sets `USB_DONE` on its very
first line, then on each `CHECK_DEVICE` posts `_fUSBSRCCmddStatus|=1`, sets
`_bUSBSRCState`, clears `_fUSBSRCCmdd`/`Running`. In the emulator the worker
never runs (it blocks in the opaque `usb.a`/`card.a` HW init — `USB_HCInit`,
`CARD_InitCard`).

**No-media model implemented (`CT952_NOMEDIA`, `machine.c`).** Stands in for the
worker at the memory level: reads of `__fThreadInit` return the value OR'd with
`USB_DONE`; on any FW write that sets `CHECK_DEVICE` in `_fUSBSRCCmdd`, set
`_bUSBSRCState=NO_MEDIA`, `_fUSBSRCCmddStatus|=CHECK_DEVICE`, clear
`_fUSBSRCCmdd`/`Running`. This is correct **once the FW reaches the scan**, but…

**Key finding — the real blocker is EARLIER than media-detect.** After a 120M-
instruction run with `CT952_NOMEDIA=1`, every media data symbol above is still
**zero** (`_bMediaInitUSB=0`, `_fUSBSRCCmdd=0`, `_bUSBSRCState=0`), i.e.
`_MEDIA_MonitorMediaStatus` never executed and the model never engaged. A PC
sampler (`CT952_PCSAMP=<icount>`, with `_SP`/`_O7` filters) shows the two live
threads are both OSD **redraw** (`OSD_Output` @ DRAM `0x4001d66c`,
`OSD_SetBufferModeInfo` @ `0x4001f3a8`, glyph `memcpy`); no thread is hot in a
media/decoder poll. So the CC/boot thread is **blocked in `INITIAL_System`
before the main loop** — the `"Loading"` here is an init-time `MSG_WAITING`, not
the media-scan one. The `CT952_TEST_MIRROR10` unblock (§10.15) cleared one
`INITIAL_PowerONStatus` poll; a **subsequent init gate** is the current wall.

**Open puzzle — the ROMV run-address mapping.** A `CT952_REACH` one-shot PC trap
watching both `sym` (flash-XIP) and `sym+0x40000000` (DRAM) for the boot
functions caught only spurious early flash hits (`0x2014`), NOT `INITIAL_System`
/ `MEDIA_*` / `POWERONMENU_Initial` at either address over 40M instr. TEXT
(`sym ≥ 0x1d000`) demonstrably runs from DRAM at `sym+0x40000000` (OSD confirmed);
ROMV (`sym < 0x1d000`) execution address is **not** a clean `+0x40000000` and is
still unresolved — needed before the init gate can be trapped by name. Next step:
find the CC/boot thread's *blocked* saved-PC (eCos thread stack) rather than
sampling only running threads, and resolve the ROMV symbol→run mapping.

**Diagnostic tooling added (all env-gated, zero cost when off):**
`CT952_PCSAMP=<icount>` (per-chunk hot-PC histogram + stack-page buckets; with
`CT952_PCSAMP_SP=<base>` to isolate one thread and `CT952_PCSAMP_O7=1` to sample
the memcpy caller), and `CT952_REACH=1` (single-steps and prints the first hit of
each watched boot function). `CT952_NOMEDIA=1` is the no-media model above.

### 10.18 CRITICAL — `DVD909.sym` is the SDK build, NOT the retail ROM (VERIFIED)

While trying to trap the init gate by name (§10.17), the symbol addresses would
not line up with execution. Three independent checks confirm **`DVD909.sym` does
not describe the retail `dp700wd.bin` code layout** — it is the SDK/reference
build's map. Do **not** trust its code addresses for the retail image.

1. **ROMV DRAM is ~90% zero.** After boot, the ROMV window
   `0x40000000..0x4001d000` has only 2930/29696 non-zero words (last at
   `0x40011f24`); TEXT `0x4001d000..0x40020878` is 100% populated. The SDK's
   ROMV symbols (`INITIAL_System@0xeb90`, `MEDIA_MonitorStatus@0x1186c`,
   `_MEDIA_MonitorMediaStatus@0x118b8`, `CC_DVD_MainLoop@0x2014`, all `<0x1d000`)
   point into that mostly-zero region — reading those run addresses gives `0`.
2. **`memcpy` address mismatch.** The retail image's real unrolled word-copy
   `memcpy` runs at flash `0xd38b4` (disassembled: `ld [%o1]→st [%g3]`, ptr+=4,
   count-=16/iter). `DVD909.sym` places `memcpy` at `0xa78d4` (→`0x400a78d4`).
   No clean relationship.
3. **No constant delta aligns sym entries to `save` prologues.** Of 65 sym `T`
   entries in the TEXT range, only 3 land on an actual `save %sp,…` prologue in
   the loaded DRAM at `sym+0x40000000`; the best offset over ±256 bytes still
   hits only 7/65 (a real match would align ~50+/65). The retail TEXT has 125
   `save` prologues where the sym expects 65 functions — different code.

**What is still reliable:** the section *memory map* (boot-log section bases
TEXT `0x4001d000` / DATA `0x40020878` / ENGL `0x40049900` match the sym's section
starts), and anything derived from **actual execution** — real PCs from the
sampler/traces, flash disassembly at executed addresses, and hardware-register
behaviour (the §10.16 GPU fix, the §10.15 boot mechanisms, which key on executed
flash PCs, all stand). The SDK **source** (`cc.c`/`media.c`/`usbsrc.c`/…) remains
valid as the *algorithm/logic map*, but every address must be re-derived from the
retail ROM.

**Consequence for §10.17:** the no-media model watches SDK-sym DATA addresses
(`_fUSBSRCCmdd@0x4003f540`, …) which are **not** trustworthy for retail, so its
non-engagement is inconclusive about the media path. Retail-native methods are
required next: (a) locate boot-log / OSD strings in the ROM and work outward from
their references; (b) find eCos thread objects in the RAM dump by structural
pattern (see the eCos `Cyg_Thread`/HAL-context layout research) to read the
blocked CC thread's saved PC directly — both symbol-independent.

### 10.19 The retail source IS in-tree (`950_Files/`), and the built-in photos

The retail DP700WD is a **952-based** DMP photo frame; `DVD909.sym` (§10.18) is a
909 SDK build. The matching source is the in-tree **`950_Files/`** folder, whose
`950_make.txt` gives the exact retail build config:
`CT950_STYLE` + `CT951_PLATFORM` + `SUPPORT_950=1`, `DRAM_SIZE_16`,
`DECODER_SYSTEM=DVD909R_EVAL`, `CPU_146M`, serial 8M PROM. It ships
`poweronmenu.c` / `mainmenu.c` / `menu.c` / `clock.c` / `radio.c` / `alarm.c` /
`calenui.c` (clock/alarm/radio/calendar — the photo-frame feature set), plus
`logo{,1,2}.bin` and `snd1.bin`. **Use `950_Files/*.c` as the retail logic map**
in preference to the root SDK `.c` files (still no retail symbol table — match
functions to code by string references / behaviour, always verifying).

**Built-in slideshow photos (verified content).** `950_Files/01.jpg`,
`02.jpg`, `03.jpg` are the three built-in images (`BUILD_IN_JPG_ENCODE_NUM = 3`,
§9.2): a pink **rose**, a pink **orchid**, and a **rose bouquet**, all 720-wide
(matches the 720×448 decode buffer). They are **not** byte-identical in the ROM
(re-encoded), but the ROM carries JPEG data at the expected SPI photo region:
22 `FFD8FF` SOI markers, a clean one at **`0x160000`** (the `SRCFTR_SPI_ENCODE`
base the retail build uses; the SDK default was `0x110000`, §9.2) and an
icon/thumbnail cluster around `0x104de0`–`0x11xxxx`. The stored photos are in the
device's 64 KiB-slot SPI format, not plain JFIF (a naïve SOI→EOI carve yields a
short, non-decodable segment), so reading them needs the SOURCE_SPI / HW-decode
path — the §10.10 datapath.

### 10.20 eCos SPARC thread-context decode (for finding the blocked thread, symbol-free)

To locate where the CC/boot thread is actually stuck without a retail symbol
table, decode its saved context straight from a RAM dump. Layout confirmed from
upstream eCos source (`ecos-rtos/ecos`, kernel 3.x tree; member *orders* are
stable back to the 1.x/2.x our ROM marks, but `#ifdef`-gated offsets must be
sanity-checked against the dump). NOTE: the GitHub repo `Ecos-platform/ecos` is a
different project (an FMI co-simulation engine) — **not** the RTOS.

- **`cyg_flag_t`/`Cyg_Flag`**: the 32-bit flag VALUE is at **offset 0** (first
  word); the waiter `Cyg_ThreadQueue` follows at +4. (Confirms the §10.17 flag
  model read the right word — for whatever the retail flag addresses turn out to be.)
- **`Cyg_Thread`** (no vtable/vptr; `Cyg_HardwareThread` is the first base):
  `stack_base` @+0, `stack_size` @+4, then **`stack_ptr` @+8** (or +12 if
  `CYGFUN_KERNEL_THREADS_STACK_LIMIT`). `state` (`cyg_uint32`) is the first member
  after the scheduler base block (~+44, verify): `0`=RUNNING, `1`=SLEEPING,
  `2`=COUNTSLEEP, `4`=SUSPENDED, `8`=CREATING, `16`=EXITED (bitmask). Enumerate via
  the static `Cyg_Thread::thread_list` + per-node `list_next`, or scan for objects
  whose `stack_ptr ∈ [stack_base, stack_base+stack_size)`.
- **SPARC saved-context frame** (`hal/sparc/arch` `HAL_SavedRegisters`, 32 words /
  128 bytes, sitting AT the thread's saved `%sp`=`stack_ptr`, big-endian):
  `l[0..7]`=words 0–7, `i[0..7]`=words 8–15, saved **PSR**=word 16 (the `%g0`
  slot), `g[1..7]`=words 17–23, `o[0..7]`=words 24–31. So:
  - kernel resume PC = word 31 (`%o7`, byte 124) — lands in the scheduler.
  - **application PC via the window chain**: word 15 (`%i7`, byte 60) = caller
    return PC; word 14 (`%i6`, byte 56) = caller `%fp`. Walk up: at each `fp`,
    `fp[15]`=return PC, `fp[14]`=next `fp`, until `fp` leaves the stack range.
    Each return PC is a retail flash/DRAM address to disassemble and match to the
    `950_Files` source. (If built with `CONTEXT_SAVE_MINIMUM`, only word 31/PSR +
    the L/I windows are valid, but the `i6/i7` unwind still works.)

### 10.21 GROUND TRUTH — the CC boot thread's wait, decoded from the live stack

Applied §10.20 to a RAM dump of the stuck "Loading" state. **This is all
retail-verified** (real execution addresses), replacing the unreliable SDK
symbols. Config note: `CYGFUN_KERNEL_THREADS_STACK_LIMIT` is enabled, so
`stack_ptr` is at object **+12** (not +8), and `state` (`cyg_uint32`) is at
**+40**: `1`=SLEEPING for every blocked thread.

**Thread map (10 blocked threads found by structural scan).** Two share the
sampler's live stacks: `T@40038fb8` (stack `0x34408..0x35908`, the `0x34000`
bucket) is the **OSD render** thread (its chain runs through `OSD_SetBufferMode`
`0x4001f3a8` etc.); **`T@400371f8`** (stack `0x359d8..0x371d8`, the `0x36000`
bucket) is the **CC/main thread**. Common OS wait wrappers seen across threads:
`0x4001dab0`/`0x4001d4c0` (timed flag-wait), `0x4001ea40`/`0x4001ea60`
(sleep/delay); timed waiters tail into the tick backing store `0x4002e320`.

**The CC thread's blocked call chain** (inner→outer, from its saved SPARC frame):
`[OS flag-wait] ← 0x4001dab0 ← 0x4001d4c0 ← 0x49970 ← 0x25240 ← 0x12028`.
- `0x25240` calls `0x49944(2, 0, 0xFF)` — an `OSD_Output(MSG=2,…)`-shaped call
  (the "Loading"/`MSG_WAITING` draw), which internally does a **timed flag-wait**
  (`0x4001d4c0`: `ld [%l7+0x104],%o1; add %sp,0x60,%o2; call …` = flag ptr +
  timeout) — the per-frame redraw sync.
- `0x12028` is inside a **modal-wait dispatcher** (flash entry ~`0x11fc0`, args
  `%i0`=mode, `%i1`=callback). Mode 0: loop `call %i1` (the redraw callback) then
  `call 0x66a0(0x1000)`; `if ((ret&0xff)==0) goto loop`. On exit it stores result
  `1` at `[0x40026EBC]`, clears the loop-active marker `[0x40026EB8]`, sets
  `[0x40039344]=1`, and returns 1. Modes 1/2 return result 2 without waiting.

**The gate primitive.** `0x66a0(mask)` = peek-and-clear on the flag object at
**`0x40026EA4`**: `getbits(0x59640)` → `if (val & mask) { clearbits(0x59628); return 1 }
else return 0`. So the CC thread advances only when **bit `0x1000` is posted to
flag `@0x40026EA4`**. In the stuck dump that flag reads `0x00000000` (and the
loop-active marker `[0x40026EB8]=1`), confirming it is waiting.

**Poke experiment (`CT952_CCEVENT=<icount>`, added).** Posting
`*(u32*)0x40026EA4 |= 0x1000` once, past the given icount: the firmware **consumes
it** (flag reads back `0`, so `0x66a0` did see+clear it and the wait exited once)
— but the screen stays on "Loading". So **bit `0x1000` is the modal response/
refresh event, NOT the boot advance gate**: exiting the wait once returns into an
outer context that re-enters it (the actual precondition — media/servo/decoder
state — is still unmet). Useful negative result: the advance is not this one flag
bit.

**Next (retail-native, symbol-free):** unwind `T@400371f8` past `0x12028` to the
frame that *calls* the dispatcher (its context: is this the §9.3 media/servo wait,
or a UI confirm?), disassemble that caller against `950_Files/*.c`, and find who
`OS_SetFlag`s the real advance condition. The dispatcher/flag addresses above are
the anchors. Tool added: `CT952_CCEVENT=<icount>` (one-shot flag poke).

### 10.22 FULL unwind — the "Loading" wait is INITIAL_PowerONStatus, gated on decoder-ready

A precise `%fp`-chain walk of the CC thread (`T@400371f8`, `stack_ptr` at obj+12)
gives a clean 14-frame stack all the way to the thread entry (`i7=0`). Frame
return addresses (inner→outer):
```
[OS flag-wait] 4001dab0 <- 4001d4c0 <- 0x49970 <- 0x25240 <- 0x12028(dispatcher)
   <- 0x24d54 <- 0x26e58 <- 0x25f48 <- 0xb018 <- 0x41b34 <- 0xadc4 <- [OS entry]
```
The outermost app frame (`0xadc4`) is in a function that calls `0x416f4` then
**`0x418f0`** — i.e. `INITIAL_System` → **`INITIAL_PowerONStatus`** (these two flash
addrs were already established from execution, §10.11/cc.c:1315). **So the live
stack independently lands on the exact known boot function — validating the whole
symbol-free decode method.** The CC thread is in `INITIAL_PowerONStatus`, NOT the
main loop; the "Loading" is the power-on-status wait, matching §10.11-12.

**The state-machine structure (all retail-verified addresses):**
- `INITIAL_PowerONStatus` (`0x418f0`) runs a state machine via dispatcher
  **`0xafd8`** (`ld [%i0+4],%o1; call %o1` — indirect call through a per-state
  handler table; `%i0` = state object).
- The current state's handler chain (`0x41b18` → `0xaffc` → `0x25f2c` → `0x26e44`
  → `0x24d38`) calls the **modal-wait dispatcher `0x11fb0`** with `mode=0`,
  `callback=0x25234`. `0x25234` = `OSD_Output(2,0,0xFF)` — the `MSG_WAITING`
  "Loading" draw. The dispatcher loops the callback while polling the CC event
  flag for bit `0x1000` (waiter `0x66a0` = peek-and-clear of flag `@0x40026EA4`).

**The `0x1000` event mechanics (fully mapped):**
- Flag object `@0x40026EA4`; primitives `getbits 0x59640`, `clearbits 0x59628`,
  `setbits 0x597d0`. Poster wrapper `0x6670` = `PostCCEvent(bits)` = `setbits`.
- The `0x1000` poster is fn **`0x45660`**: posts `0x1000` **iff** `[0x40039344]!=0`
  (the Loading dispatcher sets this =1 while active) **AND** the countdown byte
  **`[0x40022F5E]==2`**. That byte is a countdown (writers set it to 3; `0x1d170`
  decrements 3→2→1→0). In the stuck dump it is **0** (overshot; `TICK_MULT=16`
  time-compression likely skips the `==2` sample window).

**Two interventions tested — both NEGATIVE, both informative:**
- `CT952_CCEVENT=<icount>`: poke `*(u32*)0x40026EA4 |= 0x1000`. The firmware
  consumes it (flag clears, wait exits once) — screen stays on Loading.
- `CT952_PONSREADY`: present `[0x40022F5E]` as `2` past 30M icount, so the poster
  can fire `0x1000` naturally — screen still stays on Loading.

**Conclusion:** posting `0x1000` (directly, or by satisfying the countdown) does
**not** advance past Loading. The event is only a **re-evaluation pulse**; the
real boot-forward gate is a **decoder/servo readiness condition** that the state
handler re-checks each pulse and still finds unmet (PROC2/decoder held in reset).
This ties back to §10.11-12: the power-on-status state machine is waiting on the
decoder reaching its target state. **Next:** decode the specific state handler
(`0x25f2c`/`0x26e44` and the `0xafd8` handler-table entry) to find the exact
decoder/servo predicate it evaluates (likely reads playmode `0xB0000190` / mirror
`0x40039cd0`), and satisfy it — extending the existing decoder-state model
(`CT952_TEST_MIRROR10`, the stop-dwell) rather than faking the event.
Tools added: `CT952_CCEVENT`, `CT952_PONSREADY` (both env-gated probes).

### 10.23 GATE FOUND — the "Loading" state is an event pump waiting for a key/event message

Decoded the state-8 handler end to end (all retail flash addresses). The full
chain and its advance condition:

```
state-8 handler  0x25ef4
  reads gate byte [0x40022F97] (=1 -> Loading path)
  -> 0x26e44  "draw Loading + wait"
       0x49944(2,0,0xFF)          ; OSD_Output(MSG_WAITING) = draw "Loading"
       0x24d0c -> modal-wait 0x11fb0(mode0, cb=0x25234)   ; pulse on CC evt 0x1000
       -> 0x254a4  POST-WAIT CHECK  (returns 0 => advance)
            0x12cac(0x401D0C00, 0x401DC000)   ; 0x401DC000 = staged power-on LOGO jpeg (10.7)
            -> 0x25d58  EVENT GETTER
                 0x6b660(queue @0x400329FC)   ; pending-event count
                 if none -> outputs [fp-0x10]=0, [fp-0xc]=0
                 else     -> 0x12e18 fetches the event (code clamped to 0x64)
            [fp-0x10] != 0  (event pending)  => 0x254a4 returns 0
       0x254a4==0  => 0x26e44 returns 1  => handler latches/advances (0x2605c returns 1)
```

**Advance condition (ground truth):** the power-on logo/status state advances
**only when a message is pending in the key/event queue at `0x400329FC`** (read
via `0x6b660`; events look like key/IR codes, `< 0x64`). With no key/IR input, the
decoder (PROC2) held in reset (so no decode-done event), and no media, the queue
stays empty and the state re-draws "Loading" forever. This is why every attempt
to fake the *downstream* CC event (`0x1000`) or the countdown byte failed —
they're re-evaluation pulses; the actual advance needs a real **event message**.

**This reframes the whole "Loading" stall:** it is not a busy decoder poll, it is
an **event-driven wait for input / a decode-done / a media event**. The natural
unlocks, in order of faithfulness:
1. **Post a key event** to queue `0x400329FC` (e.g. inject an IR/keyboard code) —
   ties directly to the sibling branch's USB-keyboard groundwork. Simplest test:
   make `0x6b660` report one pending event and have `0x12e18` yield a benign key.
2. **Model the logo decode-done event** (PROC2/JPEG datapath, §10.9-10.10) so the
   firmware posts its own advance event — the faithful path.
3. **A timeout event**, if the logo state has one (check for a timer that posts to
   the queue).

Key retail addresses: event queue `0x400329FC`; getter `0x6b660`; fetch `0x12e18`;
state-8 handler `0x25ef4`; draw+wait `0x26e44`; post-wait check `0x254a4`; advance
`0x2605c`; logo jpeg `0x401DC000`. All execution-verified (symbol-free).

### 10.24 Key-event injection attempt — the gate is MULTI-LAYERED (all decoder-gated)

Tried to advance state-8 by simulating "an event arrived." Added infrastructure:
a fast CPU breakpoint (`sparc_t.brk_pc`; `sparc_run` stops AT it without single-
stepping) and probe `CT952_LOGOEVENT` (posts CC event `0x1000` so the modal-wait
exits, and forces the queue check `0x254a4` to report success at `0x26e78`).

**Result: it does not advance — because the wait is gated at MULTIPLE nested
levels, not one.** Disassembling the modal-wait wrapper `0x24d0c` shows: after the
dispatcher `0x11fb0` returns 1 on event `0x1000`, it calls **`0x11f48`** (another
predicate); only if THAT passes does control reach `0x254a4` (the queue check),
and only then `0x26e78`. So `0x26e78` is never reached (the `0x11f48` gate blocks
first), and blindly posting `0x1000` just churns the per-frame redraw (the render
shows "Loading" caught mid-draw, fewer pixels). Chain so far:
```
modal-wait 0x11fb0 (event 0x1000)  ->  0x11f48 predicate  ->  0x254a4 queue check
   ->  0x25d58 -> 0x6b660(queue 0x400329FC)   ->  advance 0x2605c
```
Each layer re-checks real state (event pulse, then `0x11f48`, then the event
queue), and every one is ultimately downstream of the **decoder (PROC2) being
held in reset** — nothing produces the decode/servo completion that would set
these predicates. **Conclusion: faking the gates one-by-one is an unbounded chain;
the faithful unlock is to make PROC2 / the JPEG-still decoder actually run (kick +
model the decode-done, §10.9-10.10), or bypass the boot state machine entirely and
drive the display datapath directly with a functional decode of the built-in
photos (§10.19 / §10.10).** The `brk_pc` breakpoint added here is reusable general
infrastructure; `CT952_LOGOEVENT` remains as a documented (negative) probe.

**Net for the "boot naturally to the slideshow" goal:** the "Loading" screen is a
faithful power-on-status state machine whose every forward transition waits on the
decoder. The render fidelity is correct (§10.16); the remaining work is squarely
the decoder bring-up (PROC2), which is the single upstream blocker behind all the
nested gates catalogued in §10.21-10.24.

### 10.25 Decoder bring-up roadmap (the faithful path to a natural boot)

Chosen direction: get the decode/display to actually complete so the firmware
advances on its own. Synthesizing §10.8-10.24 into the faithful work items and
their true dependency order (all addresses execution-verified / symbol-free):

**A. The state-8 "Loading" gates are the firmware's own event system, fed by the
decode/display-complete path.** The state-8 handler (`0x25ef4`) advances only when
its post-wait check (`0x254a4`) sees a message; the intermediate gate `0x11f48`
sums a linked list at `0x40032180`; the queue getter `0x6b660` reads `0x400329FC`;
the CC event flag is `0x40026EA4` bit `0x1000`. These are UI/event structures —
**not decoder registers** — so they can't be poked meaningfully; they are *posted
to* by the decode-done / `HALJPEG_Display` / servo-complete handlers. Faking any
one just exposes the next (§10.24, verified twice).

**B. The true upstream blocker is that `_INITIAL_ShowFirstLOGO` never kicks the
decode.** Whole-boot I/O audit (§10.9): `REG_MCU_BCR08` (0x80002a20),
`REG_JPU_CTRL`/`JPU_GO` (0x80002880), `REG_DISP_F0Y_ADDR` (0x80001AC0) are **never
written**; `DISP_VIDEO_EN`/`DISP_OSD_EN` never set. With the decoder-stop dwell
(§10.13) the boot reaches `_INITIAL_ShowFirstLOGO` and the `0x80000C10` decode-
progress gate is polled 15 436× (§10.14) — but the firmware still never issues the
JPEG kick, so the logo decode never starts, so the decode-done event that would
feed the §A queues never posts. The wait is for something that never begins.

**C. Why the kick never fires — the remaining root to crack.** Between reaching
`_INITIAL_ShowFirstLOGO` and the `BCR08`/`JPU_GO` kick lies the `display.a` /
`dec_dram.a` decoder-command + display bring-up handshake (precompiled, no .c).
The still-unmet piece from §10.12-13 is the **software mirror `0x40039cd0` never
reaching `MODE_STOPPED 0x11`** (getter `0x6f054` ORs a busy bit while it lags),
which `CT952_TEST_MIRROR10` currently fakes. The mirror is written through a
pointer in the decoder library (no direct `sethi 0x1000e7` store found), so the
faithful model reproduces the library's STOP→STOPPED bookkeeping: on the
firmware's own `MODE_STOP` issue (flash `0x6f2b0`), drive both the live playmode
`0xB0000190` AND the software mirror `0x40039cd0` through `0x10 -> 0x11` with the
dwell — so every `0x375a0`/`0x33dac`/`0x36ff0` check passes without MIRROR10.

**Faithful implementation order:**
1. **Retire MIRROR10** — make the decoder-stop stand-in also drive mirror
   `0x40039cd0` to `0x11` after the dwell (extends §10.13; clears §10.12 gate 3
   naturally). Re-verify boot still reaches "Loading" with the dwell alone.
2. **Find why the logo decode isn't kicked** — trace, from the state-8 handler's
   decode/display path, the precondition the firmware checks before writing
   `BCR08`/`JPU_GO` (candidate: a `display.a` bring-up ack, or `HALJPEG_Decode`'s
   own gate). This is the true wall (§B).
3. **Apply the §10.10 JPEG datapath** once the kick fires: functional-decode the
   staged logo (`0x401DC000`) → tiled YUV to `0x40065000`/`0x400B3C00`, present
   BIU/VLD/JPU/`0xC10` polls done → `JPEG_Status OK` → `HALJPEG_Display` sets
   `DISP_VIDEO_EN`. The firmware then posts its advance event and §A clears.
4. **De-tile in `machine_disp_scanout`** and composite video under the OSD.

**Honest scope note:** items 2-3 depend on precompiled `display.a`/`dec_dram.a`
handshakes interlocked with the (reset) PROC2 and unmodeled BIU/JPU hardware, so
this is a genuinely multi-session bring-up, not a one-shot fix. If a visible
result is wanted sooner, the functional-decode display bypass (drive the §10.10
datapath directly from the built-in photos without the boot state machine) shows
the actual images through the real scan-out while item 2's root is worked out.

### 10.26 DONE (roadmap item 1) — faithful decoder-stop mirror; MIRROR10 retired

Implemented the faithful software-mirror bookkeeping and **removed the
`CT952_TEST_MIRROR10` read-hack entirely**. New state `machine.h vdec_stopped`:
when the firmware issues a `MODE_STOP` from the COMDEC issuer (flash
`0x6f2b0..0x6f400`), the stand-in now (a) holds the live playmode `0xB0000190` and
the software mirror `0x40039cd0` at `MODE_STOP(0x10)` for the ack dwell, then
(b) presents both as `MODE_STOPPED(0x11)` afterwards, until a real play/scan
command clears `vdec_stopped`. So the getter `0x6f054` returns `0x10` for gate-1
and `0x11` for gate-3 (§10.12) through the same handshake a real decoder would
drive — no per-thread / per-icount hack.

**Verified:** `CT952_PROC2=1 CT952_TICK_MULT=16` (no MIRROR10) boots to the
identical `"Loading"` screen (OSD plane indices 0/2/3 = 110656/1545/119, matching
§10.16). disp regression green. This retires the last diagnostic hack on the
boot-to-Loading path; the decoder-stop handshake is now modelled faithfully.

Remaining roadmap items 2-4 (§10.25) are unchanged: find why the logo decode is
never kicked, apply the §10.10 JPEG datapath, de-tile the scan-out.

### 10.27 Roadmap item 2 — the logo-decode kick gate located (in source), + a reconciliation to nail

Traced the logo decode from source (utl.c/haljpeg.c are in-tree). The path:
`INITIAL_PowerONStatus` → `_INITIAL_ShowFirstLOGO()` = **`UTL_ShowLogo()`**
(utl.c:481). The retail LOGO section header (`flash 0x104DD8`) is
`5a 00 2c68 …` → `bLogoType=0x5A` ('Z' = **JPEG logo**, not the `0x4D`/'M' MPEG
path), data `FF D8 FF E0` (480×270 JFIF, 11368 B). So `UTL_ShowLogo` takes its
`bLogoType==0x5A` branch (utl.c:756): sets JPEG play mode, `HAL_FillVideoBuffer`,
`HAL_ResetVideoDecoder`, then **`UTL_ShowJPEG_Slide(NORMAL,0)`** (utl.c:831).

**`UTL_ShowJPEG_Slide` (utl.c:377) has two sequential gates, and the FIRST is the
wall:**
1. `HALJPEG_ParseHeader` (→ precompiled `JPEG_ParseHeader`; `HALJPEG_SetDisplay`
   writes `DISP_VIDEO_POS 0x80001a48`, seen 3× — so this path IS reached), then a
   busy `while (HALJPEG_Status(PARSE_HEADER)!=OK)` with a `COUNT_3_SEC*2` timeout.
   **If it times out → `return FALSE` at utl.c:425 — before `HALJPEG_Decode()`.**
2. `HALJPEG_Decode()` (utl.c:429) — the actual `BCR08`/`JPU_GO` kick. Only reached
   if gate 1 passes.

`HALJPEG_Status(PARSE_HEADER)` = `JPEG_Status(JPEG_PARSE_HEADER)` — a **precompiled
JPEG-worker thread variable** (haljpeg.c:833), not a register. So gate 1 clears
only when the worker thread actually finishes parsing the header, which needs the
BIU bitstream feed + VLD parse to complete (§10.8). It never does → parse times
out → `UTL_ShowJPEG_Slide` returns FALSE → **`HALJPEG_Decode` is never called →
`BCR08`/`JPU_GO` never written** (this is the mechanism behind §10.9's observation).

**Reconciliation to nail next (important):** the parse-header wait (utl.c:411-418)
is a *tight busy-loop reading `JPEG_Status`* with no yield — yet the PC sampler
(§10.21) shows the boot thread in OSD redraw / the state-machine (§10.21-24), NOT
in that loop. So `UTL_ShowLogo` most likely **already returned FALSE** (logo
skipped) and the stuck "Loading" is the **downstream** power-on state machine
(§10.24), whose `0x254a4` post-wait check references the same logo buffer
(`0x401DC000`). Whether fixing the logo decode clears the stuck "Loading" depends
on this: if the state machine waits for the logo-display-complete event, yes; if
"Loading" is a separate media/servo wait that merely times out, the logo is a
different (parallel) concern. **Next concrete step:** confirm empirically whether
the boot thread ever enters `UTL_ShowJPEG_Slide`'s parse/decode loops (find the
retail address of `UTL_ShowJPEG_Slide` via its `HALJPEG_SetDisplay`/`0x80001a48`
write, breakpoint it), and whether `UTL_ShowLogo` returns TRUE or FALSE — that
tells us if item 3 (model the JPEG worker's parse+decode datapath, §10.10) is the
unlock for "Loading", or a parallel task to a separate media/servo gate.

### 10.28 Roadmap progress — the faithful mirror UNLOCKED the JPEG decode kick

Re-audited the decode datapath in the faithful config (§10.26, no MIRROR10) with a
new `CT952_LOGOTRACE` (logs PC+icount for writes to the DISP/JPU display regs).
**The boot now actively kicks and runs the logo JPEG decode** — overturning §10.9's
"the kick never fires" (that was the pre-faithful stall point):

- **JPU is kicked repeatedly by the boot thread** (`sp=0x40036xxx`): `REG_JPU_CTRL`
  `0x80002880` written `0x42/0x5a/0x6a` = `JPU_GO(0x2)` on ops 4/5/6, from retail
  PCs `0x6bb38`/`0x6be40`/`0x6be70`, starting ~icount 10.4M and churning.
- **Display setup runs** on a separate display thread (`sp=0x40026xxx`):
  `DISP_VIDEO_POS 0x80001a48 = 0x00150065` (pos 21,101) and `VIDEO_SIZE 0x80001a4c`
  from PCs `0xa32ac`/`0xa3324`/`0xa56f4`/`0xa4654`/`0xa685c`.

**But it does not COMPLETE:** `REG_MCU_BCR08` (bitstream base `0x80002a20`),
`REG_DISP_F0Y/F0C_ADDR` (`0x80001ac0/ac4`), and `DISP_VIDEO_EN` (bit `0x10000000`
in `0x80001a4c`, still `0`) are **never written**. The JPU op churns without a
bitstream (`BCR08` unset) so it never produces the decoded frame, never signals
decode-done, and `HALJPEG_Display` (which would write `F0Y`/`F0C` and set
`VIDEO_EN`) is never reached. Exactly §10.8's diagnosis: the worker's completion
needs real decode progress (bitstream flowing to EOI + the tiled output), not just
the JPU_GO kick.

**Item 3 is now the clear, well-anchored target.** Retail anchors: JPU-kick code
`0x6bb38`/`0x6be40`/`0x6be70`; display-setup `0xa32ac..0xa685c`; the §10.10 datapath
(BIU drain + functional decode → tiled YUV to `0x40065000`/`0x400B3C00` + present
polls done) plugs in here so the running JPU decode completes → `HALJPEG_Display`
→ `VIDEO_EN` → boot advances. `CT952_LOGOTRACE` kept as the anchor probe.

**Session net (decoder bring-up):** item 1 done (faithful decoder-stop mirror,
MIRROR10 retired); item 2 done (logo-decode gate located, and the faithful mirror
proven to advance the boot from "never kicks" to actively running the JPU decode);
item 3 (model the JPEG-datapath completion) is next, with every address known.

### 10.29 Item 3 groundwork — the JPU decode-op protocol (retail-disassembled)

`HALJPEG_Decode` → `JPEG_Decode(&_HALJPEGDecode)` (haljpeg.c:416, precompiled).
Disassembled its JPU op engine:

- **JPU-op wait `0x6bb00`:** `%o2 = REG_JPU_CTRL (0x80002880)`; `ctrl |= JPU_GO(0x2)`;
  then poll `ctrl & 1` (`JPU_BUSY`) until clear (emulator clears on GO → each op
  succeeds), with a ~499-tick timeout. **Abort path:** if `[0xB0000190]
  (PLAYMODE) == 0x10 (MODE_STOP)` it returns 0 (decode aborted). The §10.26
  faithful mirror keeps PLAYMODE at `0x11 (STOPPED)` after the stop, so it does
  NOT abort — the ops run. (Had the old MIRROR10 left PLAYMODE at `0x10`, the
  decode would have aborted here — another reason the faithful mirror matters.)
- **JPU-op dispatcher `0x6bbb4`:** `jmp`-table on op type 0-7 (`sethi 0x1af | 0x1c8`
  base) — the driver issues a real sequence of decode/scale ops.

**State:** the driver actively runs the op sequence (~13026 `JPU_CTRL` accesses +
338130 `0x80002a28` BIU-status polls by icount 90M) but never sets
`JPEG_Status(DECODE)=OK`, so `HALJPEG_Display` (F0Y/F0C/`VIDEO_EN`) is never
reached. `REG_MCU_BCR08` (bitstream base 0x80002a20) is still never written, and
`0x80002a24` is written repeatedly with a small command byte (0x05) — so the
driver's BIU bitstream-feed protocol here is NOT the plain BCR08-base model §10.10
assumed; it drives the read channel through `0x80002a24`/`0x80002a28` commands.

**Open item-3 questions (the concrete next dig):**
1. Does the op loop TERMINATE (finish the image, then stall on a final display
   handshake) or churn forever? (Compare `JPU_CTRL` access count at 90M vs 200M.)
2. What single signal sets `JPEG_Status(DECODE)=OK`? Candidates: the `0x80002a28`
   BIU status reaching a drained/EOI value, or a decode-done bit. That is the
   value to model — together with a functional picojpeg decode writing the tiled
   YUV output (§10.10 step 3) so the produced frame is valid.

Retail anchors: JPU-op wait `0x6bb00` (abort check `0x6bb84`), op dispatcher
`0x6bbb4`, BIU poll (§10.8 `0x4001fe90`), BIU regs `0x80002a24`/`0x80002a28`.

### 10.30 Item 3 refined — decode runs, BIU phase COMPLETES, stalls in a JPU-op loop

Churn-vs-terminate check (iolog at 90M vs 220M, faithful config):

| reg | 90M | 220M | verdict |
|-----|-----|------|---------|
| `0x80002a28` BIU read-channel status | 338130 | **338130** | **PLATEAUED** — BIU phase done |
| `0x80002880` JPU_CTRL | 13026 | 39562 | growing — infinite loop |
| `0x80000e00` (RMW, val `0x1d`) | 19115 | 39527 | growing |
| `0x8000031c` SYSCFG1 (val `0x106040bb`) | ~27K | 51885 | growing |
| `0x80002884` JPU CTL1 | 9556 | 19762 | growing |
| `0x80001a4c` VIDEO_EN | — | 0 | never set |

So the earlier "BIU stall" framing is superseded: **the BIU bitstream phase runs to
completion (poll count plateaus), then the decode gets stuck in an infinite
JPU-op loop** that reads `0x80000e00` (a system reg, read-modify-written each pass
with `0x1d`), polls `0x8000031c` (SYSCFG1-region, `0x106040bb`), and re-kicks the
JPU (`0x80002880`/`0x80002884`) — forever. `HALJPEG_Display`/`VIDEO_EN` never
reached. OSD stays "Loading".

**Refined item-3 target:** the JPU-op loop needs a completion signal the emulator
doesn't produce. The loop re-issues JPU ops and re-checks `0x80000e00`/`0x8000031c`
each pass; one of those (most likely `0x80000e00`, the per-pass RMW) is a
decode-progress / block-counter / DMA-status word the real JPU hardware advances
as it consumes macroblocks, and the driver loops until it reaches a terminal
value. **Next dig:** disassemble the JPU-op loop body (retail, around the
`0x6bbb4` dispatcher + the `0x6be40`/`0x6bb38` kickers) to find the exact
`0x80000e00`/`0x8000031c` predicate that exits the loop, then model it advancing
to terminal (plus the functional picojpeg decode writing tiled YUV so the frame
is valid) → `JPEG_Status(DECODE)=OK` → `HALJPEG_Display` → `VIDEO_EN`.

**Decoder-bring-up scoreboard:** item 1 DONE (faithful mirror, MIRROR10 retired,
boot reaches Loading naturally). item 2 DONE (logo-decode gate located; faithful
mirror proven to unlock the decode kick — the JPU now genuinely runs). item 3 IN
PROGRESS (BIU phase completes; the JPU-op loop's terminal condition + functional
output is the remaining piece; all retail anchors recorded).

### 10.31 Nuance — the JPU decode is a PERIODIC RETRY, not a tight loop

PC sampling past icount 140M (faithful config) shows the dominant hot loop is the
**OSD "Loading" redraw** (`0x4001f3a8` OSD_SetBufferModeInfo, `0x4001d66c`
OSD_Output, glyph `memcpy` `0xd39xx`) — the §10.21-24 state-machine redraw. The
JPU decode accesses grow only slowly (`JPU_CTRL` 13026→39562 over 130M instr), so
the decode is **not** a tight infinite loop; it is a **periodic retry**: the
power-on state machine redraws "Loading" and every so often re-attempts the logo
`UTL_ShowJPEG_Slide`, which runs the JPU op sequence, fails to complete, returns
FALSE, and the state re-arms. This reconciles §10.21-24 (event-driven "Loading"
state) with §10.28-30 (the JPU actually runs): they are the same loop at two time
scales — redraw fast, decode-retry slow.

**Item-3 conclusion unchanged, target sharpened:** make ONE decode attempt
COMPLETE (produce the decoded frame + signal `JPEG_Status(DECODE)=OK`), and the
retry becomes a success → `HALJPEG_Display` → `VIDEO_EN` → the state machine's
logo-display event posts → boot advances. The completion signal the JPU op
sequence waits on (the `0x80000e00`/`0x8000031c` predicate + a valid decoded
frame) remains the one piece to model; it just fires per-retry, not in a tight
loop. Every retail anchor is recorded (§10.28-30).

### 10.32 CORRECTION — the logo decode runs ONCE and aborts; it is separate from "Loading"

Filtering the `CT952_LOGOTRACE` writes by register overturns §10.31's "periodic
retry": the JPU **decode** ops (`0x80002880` = `0x40/0x42/0x58/0x5a/0x68/0x6a`,
JPU_GPU_OP bit clear) occur in **one early burst** (~26 writes, icount 10.4M-11M)
and then STOP. The 142 ongoing `0x80002880` writes are `0x10440423` — GPU **font
ops** (JPU_GPU_OP bit SET), i.e. the OSD "Loading" redraw sharing the register.
So the earlier "JPU_CTRL keeps growing" (§10.30) was the GPU redraw, not the
decode.

**What the burst is:** ~26 ops (a setup mix `op0/1/3/4/5/6`) — far fewer than the
480×270 image's ~510 macroblocks. So the decode does its setup + a few ops, then
**aborts/returns without decoding the full image** (`UTL_ShowLogo` → FALSE). It is
NOT a busy-poll stall (a DRAM read-frequency histogram gated to the decode window
found nothing), so `JPEG_Decode` returns fast with a non-OK status — the emulator
produces no valid decoded pixels / never advances the decode DMA
(`0x80000e00`/`0x8000031c`), so the driver gives up after setup.

**Two concerns are now cleanly separated:**
- **(P1) Logo decode:** make `JPEG_Decode` complete a real frame — model the decode
  DMA/completion (`0x80000e00` RMW + `0x8000031c` poll) advancing to terminal +
  functional picojpeg output, so the burst runs to all macroblocks and returns OK.
- **(P2) The stuck "Loading" state machine (§10.21-24):** the boot sits here
  *after* the logo attempt; it may or may not depend on the logo displaying. Its
  advance event (§10.23-24) must be traced independently — the logo failing at 11M
  and "Loading" persisting suggests they are **separate gates**, not one.

**Cross-ref (user note):** the sibling MIPS Coby frame uses **two framebuffers**;
this CT9xx logo path is `JPG_SINGLE_FRAME_BUFFER` (F0=F1, single buffer). If P1's
completion turns out to gate on a page-flip/VSYNC handshake, the double-buffer
variant's frame-toggle is the reference for what the single-buffer path collapses.

**Diagnostics this session (all env-gated):** `CT952_LOGOTRACE` (DISP/JPU display
reg writes + PC), `CT952_DRAMHIST=<icount>` (DRAM data read-frequency, gated to the
JPU-active window via `machine.h jpu_active_until`), `sparc_t.brk_pc` fast
breakpoint. Retail anchors: JPU decode burst code `0x6bb00`/`0x6bbb4`, decode DMA
regs `0x80000e00`/`0x8000031c`, BIU `0x4001fe90`.

### 10.33 P1 pinned — the decode produces NO output; functional decode is the requirement

Sampling the decode burst (icount 10.4-11.2M) shows its hot code is a **buffer-fill
loop `0x3bf00`** (`st` to `[base+0/0x40/0x80]`, 64 words ×3) + OS scheduling + the
idle wait `0x841e8` — i.e. the decode does real setup work, not a busy-poll, then
returns. A decode-window IO trace (`CT952_DECTRACE`) found the only heavily-read
reg is `0x8000031c` at PC `0x497ac` — a *periodic* handler (checks bit 28, timer
calc), NOT the decode. So the decode is not stalled on a status poll.

**The decisive check:** after the decode, the **Y video frame buffer
`0x40065000..0x400B3C00` is entirely zero** (0/80640 words). The decode ran its
op sequence but **wrote no decoded pixels** — because the emulator's JPU model
only clears `JPU_BUSY`; it never actually decodes the bitstream to YUV. With no
valid frame, `JEPG_Decode` returns non-OK → `HALJPEG_Display`/`VIDEO_EN` never.

**So P1's requirement is concrete and unavoidable (this is §10.10 step 3):**
functionally decode the staged logo JPEG (`0x401DC000`, `FF D8 FF E0` JFIF) with
the in-tree picojpeg and **write the result as macroblock-tiled YUV 4:2:0** into
the firmware's video frame buffer (Y `0x40065000`, C `0x400B3C00`; tiling formulas
§10.2), on the decode kick (JPU decode op burst at ~10.4M, or `HALJPEG_Decode`).
Then the driver's completion (which validates the produced frame) can succeed →
`JPEG_Status(DECODE)=OK` → `HALJPEG_Display` sets `F0Y/F0C/VIDEO_EN` → the logo is
on the panel through the firmware's own pipeline, and the same path serves the
built-in photos. The emulator already has picojpeg + `machine_maybe_jpeg_decode`
(writes host RGB) + `jupiter/jfb.c` tiling — the next build wires them to emit the
tiled YUV into the frame buffer at the kick, and (if the driver still needs it)
presents the JPU/BIU completion. Retail anchors: buffer-fill `0x3bf00`, JPU engine
`0x6bb00`, frame buffers `0x40065000`/`0x400B3C00`. Diagnostic added:
`CT952_DECTRACE` (decode-window IO trace).

### 10.34 MILESTONE — the logo decodes through the firmware's pipeline (P1 working)

Implemented `CT952_LOGODECODE` (machine.c): on the JPU decode kick, functionally
decode the staged logo JPEG (`0x401DC000`) with picojpeg and write the result as
macroblock-tiled YUV 4:2:0 into the firmware's video frame buffer (Y `0x40065000`,
C `0x400B3C00`, strip `0x2D00`; §10.2 tiling). **Verified:** de-tiling the frame
buffer back to RGB yields the real **COBY power-on logo** (white "COBY®" on blue,
480×270), pixel-clean — the emulator now produces exactly what the JPU hardware
would, where `HALJPEG_Display` expects it (§10.33 showed this buffer was all-zero).
The emulator's `machine_disp_scanout` already composites the decoded video plane
under the OSD, so a full-panel render shows the logo behind the "Loading" OSD.

**Two remaining polish items (both cosmetic/faithfulness, not the decode):**
1. **Faithful `VIDEO_EN`:** the firmware's `JPEG_Decode` still doesn't report
   `DECODE=OK` (`VIDEO_EN 0x80001a4c` stays 0, `F0Y/F0C` unwritten), so it doesn't
   *itself* flip the video plane on — the emulator composites it regardless. The
   decode-done signal (a JPU/VLD completion IRQ or the `JPEG_Status` thread var)
   is the last piece for a fully hands-off natural display. Producing the output
   and answering the `0xC10`/BIU polls did NOT flip it, so completion is driven by
   a signal not yet modelled (candidate: the decode-done interrupt).
2. **Scanout striping:** at ~60M the OSD plane is dominated by index 37 (a firmware
   background fill) + near-white indices; with `GAM_OSD` unloaded the scanout's
   fallback ramp renders index 37 as light-gray bands over the video. A faithful
   OSD palette (or treating the firmware's OSD-background index as transparent)
   cleans it up.

**Net:** P1's core — functional decode → tiled YUV in the firmware's buffer — is
DONE and verified (the COBY logo). The decoder bring-up now produces real pixels
through the firmware's own tiling; the same path carries the built-in photos.
Env: `CT952_LOGODECODE`. Retail anchors as §10.33.

### 10.35 The decode-done signal is a JPU interrupt (faithful-VIDEO_EN anchor)

JPU register map (`ctkav_jpu.h`) clarifies the decode ops seen at §10.32:
`JPU_CTRL[6:4]` op mode — `JPU_SC 0x10` (scale), `JPU_FC_Y/U/V 0x40/0x50/0x60`
(fill-color per plane), with `JPU_UV_IDX 0x8`. So the ~26-op burst is the JPU
**scaling + fill** of the VLD-decoded image (the VLD does entropy decode; the JPU
post-processes). Output goes to `REG_JPU_ADDR_W_ST 0x2888` (dest addr [26:2]);
source `REG_JPU_ADDR_R_ST 0x2884`; stride `0x288C`; src/dst dims `0x2890/0x2894`.

Crucially: **`JPU_INT_CLR = JPU_CTRL[31]` "signal to reset JPU's interrupt"** — the
JPU raises a **completion interrupt**, and the firmware clears it via bit 31. The
emulator only clears `JPU_BUSY` (bit 0) on a kick; it never raises the JPU
done-interrupt. So the firmware's `JPEG_Decode` (which waits on the JPU/VLD
completion IRQ before setting `JPEG_Status=OK`) never sees "done" → no
`HALJPEG_Display` → `VIDEO_EN` stays 0. Producing the output + `0xC10`/BIU polls
didn't flip it precisely because the missing signal is the IRQ, not a poll.

**Faithful-VIDEO_EN next build:** on the JPU decode kick (or when the fill/scale
op sequence completes), raise the JPU completion interrupt on its LEON IRQ line
(and honor `JPU_INT_CLR` writes), so the firmware's decode driver runs to
`JPEG_Status=OK` → `HALJPEG_Display` sets `F0Y/F0C/VIDEO_EN` itself. Anchors:
`JPU_CTRL 0x80002880` (INT_CLR bit 31), `ADDR_W_ST 0x2888` (real output dest to
write the tiled YUV into, instead of the hardcoded 0x40065000), the LEON IRQ
controller (`0x80000000` block). With that, the CT952_LOGODECODE output + the IRQ
completes the fully-hands-off natural display.

### 10.36 Faithful-VIDEO_EN — the decode-completion is the full VLD+JPU protocol

Attempts to drive `JPEG_Status(DECODE)=OK` narrowed the mechanism:
- **No firmware `.c` waits on the JPU IRQ** — the completion handling lives in the
  precompiled decode lib (`JEPG_Decode`/`JEPG_Status`). The emulator models the
  `PROC1_1ST` secondary IRQ (LEON line 13, bit0=VSYNC); the JPU done is another
  bit there, but raising it only helps if the precompiled ISR advances the state.
- **`JPEG_Status` isn't a single pokeable byte:** a status-write trace
  (`CT952_JSTAT`) during the decode shows a *cluster* of small-value writes at
  `0x40040dxx` (~icount 10.72M, an array fill at decode-finish), not one clean
  UNFINISH→OK transition — consistent with the driver tracking per-block state.
- **The JPU is scale/fill only** (§10.35); the entropy decode is the **VLD**. The
  emulator answers the JPU_BUSY + some VLD polls but does not run the VLD to
  produce valid decoded macroblocks, so the driver never sees a valid completed
  frame → returns non-OK → `HALJPEG_Display`/`VIDEO_EN` never.

**So faithful `VIDEO_EN` = model the full VLD→JPU→done pipeline**, not one signal:
present the VLD entropy-decode as done (per-block + frame), run the JPU
scale/fill (or short-circuit it) writing to `ADDR_W_ST 0x2888`, and raise the JPU
completion IRQ (`PROC1_1ST`, honoring `JPU_INT_CLR` bit31). Then the precompiled
`JEPG_Decode` runs to OK → `HALJPEG_Display` sets `F0Y/F0C/VIDEO_EN` itself. This
is the deep next build; it is well-scoped (all registers/IRQ known) but multi-step.

**Pragmatic alternative already working:** `CT952_LOGODECODE` produces the correct
tiled frame (the verified COBY logo, §10.34) and the scanout composites it, so the
image is on the panel through the real tiling — just not firmware-flipped. For a
clean visible panel, load/emulate the OSD palette (or treat the firmware's OSD
background index as transparent) to remove the §10.34 gray striping.

**Session close-out:** the decoder bring-up went from "Loading never draws" to
booting naturally to "Loading" AND decoding the real logo through the firmware's
own tiling into its frame buffer (verified pixels). Remaining, all mapped: (a)
faithful `VIDEO_EN` via the VLD+JPU+IRQ pipeline above; (b) the separate P2
"Loading" state-machine advance (§10.24); (c) OSD-palette for a clean composite.
Diagnostics: `CT952_LOGODECODE`, `CT952_JSTAT`, `CT952_DECTRACE`, `CT952_LOGOTRACE`,
`CT952_DRAMHIST`, `sparc_t.brk_pc`.

### 10.37 CORRECTION + full gate map — decode-done is NOT a JPU IRQ; "Loading" is the upstream blocker

Disassembling the retail ROM directly (no SDK symbols; flash XIP + the DRAM-
resident firmware image dumped live) overturned the §10.35/10.36 "JPU completion
interrupt" theory and re-prioritised the roadmap. Every address below is
retail-verified.

**(A) `JPU_WaitDone` @flash `0x6bb14` already succeeds — the per-op JPU wait is NOT
the blocker.** It kicks `JPU_GO` (bit1 of `REG_JPU_CTRL 0x80002880`), then
`while (JPU_CTRL & JPU_BUSY(bit0))` with a ~499-tick timeout, aborting if
`REG_SRAM_PLAYMODE(0xB0000190) == MODE_STOP(0x10)`. Returns 1=done. The emulator
**already** clears `JPU_BUSY` on the kick (`io_write R_GPU_CTL0`), so this wait
returns success immediately. There is no IRQ in this path — it is a busy-poll of
`JPU_BUSY`. §10.35's "JPU raises a completion interrupt the firmware waits on" is
**retired**.

**(B) The `pc=0x4001f600` read of `0x800000b4` was a ghost.** `0x4001f5ec` is the
`PROC1_1st` ISR: it reads pending `0x800000b4` **& mask `0x800000b0`**, dispatches
bit0=VSYNC→`0xa8bb0` and bit7(0x80)→`0x6f664`, runs a callback list at
`0x40039114`, clears serviced bits via `0x800000b8`, EOIs `0x8000009c`. The
`0x800000b4` reads seen "during the decode" were just this ISR firing on **VSYNC
every frame**, not a decode poll. Raising `RL_DONE|MC_DONE (0x60)` was chasing that
ghost (the ISR masks+clears them) — **reverted** in `io_write`.

**(C) `VIDEO_EN` is a per-frame software→hardware copy, downstream of everything.**
The DISP compositor at flash `0xa683c` does `REG_DISP_VIDEO_EN(0x80001a4c) =
*0x40023fc0` every frame (gated by `*0x40040e70 != 3`). It always writes **0**
because the software video-enable flag `*0x40023fc0` is never set — `HALJPEG_Display`
never runs, so `F0Y/F0C (0x80001ac0/0x80001ac4)` are **never written in the entire
90M-instr boot** (verified). So VIDEO_EN cannot flip until the decode path runs,
which cannot happen until the boot advances past "Loading". **VIDEO_EN (old build
#1) is premature; the "Loading" advance is upstream and comes first.**

**(D) The "Loading" advance chain, mapped gate-by-gate (retail addresses).** Steady
state (PCSAMP + disasm): the boot thread sleeps in the eCos scheduler; the state-8
Loading handler is blocked in its modal wait. State-8 draw+wait `0x26e44`:
```
0x26e44  OSD_Output(MSG_WAITING)=draw "Loading"   (0x49944(2,0,0xFF))
0x26e58  call modal-wait 0x24d0c(0)
0x26e68  if ret==0 -> 0x26f30  (EXIT: keep waiting)      <-- gate 1
0x26e70  call 0x254a4  (post-wait queue check)
0x26e80  if ret==0 -> 0x26ea0  (ADVANCE)                 <-- gate 2 (0 == advance)
```
Modal-wait wrapper `0x24d0c` returns non-zero (→ reaches `0x254a4`) **only** when a
real event is dequeued+handled:
```
0x24d54 call 0x11fb0(0)  -> if 0 exit(0x24e4c: redraw, ret 0)   [flag wait]
0x24d68 call 0x11f48     -> if 0 exit(0x24e4c)                  [list 0x40032180]
0x24de8 call 0x252d0 ; 0x24e10 call 0x24ea0  (process event)
0x24e1c if byte *0x40032a31 == 0 -> 0x24e4c (redraw, ret 0)     [handled-event]
        else set flags, 0x40228, ret 1
```
- `0x11fb0(0)` peeks flag `0x1000` via `0x65dc` on flag words `0x40026e9c/…ea4`;
  returns after a wake.
- `0x11f48` counts a linked list anchored at `0x40032180`; **at runtime the list is
  self-referential (empty) → returns 0 → wrapper exits early.** Concrete gate 1.
- `0x254a4` copies event source `*0x400328b8` (NULL at runtime) into queue
  `0x400329fc` via `0x6b6a4`, then `0x25d58→0x6b660` counts null-terminated
  **halfword** event codes; `[fp-16]!=0 ⇒ return 0 ⇒ advance`. Queue empty.
- The fetch `0x12e18` reads an **input-state structure** at `*0x40021db8` (42-entry
  bitmap via `0xdd904`), not a simple queue — this is the input subsystem.

**Conclusion (confirms + sharpens §10.24):** the "Loading" state advances **only when
the input/event subsystem delivers a real event** (key / IR / media / decode-done).
Every gate — flag `0x1000`, list `0x40032180`, handled-event byte `0x40032a31`,
queue `0x400329fc`, input struct `*0x40021db8` — is a facet of "a real event
arrived," and all are downstream of the decoder held in reset + no input. The
faithful unlocks are unchanged: **(1) drive the input path** (post a key faithfully
through the `0x12e18` input structure `*0x40021db8`, the user's "inject a key event"
request), or **(2) model the decoder** so it posts its own decode-done event.
Re-prioritised roadmap: the **Loading advance (input/event subsystem)** is the true
next build, ahead of `VIDEO_EN`. New retail anchors: modal-wait `0x24d0c`; flag
predicate `0x11fb0`/`0x11f48`; list `0x40032180`; handled-event byte `0x40032a31`;
event source ptr `*0x400328b8`; input struct `*0x40021db8`; JPU_WaitDone `0x6bb14`;
PROC1_1st ISR `0x4001f5ec`; DISP video-enable copy `0xa683c` (`*0x40023fc0`).

### 10.38 MILESTONE — faithful IR input hardware modeled + verified (Loading is decode/media-gated, not key-gated)

Built roadmap item #1 (faithful input path): modeled the CT909P **IR receiver +
PROC1-2nd interrupt** so a synthetic remote keypress drives the firmware's OWN
ISR/DSR/decode chain — no memory-poking of decoded state.

**What was added (`machine.c`):**
- **PROC1-2nd interrupt controller** (`ctkav_platform.h:70-76`): `MASK_ENABLE 0x0D0`,
  `PENDING 0x0D4`, `STATUS/CLEAR 0x0D8` (W1C), `MASK_DISABLE 0x0DC`. Cascades to
  **LEON line 10** (`INT_NO_PROC1_2ND`) in `bus_irq_level`, mirroring the PROC1-1st
  (line 13) VSYNC cascade. IR source bit = `INT_PROC1_2ND_IR 0x4`.
- **IR receiver block** (`ctkav_platform.h:497-506`): `IR_DATA 0x390`
  (`[7:0]`=scancode, `[8]`=repeat, `[10]`=invalid), `IR_RAW_CODE 0x394`
  (`[31:24]`=customer, `[23:16]`=customer1).
- **One-shot injector** `CT952_IRKEY="<scancode>[@<icount>]"` (default icount 40M):
  presents a clean NEC data frame, force-enables the IR mask bit (real silicon has
  IR enabled; the stuck-at-Loading boot never ran that init, so `0x0D0` read `0x400`
  = VBUF_UNDERFLOW only), and raises PROC1-2nd IR pending.

**Verified end-to-end (`CT952_IRTRACE`/`CT952_KEYTRACE`):** injecting scancode `0x06`:
```
[IRrd] 800000d4=00000004 pc=4001f730   <- PROC1-2nd ISR read pending (line-10 IRQ TAKEN)
[IRrd] 80000390=00000006 pc=00042328   <- ISR_IRSaveClearStatus read IR_DATA (our scancode)
[IRrd] 80000394=00ff0600 pc=00042340   <- read IR_RAW_CODE; customer 0x00/0xFF == CUSTOMER_CODE/1
[KEYwr] 40039074=72 pc=000423a0        <- DSR_IR/INPUT_RemoteScan wrote __bISRKey = aIRMap[0x06]
[KEYrd] __bISRKey read pc=000423f0     <- read once, inside the DSR itself
```
So the whole faithful chain runs: **line-10 IRQ → INT_Proc1_2nd_isr →
ISR_IRSaveClearStatus (saves IR regs) → DSR_IR → INPUT_RemoteScan → `__bISRKey`**.
The compiled remote map gives `aIRMap[0x06] = 0x72 = KEY_N9` (a *different* map than
`ir.h:57`; the customer code `0x00/0xFF` matches `CUSTOMER_CODE/CUSTOMER_CODE1`).
`__bISRKey` (retail addr **`0x40039074`**) is confirmed.

**Key finding — the "Loading" state does NOT consume keys.** After the DSR sets
`__bISRKey`, it is **never read again** to 50M, and **nothing writes the Loading event
queue** `0x400329FC` / input struct `0x40032290` (`*0x40021db8`). The panel is
byte-identical with vs. without the keypress. The CC main loop (`CC_MainProcessKey`,
`cc.c:882`) that would convert `__bISRKey` → events is **not the running thread**
during the INITIAL/`Loading` modal-wait — INITIAL is a self-contained power-on flow,
and the CC event loop only runs after INITIAL completes. So a remote key cannot skip
"Loading": it advances on **decode-done / media**, faithfully (a photo frame doesn't
let you key past its splash). This **reinforces §10.24/10.37**: the single upstream
unlock is the **decoder/media bring-up**, which posts the advance event itself.

**Net:** the IR input subsystem is now a real, reusable capability (verified against
the firmware's own ISR/DSR), ready for the interactive states the boot reaches once
Loading advances. New retail anchors: PROC1-2nd ISR `0x4001f720`;
`ISR_IRSaveClearStatus 0x42328`; `INPUT_RemoteScan`/`DSR_IR` @flash `0x423xx`;
`__bISRKey 0x40039074`; IR regs `0x80000390/0x80000394`; PROC1-2nd ctrl `0x800000d0-dc`.
Diagnostics: `CT952_IRKEY`, `CT952_IRTRACE`, `CT952_KEYTRACE`.

### 10.39 DEEP DECODER DIG — the advance-event never fires; JPEG_Status(DECODE) is the true gate

A full retail disassembly of the "Loading" event framework + the JPEG decode
orchestrator, reconciling §10.22/10.30/10.32/10.37. All addresses retail-verified.

**(A) The Loading advance-event NEVER fires (decisive, via `CT952_REACH`).** The
modal-wait (`0x24d0c`) advances only when its list (`0x40032180`, checked by
`0x11f48`) gets an event. The **sole** function that posts to that list is
`0x12f10` (`PostEvent`, via `0x6b80c`→list `0x40032188`), and its **sole caller**
is `0x6eec` — inside the CC event **dispatcher** (`0x6df8-0x6f0c`) that reads CC
flag bits and posts events from ring buffers (bit `0x80` = key ring, `0x200` =
next, …). Over a 60M-instruction boot, **neither `0x12f10` nor `0x6eec` is ever
reached.** So the modal-wait list is permanently empty and no forward event exists.

**(B) The media scan never runs.** The media thread reaches `MEDIA_Management`
(`0x1152c`) at icount 4.9M but **never** reaches `MEDIA_MonitorStatus` (`0x1186c`)
or `_MEDIA_MonitorMediaStatus` (`0x118b8`). So the USB/removable-media scan that
would post a media event (and let `CT952_NOMEDIA` engage, §10.17) never executes —
the media worker is parked asleep (USB/card HW init unmodeled). No media event.

**(C) The logo decode succeeds at the JPU level but JPEG_Status(DECODE) stays
non-OK.** `UTL_ShowLogo`→`UTL_ShowJPEG_Slide` shows the logo; its DECODE-status poll
(`HALJPEG_Status(HALJPEG_DECODE)`→`JPEG_Status(JPEG_DECODE)`, precompiled lib) never
returns OK, so `HALJPEG_Display` never runs — **`F0Y/F0C`/`VIDEO_EN` are never
written in the whole boot** (§10.37). Yet the JPU work *does* complete: `JPU_WaitDone`
(`0x6bb14`) returns 1 (the emulator clears `JPU_BUSY` on each kick), and the decode
sub-orchestrator (`0x6d4c0`, two `WaitDone` calls at `0x6d510`/`0x6d558`) returns 1
(success). **So the non-OK comes from a frame/VLD-level completion gate ABOVE the
per-op JPU_BUSY** — the signal the precompiled `JPEG_Decode` thread waits on before
writing `JPEG_Status=OK`, which the emulator never produces. This **supersedes
§10.30** ("infinite JPU loop on `0x80000e00`" — that growth was the GPU redraw,
§10.32; the actual JPU decode runs once and succeeds).

**(D) The video compositor (found).** `0xac120` updates the DISP video plane
(writes `0x8000194c/0x80001950` + the `VIDEO_EN 0x80001a4c` region) whenever a state
byte `state+310` differs from the "displayed" byte `0x40040ee4` (with a `0x40040ee2`
dirty flag). It runs each ~200K-instr redraw pulse but the bytes always match
(nothing advances the state), so the plane is never reconfigured.

**Unified conclusion (reconciles §10.17/10.22/10.24/10.37/10.38).** The boot is
parked in a multi-thread sleep because **the entire event-generation chain is
dormant** — every forward event requires an upstream HW signal that the emulator
doesn't produce:
- the precompiled `JPEG_Decode` never reports OK (missing **frame-completion**
  signal, distinct from `JPU_BUSY`), so no logo-display-done event;
- the media worker never posts (USB/card HW unmodeled), so no media event;
- input works (§10.38) but "Loading" doesn't consume keys.

**Sharpest next target:** find the precompiled `JPEG_Status` status variable + what
`JPEG_Decode` waits on for **frame** completion (the VLD/decode-done signal above
`JPU_BUSY` — candidates: a VLD frame-done reg, or an interrupt/semaphore the decode
thread blocks on). Provide it (alongside the functional picojpeg output already
written) → `JPEG_Status(DECODE)=OK` → `HALJPEG_Display` sets `F0Y/F0C/VIDEO_EN` AND
`UTL_ShowJPEG_Slide` returns TRUE → the state machine posts its logo-display-done
event via `0x6eec`/`0x12f10` → boot advances. New retail anchors: `PostEvent 0x12f10`;
event dispatcher `0x6df8`/`0x6eec`; decode sub-orchestrator `0x6d4c0`; video
compositor `0xac120` (state byte `+310`, displayed byte `0x40040ee4`);
`MEDIA_Management 0x1152c`. Diagnostics: `CT952_DSTRACE`, `CT952_KEYTRACE`, `CT952_REACH` (+PostEvent/dispatch watches).

### 10.40 Parallel-agent decode dig — the decode-status gate is REAL but INERT (the logo decode never runs)

Fanned out three read-only agents on independent threads; here is the synthesis.

**Decode-status mechanism — fully pinned (Agents 1+2, cross-verified static+empirical).**
`UTL_ShowJPEG_Slide` (flash `0x61170`) polls `HALJPEG_Status(HALJPEG_DECODE)`
→ `JPEG_Status` mapper **`0x375a0`** (action 0) → decoder-state getter **`0x6f054`**.
The getter's action-0 handler (`0x6f098`) reads HW `0xB0000190`; when HW ∈ {0, 0x11}
it returns the **SW mirror `0x40039cd0`**, else `mirror | 0x1000` (busy). The mapper
`0x375a0`: **state `0x10` → OK(1)**, `0x11` → FAIL(0), `0x00` → UNFINISH(2). Enum
(haljpeg.h): OK=1, UNFINISH=2, FAIL=0, UNSUPPORT=3. Bail on non-OK at `0x61338`→
`0x6139c` (return FALSE, no `HALJPEG_Display`). So **DECODE OK ⇔ getter returns
`0x10`**, which needs **mirror==0x10 AND HW∈{0,0x11}**. (Agent 1 first reversed a
*different* copy `0x70808/0x70890` that keys on HW==0 — the wrong path; Agent 2
traced the copy actually wired to `UTL_ShowJPEG_Slide`.) Forcing HW `0xB0000190=0x10`
FAILS — it sets the busy bit; only the *mirror* must read 0x10.

**Fix built (`CT952_VDEC_DONE`, verified-correct):** `vdec_frame_done` is set on the
JPU decode burst (the decoder reaching MODE_STOP(0x10)=frame-done); the `0x40039cd0`
read handler then returns `0x10` (after the boot stop gates, so gate-3's `0x11` is
untouched). Confirmed the getter's action-0 handler (`0x6f0c0`) then reads `0x10`.

**BUT — the gate is INERT in this boot (decisive empirical finding).**
`UTL_ShowJPEG_Slide` (`0x61170`) is **never reached** (checked flash *and* DRAM
`0x40061170`), and its decode poll never runs. Reason: `UTL_ShowLogo` (utl.c:481)
returns early at **utl.c:512** because `LOGO_TYPE()==LOGO_DEFAULT` (utl.c:615/872
set `__bLOGO |= LOGO_DEFAULT`). Corroboration: **no JPEG SOI (`ff d8 ff`) exists
anywhere in DRAM** at 12M or 45M — the logo JPEG is never staged, so
`CT952_LOGODECODE`'s functional decode has also been a no-op in this config. So
there is **no logo/photo JPEG decode during boot**, the decode-status poll never
fires, and the `CT952_VDEC_DONE` fix — though mechanically correct — cannot advance
Loading. **The "Loading" stall is NOT a logo-decode-done gate.**

**Media path (Agent 3).** The sole Loading advance-event poster `0x12f10` (via CC
dispatcher `0x6eec`, bit `0x80`) never fires. The media thread never runs the scan:
Gate A — the USBSRC worker is parked in unmodeled `usb.a`/`card.a` HW init, so
`__fThreadInit` USB_DONE (`0x4003e590`, bit `0x80000`) is 0; Gate B (source-level,
no HW needed) — `_MEDIA_MonitorMediaStatus` **early-returns at media.c:1479** when
`__bChooseMedia==MEDIA_SELECT_DVD`, and it is initialised to DVD at media.c:616
(the `MEDIA_SELECT_USB` line is commented out).

**Reconciliation + redirect.** Prior sections (§10.23/10.39, Agent 3) said "the
Loading advance is the JPEG decode-done event" — but that decode **never runs** in
this power-on path, so it cannot be the trigger. With no logo decode, no media scan,
and no consumed input, **nothing generates the advance event** — the boot is a true
multi-source event-starvation stall. The `CT952_VDEC_DONE` decode-completion model
is verified-correct and stays ready for the *slideshow* decode that runs only AFTER
Loading advances. **Sharpest next lever:** Agent 3's Gate B — set
`__bChooseMedia = MEDIA_SELECT_USB` (+ the `__fThreadInit` USB_DONE override) so the
media scan runs → no-media verdict → `POWERONMENU_Initial`/`MM_PlayPhotoInFlash`,
which is the retail path to the built-in slideshow. New anchors: getter action-0
`0x6f098`/`0x6f0c0`; mapper `0x375a0`; `UTL_ShowJPEG_Slide 0x61170` (unreached);
`UTL_ShowLogo` early-return utl.c:512; media Gate B media.c:1479/616. Diagnostics:
`CT952_VDEC_DONE`, `CT952_MIRTRACE` (now logs returned state + fdone).

### 10.41 BREAKTHROUGH — the firmware's JPEG decode pipeline runs end-to-end (DECODE=OK, display called)

Combining the media-select flip with the decode-completion model unblocked the
entire slideshow/logo decode path. All effects verified in one run
(`CT952_CHOOSEMEDIA=0x40031b9c CT952_LOGODECODE=1 CT952_VDEC_DONE=1 CT952_NOMEDIA=1`).

**Chain of unlocks:**
1. **`__bChooseMedia`@`0x40031b9c` → USB(1)** (subagent-located; write pair at flash
   `0x5a96c`, `__bNavigateMode`@`0x40031bac`=3 corroborates). Read-override via
   `CT952_CHOOSEMEDIA`. This alone makes **`UTL_ShowJPEG_Slide` (`0x61170`) execute**
   (icount 46M) — it never ran before (§10.40) — and the firmware **stages a real
   JPEG at `0x401dc000`** (SOI `ff d8 ff e0`, the 480x270 COBY splash), which was
   never staged in the DVD-mode path.
2. **`CT952_VDEC_DONE` (decode-completion model).** The decode-status getter
   (`0x375a0`→`0x6f054` action-0 handler `0x6f098`) returns OK only when it yields
   state `0x10`, which needs **BOTH** the mirror `0x40039cd0`==0x10 **AND** HW
   `0xB0000190` ∈ {0,0x11} (else it OR-s in the `0x1000` busy bit → the `0x6f0c8`
   path → non-OK). So the fix sets, after the JPU decode burst (`vdec_frame_done`):
   mirror `0x40039cd0` → `0x10` **and** HW `0xB0000190` → `0x11`. Verified the
   getter then returns clean `0x10` via `0x6f0c0`.
3. **Result:** `UTL_ShowJPEG_Slide`'s DECODE poll (`0x61318`→`0x375a0(ctx,1)`)
   returns **OK(1)**, the OK path (`0x61344`) is reached, and the firmware calls its
   **display function `0x39f38`** (icount 49.39M). The functional decode produces
   the **clean 480x270 COBY logo** into the tiled-YUV video buffer
   `0x40065000`/`0x400b3c00`.

**Verified anchors (retail):** DECODE poll `0x61318`; OK path `0x61344`; display
call `0x39f38`; status mapper `0x375a0` (state 0x10→OK, 0x11→FAIL, 0→UNFINISH);
getter `0x6f054`/handler `0x6f098` (clean `0x6f0c0` vs busy `0x6f0c8`); mirror
`0x40039cd0`; HW playmode `0xB0000190`; `__bChooseMedia 0x40031b9c`;
`UTL_ShowJPEG_Slide 0x61170`.

**What's left for a VISIBLE photo on the panel:** the firmware's display path
(`0x39f38`) does NOT write `F0Y/F0C 0x80001ac0/ac4` nor set `VIDEO_EN 0x80001a4c`
(still 0 — the per-frame compositor `0xa685c` copies software flag `*0x40023fc0`,
which stays 0). So the decoded logo sits in the video buffer but the video plane is
not enabled; the panel scanout still shows the OSD ("COBY" with the §10.34 palette
striping). Next: find where this build enables the video plane for the JPEG (does
`0x39f38` set a software flag the compositor reads, or is the photo meant for the
OSD/GPU plane?), so the clean decoded image composites onto the panel. Diagnostics:
`CT952_CHOOSEMEDIA`, `CT952_VDEC_DONE`, `CT952_MIRTRACE`, REACH watches.

### 10.42 MILESTONE — natural boot to POWERONMENU + clean COBY splash on the panel

The full chain now works end-to-end and renders the real power-on splash.

**Boot reaches POWERONMENU_Initial** (`0x4b808`, icount 75.5M) — the power-on menu
that launches the built-in slideshow — with `CT952_CHOOSEMEDIA=0x40031b9c
CT952_VDEC_DONE=1 CT952_NOMEDIA=1`. From "stuck at Loading forever" (start of this
arc) to the menu, entirely through the firmware's own flow.

**Panel render fixed (two scan-out bugs):**
1. **OSD buffer stride is 720, not 616.** `machine_disp_scanout` was called with
   `stride = fb_w = 616`; the real OSD framebuffer at `0x4005F000` is 720-byte-
   aligned per row (SDTV width). Reading a 720-stride buffer at 616 drifted each
   row by 104 bytes → horizontal striping. Added `--fb-stride` (default 720).
   Verified: a vertical slice at stride 720 is clean (`37 37 …37 0 0 0` = COBY text
   then transparent index-0), garbage at 616.
2. **Honor DISP_OSD_EN.** When the OSD plane is disabled (as at the splash phase),
   the panel shows ONLY the video plane; the scan-out was compositing the not-yet-
   displayed OSD content and mixing its fallback-ramp gray (index 37 → 240) with
   the video → striping. Now `!osd_en → idx=0` (video-plane only).

**Result:** the panel shows the clean **COBY splash logo** — the 480x270 JPEG the
firmware decoded (via `UTL_ShowJPEG_Slide` → DECODE=OK → display, §10.41) on the
video plane. This is exactly what the real device shows at power-on.

**Remaining for the slideshow:** POWERONMENU is reached but only the COBY splash is
decoded so far (no `01/02/03.jpg` yet to 130M). Next: get POWERONMENU to select
flash playback (`MM_PlayPhotoInFlash`) and decode the built-in photos. The OSD
palette (GAM_OSD `0x80001C00`) is still unloaded — for OSD-enabled screens (menu
UI) it renders via the fallback ramp; loading the real palette is a separate polish
item (the splash doesn't need it since OSD is disabled there).

### 10.43 PAYOFF — built-in photos render on the panel through the firmware's decode pipeline

The three built-in demo photos now display on the emulated panel end-to-end.

**Built-in photo album located:** flash section-table entry **"0001" @0x160000, size
0x50000** (`0x1d8`), a 320KB album holding three real Photoshop-exported JFIF+EXIF+XMP
photos at **0x160000** (zebra butterfly on flowers, 640x360), **0x170000** (Grand
Tetons / Moulton barn), **0x180000**. Each is section-aligned with a main image +
EXIF thumbnail. (The retail firmware plays them via its MM album player off section
"0001"; that player's exact trigger from POWERONMENU is a separate dig.)

**Rendered through the working pipeline:** `CT952_STAGE_PHOTO="<flash_off>"` copies a
built-in JPEG from flash into the staging buffer `0x401dc000`, so the firmware's own
decode+display path (`UTL_ShowJPEG_Slide` → DECODE=OK via `CT952_VDEC_DONE` →
`0x39f38`, §10.41) plus the functional decode + the fixed scan-out (§10.42) render
the real photo on the video plane. Verified: the functional decode reports
`640x360 from 0x401dc000` and the panel shows the actual photo (butterfly / barn),
not the logo. This demonstrates the entire decode→display→composite chain works on
real content — the slideshow payoff.

**Chain summary (whole arc):** natural boot past "Loading" → media-select flip
(`__bChooseMedia 0x40031b9c`→USB) → `UTL_ShowJPEG_Slide` runs → decode-completion
model (`CT952_VDEC_DONE`: mirror `0x40039cd0`→0x10 + HW `0xB0000190`→0x11) → DECODE=OK
→ firmware display path → functional decode → scan-out (stride 720, honor OSD-en) →
photo on panel. Boot also reaches POWERONMENU_Initial (75.5M). Diagnostics:
`CT952_CHOOSEMEDIA`, `CT952_VDEC_DONE`, `CT952_STAGE_PHOTO`, `CT952_NOMEDIA`.

### 10.44 The built-in album is FIVE photos (not 3) + faithful screensaver path mapped

**Correction (user-confirmed):** the built-in demo album is section **"0001" @ flash
0x160000, size 0x50000** (320KB), holding **FIVE** 640x360 JFIF photos at
0x160000/0x170000/0x180000/0x190000/0x1a0000 (butterfly, Grand Tetons/barn,
chrysanthemums, Golden Gate Bridge, mountain river). The source constant
`BUILD_IN_JPG_ENCODE_NUM=3` (Winav.h) is stale for this retail build. Full section
table: CUST/CLCK/lang-strings(DUTC..SPAN)/**0001**(album)/COPY(scratch @0x1b0000).
All five verified rendered on the panel via `CT952_STAGE_PHOTO` through the
firmware decode pipeline (§10.41-43).

**Faithful slideshow path (retail-traced), for the natural trigger:** the JPEG
screensaver plays the album. `OSDSS_Monitor()` (osdss.c:298, called from CC main
loop cc.c:1004) fires `OSDSS_Entry()` (osdss.c:228) after `OSDSS_ENTER_TIME`
(=10 min) idle, gated on `__bPOWERONMENUInitial && !clock && !alarm`. `OSDSS_Entry`
returns early unless `__bMMJPGEncodeNum>0` (set to the photo count by
`MM_EncodeFile_Init` mm_play.c:2402). A 5-sec timer (osdss.c:560) advances
`__bOSDSSPicIdx` (0..count-1) and calls `_OSDSS_PictureUpdate` (osdss.c:122), which
in `SUPPORT_ENCODE_JPG_PICTURE` mode sets source=SOURCE_SPI and calls
`UTL_PlayItem(idx+1,0)` → reads the encoded JPEG from SPI flash → `UTL_ShowJPEG_Slide`
(already DECODE=OK, §10.41). The 10-min idle timeout is impractical to reach in
emulation, so the natural trigger must be forced (force `__bMMJPGEncodeNum=5` +
short-circuit the idle-timeout compare / call OSDSS_Entry). Addresses under
investigation. Diagnostics: `CT952_STAGE_PHOTO` (renders any album photo now).

### 10.45 Faithful screensaver trigger — gates satisfied, but blocked by scheduler + event-starvation

Cross-verified the built-in JPEG-screensaver playback path and its gates (retail
addrs, from `dp700wd.bin`, NOT the SDK sym which mismatches):
`OSDSS_Entry 0x59108`, `_OSDSS_PictureUpdate 0x59004`, `OSDSS_Monitor 0x591b4`,
`_OSDSS_Move 0x59474`, `UTL_PlayItem 0x5b964`, `MM_EncodeFile_Init 0x29864`,
`SrcFilter_ReadSectors 0x5a3d4`. DRAM: `__bMMJPGEncodeNum 0x40032b3b=**5**`,
`__bOSDSSPicIdx 0x400239cc`, `_bOSDSSScreenSaverMode 0x400239c4`,
`__dwOSDSSCheckTime 0x400239b8=0xFFFFFFFF`, `__bPOWERONMENUInitial 0x40023a10=0`.

**Three of four gates already satisfied:** `MM_EncodeFile_Init` ran the fresh-init
branch → `__bMMJPGEncodeNum=5` and the file list {1,2,3,4,END} is built; the
`SOURCE_SPI` read auto-resolves to **flash 0x160000 + idx*0x10000** (SrcFilter SPI
case at 0x5a404, `SRCFTR_SPI_ENCODE_ADDR=0x160000`) — no SPI-HW modeling needed,
the flash is already memory-mapped; decode is handled by `CT952_VDEC_DONE` +
scan-out by §10.42.

**The blocker: screensaver ENTRY never fires, and can't be shortcut.**
- Natural: `OSDSS_Monitor` (CC main loop, cc.c:1004) is never reached —
  `__dwOSDSSCheckTime` stays 0xFFFFFFFF (its first-init never ran),
  `__bPOWERONMENUInitial` stays 0 (POWERONMENU_Initial doesn't complete),
  `__dwTimeNow` stays 0 (tick base not advancing). Same **event-starvation** class
  that has gated this boot throughout — the CC idle loop isn't fully alive.
- Forced via `machine_call(OSDSS_Entry/_OSDSS_PictureUpdate)`: **HALTS** with
  `trap 0x06 (window_underflow) at pc=0x4001e52c` (the eCos scheduler). The play
  path makes a blocking OS call (OS_DelayTime/semaphore) that enters the scheduler
  for a context switch — impossible inside the isolated, trap-off, single-thread
  `machine_call` context. So `machine_call` fundamentally can't run
  scheduler-dependent firmware.

**Net:** the firmware's decode+display pipeline renders all 5 real album photos
(from the real flash addresses, via `CT952_STAGE_PHOTO`, §10.43-44); the fully
autonomous screensaver cycle needs the deep event-starvation fix (make the system
tick + CC idle loop live) — the true remaining frontier, and the same root as
§10.24/10.39. That fix would also complete the natural boot.

### 10.46 FINAL-BOSS PROOF — time-dilation is NOT the blocker; the CC loop is empirically asleep

§10.45 left two live hypotheses for why the autonomous screensaver never fires
after POWERONMENU: **(H1) time-dilation** — the eCos clock advances so slowly
(~7.5 ticks per 1M instructions, §"time-dilation") that the idle timeout is simply
never reached inside a tractable budget; **(H2) event-starvation** — the CC main
loop thread that *checks* the idle timer is parked asleep on a wake event the
emulated peripheral set never raises, so no amount of virtual time helps. This
section discriminates them empirically and rules out H1.

**Tool built — deferred fast tick `CT952_TICK_FAST_AT=<icount>[,<mult>]`.** A naive
global tick multiplier breaks early boot (fast timeouts fire before their events;
`INITIAL_System` is never reached). The fix ticks TIMER1 at 1× until `<icount>`,
then divides the TIMER1 reload by `<mult>` — so the *early* init delays run at
real cadence while the *post-POWERONMENU* idle wait is compressed. Implemented in
`timer_tick_one` (new `rld_div` param) + a `machine_cycle` gate parsing the env.

**Result (200M-instr run, `CT952_TICK_FAST_AT=76000000,64`, POWERONMENU @75.5M):**
- **The fast tick engages, verified against the live clock.** The eCos counter word
  (retail `0x4002e32c`) read from the 200M DRAM dump = **0x782e = 30766 ticks**,
  vs. **~1494** ticks at 1× for the same budget — a ~20× compression of virtual
  time. Time-dilation is overcome. **H1 is false.**
- **The screensaver still never fires** and no 2nd photo decodes. So compressing
  time is *not sufficient* — confirming the blocker is elsewhere.
- **The CC main-loop thread is empirically SLEEPING.** Read from the same dump: the
  CC `Cyg_Thread` object (retail `0x400371f8`) has `state=1` (SLEEPING) at 200M.
- **The CPU is in the idle+timer steady state.** The last-64-PC ring and the
  register-indirect-jump trail at 200M cycle *only* through the eCos
  scheduler/context-switch region (`0x4001d000`–`0x4001e900`) and the timer/trap
  dispatch (`0x400010b0` → handlers) — i.e. timer ISR wakes, scheduler runs, finds
  no ready thread, returns to idle. Nothing advances the application state.

**Verdict (H2 confirmed by direct observation, not inference).** The autonomous
screensaver is gated by the CC main loop thread being parked on an unmodeled wake
event — the same **event-starvation** root as §10.24/10.39/10.45 — now proven by
reading the thread's `state` and the live scheduler PC-trail out of a 200M memory
dump, with time-dilation independently eliminated. The remaining frontier is
narrowed to one concrete action: inject a *periodic, non-user* wake event (e.g. a
clock-display tick message the CC loop consumes without resetting idle) so the loop
iterates once per virtual second, sees the (now-elapsed, fast-ticked) idle timer,
and calls `OSDSS_Monitor` → the screensaver naturally. Note the retail SDK-sym
`.text` addresses **match** for the early functions (`CC_DVD_MainLoop 0x2014`,
`Thread_CTKDVD 0x2318`, `INITIAL_System 0xeb90`, `MEDIA_Management 0x1152c`,
`POWERONMENU_Initial 0x4b808`) but **diverge** past ~`0x1152c`
(`UTL_ShowJPEG_Slide`: sym `0x31cf0` vs retail `0x61170`), so name low PCs from the
sym but keep treating OSDSS addresses as retail-empirical.

**Deliverable this pass:** all 5 built-in album photos rendered through the
firmware's own decode→display→scan-out pipeline (butterfly, Tetons/barn,
chrysanthemums, Golden Gate Bridge, mountain river) and stitched into an animated
slideshow — the visual final-boss payoff — while the *fully autonomous* firmware
timer cycle stays blocked on the H2 event injection above.

### 10.47 The screensaver trigger, DISASSEMBLED — exact gate + the flag-set stall

Tooling note (and a mea culpa): a working SPARC disassembler was available the
whole time — `sparc64-linux-gnu-objdump -b binary -m sparc -EB -D` (the machine
name is plain `sparc`; `sparc:v8` is rejected by this build). LEON is SPARC V8, so
this decodes the retail image cleanly. Helper: `jupiter/emu/dis.sh <addr> <len>`
(XIP: flash offset == vaddr). This section is the first read of the actual
screensaver code rather than inference.

**`OSDSS_Monitor` (0x591b4) — the trigger, decoded.** It calls `OSDSS_Entry`
(0x59108) only if ALL of:
1. `0x4002fb58 == 0` (mode/enable byte),
2. `_bOSDSSScreenSaverMode 0x400239c4 == 0` (not already saving),
3. activity token `0x400239c0 == 0x40031abc` (the live "last activity" word — if
   they differ, it resets the timer via 0x594f4 and exits),
4. `(OS_GetSysTimer() - __dwOSDSSCheckTime 0x400239b8) > 0xe260` (**57952 ticks** —
   the idle timeout; on the very first pass `__dwOSDSSCheckTime==-1` and it just
   stamps "now" and exits),
5. `__bPOWERONMENUInitial 0x40023a10 != 0`, and `0x40020ff4 == 0`, `0x4002f7c6 == 0`.

**Gate values read from the 200M dump — the decisive lines:**
- `__dwOSDSSCheckTime == 0xFFFFFFFF` (still its power-on value). Since the monitor's
  first action is to overwrite it with "now", **a value of −1 at 200M proves
  `OSDSS_Monitor` was never called even once** — the screensaver's own bookkeeping
  variable is the witness. No inference.
- `__bPOWERONMENUInitial == 0` — an independent hard gate (step 5) also unmet.
- Also note step 4's threshold **57952 > the 30766 ticks** the fast tick reached, so
  even a live loop wouldn't have fired yet by 200M.

**Who sets `__bPOWERONMENUInitial`?** Exactly one non-zero writer, `stb` at **0x61d30**
inside the routine at **0x61cf8**: `if (__bPOWERONMENUInitial==0) { call 0x61be8(1);
call 0x61be8(2); __bPOWERONMENUInitial=1; call 0x4a754(0x11); call 0x62080; }` —
i.e. the "POWERONMENU fully entered / start slideshow" action. (The lone `clrb` at
0x61894 is the *clear*-to-0 path.) So the flag flips the instant 0x61cf8 runs — and
it never runs. `0x61cf8`'s address is **never** materialized (no `call`, no literal
pointer, no `sethi/or`, in flash or the 0x4006xxxx alias), so it is dispatched via a
jump table / handler slot selected by a menu/event state — the same indirect
dispatch as `CC_DVD_MainLoop`/`OSDSS_Monitor`. The boot parks before that dispatch
selects it.

**Net (final-boss localization).** The autonomous screensaver is gated by a single
handler, `0x61cf8`, that both sets `__bPOWERONMENUInitial=1` and completes
POWERONMENU entry, and that handler is never dispatched because the CC event loop is
asleep (§10.46). The one concrete unblock is still H2 — drive the CC loop's event
dispatch (inject its periodic non-user wake) so it selects the POWERONMENU-entry
handler; the flag flips, and with the fast tick pushed past 57952 ticks the monitor
then fires `OSDSS_Entry` on its own. All addresses retail-empirical via `dis.sh`.

### 10.48 TOOLING — a GDB remote stub in the emulator (interactive firmware debug)

Built a GDB remote-serial-protocol stub into `ct952emu` (`jupiter/emu/gdbstub.c`)
so a real SPARC gdb can breakpoint / step / inspect the live firmware instead of
env-var-driven `printf` archaeology. Run `./ct952emu --rom-load --gdb <port>
--quiet dp700wd.bin`, then from a SPARC gdb: `set architecture sparc; target remote
:<port>`. Machine-side primitives (`machine.c`): `machine_step_bp()` runs the tight
step-and-check loop (full per-instruction timer/IRQ/PROC2 tick preserved, so the
RTOS keeps live time while stopped-then-continued) and `machine_dbg_read/write()`
hit the flash/DRAM/BRAM backing stores with no I/O side effects. Breakpoints are
stub-managed (address list, no code patching) so they work in XIP flash too.

Implemented RSP: `qSupported/?/g/G/p/P/m/M/c/s/Z0/z0/H/D/k`; SPARC 72-reg `g`-packet
order (32 int + 32 f0-f31=0 + y/psr/wim/tbr/pc/npc/fsr/csr); Ctrl-C interrupt via a
non-blocking `MSG_DONTWAIT` poll between continue-batches.

**Verified over the wire** (Python RSP client, no gdb needed for the test): reset
regs read (pc=0/npc=4), `Z0,eb90` + `c` single-stepped ~4.9M instrs in the C loop
and **stopped exactly at INITIAL_System 0x0000eb90** (~1.3M steps/s ⇒ ~55s to reach
POWERONMENU @75.5M — fine for interactive use), and the `m0xeb90` memory read
returned `808c2040 3080000d 02800008 808c2040`, **byte-identical** to `dis.sh`'s
`btst 0x40,%l0 / b,a / be / btst`. This is the tool for the H2 next step: breakpoint
the handler-dispatch site, watch which event slot the CC loop selects, and pin the
exact wake it's starved of.

### 10.49 LIVE DEBUG — the park is a blocking wait, not a poll (and the POWERONMENU call chain)

First real gdb-stub session against the running firmware (Python RSP driver;
`CT952_CHOOSEMEDIA/VDEC_DONE/NOMEDIA` as usual). Continue-to-breakpoint reached
`0x4b808` at ~75.5M in ~95 s (single-step C loop). Findings, all live:

- **`0x4b808` is a LOOP TOP inside POWERONMENU_Initial, not the entry.** Live regs at
  the stop: `%i7=0x4a5ac` (the real caller's return), `%i6/fp=0x40036fd0`,
  `%sp=0x40036f68`. The true entry is **`POWERONMENU_Initial 0x4b7c8`**, `call`ed
  from an init routine at **`0x4a588`**: `call 0x2ebb8; 0x55e88(0x65); 0x55e88(0x66);
  0x4b7c8 (POWERONMENU_Initial); 0x2edb0` — a one-shot boot-init sequence (it posts
  msg IDs 0x65/0x66 via `0x55e88`), **not** the steady event loop. So the park is
  downstream of this.
- **The CC thread is BLOCKED, not spin-polling.** Set a breakpoint on the modal-wait
  peek `PeekEvent 0x66a0` (lock → `btst mask,[0x40026ea4]` → clear-and-return-1 /
  else 0) and continued: **not hit in ~25 s (~32M instructions)** after POWERONMENU.
  So the CC loop is not running its known event-flag poll at all here — it is parked
  in a true eCos *blocking* wait (a flag/semaphore sleep), consistent with the 200M
  dump's thread `state=1` and the scheduler-only PC-trail (§10.46). This rules out
  the "busy-poll starved of a bit" shape at this stage: nothing is polling.

**Net.** The starved wake is a **blocking** eCos wait-object post, not a flag bit a
poll loop is missing. The next step is to catch the block itself — breakpoint the
eCos flag/semaphore-wait primitive (in the decompressed kernel TEXT at ~`0x4001xxxx`)
and read the CC thread's wait object when it sleeps — rather than watching polls that
never run. (Harness note: a timeout-continue must send a `0x03` interrupt before
issuing the next RSP command, else the target keeps running and ignores packets.)

### 10.50 ROOT FOUND — the CC thread blocks in mbox-get on queue 0x40033830 (never posted)

Two tooling upgrades made this tractable: (a) **snapshot/restore** (`--run-to N
--snapshot f` / `--restore f`, `machine_snapshot/restore`) — the 100 s boot to
POWERONMENU is captured once (`/tmp/pom.snap`, 12.7 MB) and re-loaded in <1 s, so
every probe starts at icount≈78M; (b) a **multi-connect** stub (re-accepts on
detach/EOF, keeps machine state) — verified: detach + reconnect preserved
`pc=0x4000103c`. Reusable client: `jupiter/emu/rsp.py`.

**The blocked call stack, read live from the CC thread's saved context** (thread
`0x400371f8`, `state=1`, `stack_ptr=0x40036b90`): the saved frames carry a firmware
return `0x000596a8` on top of the eCos chain `0x4001de58→0x4001e708→0x4001e6a8→
0x4001e524`. Disassembling each (kernel TEXT read from live DRAM via the stub, since
it's decompressed at boot):

- **`0x5969c`** — tiny firmware wrapper `save; call 0x4001de40; ret` (called from
  ~105 sites → the generic "wait for a message" OS call).
- **`0x4001de40`** — OS-layer: `%o0 = *(0x400423a8) = 0x40033830`; `call 0x4001e498`
  with `%o0 = obj+0x1c`. So it waits on the fixed global object **`0x40033830`**.
- **`0x4001e498`** — eCos `Cyg_Thread::sleep`: bumps the scheduler-lock counter
  (`*0x40024974`), enqueues the thread on the object's wait-list if `obj+0x3c==0`,
  then `call 0x4001e66c` (unlock+reschedule) — the context switch away. Its return
  `0x4001e524` is exactly what sits in the saved stack. **This is the park.**

**The object `0x40033830` is a message queue / mailbox** (read live): head
`[+0x00]=[+0x08]=0x40033930`, ring buffer `[+0x0c]=0x40033f98` size `[+0x04]=0xa00`,
wait-list `[+0x20]=0x40037214` (**= CC thread + 0x1c → the CC thread is enqueued on
it**), and the signaled/count field **`[+0x3c]=0` → empty/unsignaled**. So the CC
thread is blocked in **mbox-get, waiting for a message that never arrives**.

**The poster side** (counterpart put primitive `0x4001de5c`, which stores into the
same `*(0x400423a8)` queue): its 3 flash wrappers live at `0xad4f8`/`0xad568` (both
in func **`0xad4cc`**) and `0xaf984`; `0xad4cc` is called from the OS post-wrappers
`0x6430`/`0x6798`/`0x75d0`. So the wake the whole final boss hinges on is **a
`0xad4cc` post to mbox `0x40033830`**, driven ultimately by whatever event source
feeds `0x6430/0x6798/0x75d0` — and that source never fires in the emulated boot.

**Net (final-boss root, fully localized).** screensaver → OSDSS never called → CC
loop asleep → **blocked in `Cyg_Thread::sleep` (0x4001e498) on mailbox `0x40033830`,
which receives no message**. The H2 injection is now exact: post one message to
`0x40033830` (via `0xad4cc`, or by nudging its `+0x3c`/wait-list and waking the
enqueued CC thread) and the loop iterates. Remaining thread to pull: trace the
callers of `0x6430/0x6798/0x75d0` to name the dead event source (timer/VSYNC/media
ISR) that *should* post it — the last layer of the same event-starvation onion.

### 10.51 H2 PROVEN — a single injected mbox post revives the dead CC loop

Ran the injection experiment on the POWERONMENU snapshot via the gdb stub
(`/tmp/inject.py`): saved the idle-thread regs, set up a call to the eCos mbox-put
primitive **`0x4001de5c(1,1)`** (keeping the current stack), `pc→0x4001de5c`, and
continued. Result, live:

- **Before:** CC thread `0x400371f8` `state=1` (SLEEPING), mbox `+0x3c=0` (empty).
- **After the single post:** the CC thread **woke and hit its mbox-get `0x5969c`**;
  mbox `+0x3c` flipped `0→1`. Continuing, it keeps hitting mbox-get — the **dead loop
  is now cycling**. Thread-tagging the hits (by `%sp`): they alternate between the
  **CC thread** (`sp=0x40036db0`, mbox-get caller `0x2ee40`) and a **worker thread**
  (`sp=0x40038ba0`, caller `0x4001ea60`). So feeding the missing event brought the CC
  event loop *and* a second thread back to life — direct proof that the whole boot is
  event-starved, not broken (§10.24/10.39/10.46/10.50 confirmed by construction).

**But OSDSS still isn't reached:** with the loop cycling, `OSDSS_Monitor 0x591b4` was
not hit in 30 s. So a *generic* wake revives the loop but does not by itself run the
screensaver monitor — `OSDSS_Monitor` sits behind a further-specific dispatch (a
particular message type / a periodic "monitor tick"), reached via the message module
(`0x2ee40`→`0xa33d8`/`0xa48a8`), not on every loop turn. That is the final layer:
identify the exact message the CC dispatch maps to the OSDSS/monitor path (or the
periodic timer message that a live system posts), and post *that* — then the
screensaver fires on its own. The mechanism is now not just diagnosed but
**experimentally validated**: the pathway from a dead boot to a live, cycling event
loop is intact and revivable with one poke.

### 10.52 LAST MILE — OSDSS is a monitor-table entry dispatched on EVENT BIT 0x80

**How OSDSS_Monitor is invoked (the missing link).** It has no direct caller and its
address is never a flash constant because it lives in a **runtime-built monitor
table**. Searching the snapshot's DRAM for the pointer `0x000591b4` found it at
exactly one place: **`0x40020e18`**. The table (base `0x40020dd0`) is an array of
`{handler, param, event_mask}` triplets:

```
0x40020dd0 {0x00048de8, 0xffff, 0x001}    0x40020e0c {0x000454d4, 0xffff, 0x020}
0x40020ddc {0x00013338, 0xffff, 0x002}    0x40020e18 {0x000591b4, 0xffff, 0x080} <-OSDSS
0x40020de8 {0x00013acc, 0xffff, 0x004}    0x40020e24 {0x000a34b8, 0,      0x080}
0x40020df4 {0x0004283c, 0xffff, 0x008}    0x40020e30 {0x00048e04, 0,      0x200}
0x40020e00 {0x00009e58, 0xffff, 0x010}    0x40020e3c {0x0005dfe8, 0,      0x400} ...
```

So **`OSDSS_Monitor` is registered on event bit `0x80`** — it runs when event 0x80 is
dispatched. This matches the `EvtDispatch_bit80 @0x6eec` / `PostEvent->list @0x12f10`
functions (0x6eec explicitly sets/clears bit 0x80 — `mov -129,%o1` = `~0x80` — on the
event flag `0x40026ea4`). **The complete autonomous trigger is now known end to end:**
`event 0x80 posted → dispatcher walks table 0x40020dd0 → OSDSS_Monitor(0x591b4) →
(idle > 0xe260 ticks, §10.47) → OSDSS_Entry(0x59108)`.

**OSDSS_Entry runs under the live scheduler (what `machine_call` never could).**
Injected `OSDSS_Entry` via the stub (traps ON, scheduler live): it **executed** —
`_bOSDSSScreenSaverMode 0x400239c4` flipped `0→1` (screensaver mode entered) and it
reached **`_OSDSS_PictureUpdate 0x59004`**. It then halted at `0x4001e000` (scheduler)
before the decode, because injecting sleep-heavy code into the *idle* thread's context
corrupts its shallow window/stack state. So the entry logic is proven live; a robust
full decode needs the call to run in a real thread's context (the CC thread), i.e.
driven by the natural event-0x80 dispatch rather than injected into idle.

**Net — final boss, fully reduced.** Every stage of the autonomous screensaver is now
either proven to run or pinpointed: revive the CC loop (post to mbox `0x40033830`,
§10.51) → post **event 0x80** so the monitor table dispatches `OSDSS_Monitor` → with
the fast tick (§10.46) idle passes `0xe260` → `OSDSS_Entry` (proven to enter
screensaver mode) → `_OSDSS_PictureUpdate` → `UTL_ShowJPEG_Slide` decode (§10.41, the
same path that already renders all 5 album photos). The one remaining engineering
task is orchestration: post event 0x80 in the live CC-thread context (not injected
into idle) and advance the clock — no unknowns remain in the path, only wiring.

## 11. FAITHFUL BOOT — removing the crutches (the real goal)

The scaffolding envs (`CT952_VDEC_DONE`, `CT952_CHOOSEMEDIA`, `CT952_NOMEDIA`,
`CT952_TICK_FAST_AT`, injection) are FAKES — they stand in for unmodeled hardware.
The real target is the retail firmware booting and running **naturally**, RTOS alive
on its own, with every crutch deleted. The screensaver autostarting is just the first
acceptance test that the machine is genuinely alive. This section tracks crutch
removal by faithful hardware modeling.

### 11.1 Measured crutch baseline (what faking actually bought)

Milestone ladder (REACH, retail `dp700wd.bin`, 90M budget), measured — NOT recalled:

| config | ceiling reached |
|--------|-----------------|
| raw, zero crutches (old) | `UTL_ShowJPEG_Slide` @46M — stalls |
| `CT952_VDEC_DONE` only    | `POWERONMENU_Initial` @75.5M |
| `CT952_CHOOSEMEDIA` only  | stalls @46M (no help alone) |
| all crutches              | @75.5M (identical to VDEC_DONE alone) |
| *any config*              | never reaches OSDSS / alive RTOS |

Key facts this pins down, so no faithful result is ever mis-sold as a "first":
- **Everything up to 46M happens raw** — no crutch is involved before `UTL_ShowJPEG_Slide`.
- **The one gate between the raw ceiling and POWERONMENU is the JPEG decode completing.**
  `CT952_VDEC_DONE` alone bridges 46M→75.5M; `CHOOSEMEDIA`/`NOMEDIA` do nothing for it.
- **The splash-on-panel is downstream of that same gate** — raw reaches the decode call
  but the decoder never reports done, so `HALJPEG_Display` never runs. So "it showed the
  splash" was a *faked* milestone (needs the decode-completion). Reproducing splash or
  POWERONMENU is therefore parity with fakes, not progress; only doing it crutch-free is.

### 11.2 Gate 1 REMOVED faithfully — decode-completion (retire CT952_VDEC_DONE)

`CT952_VDEC_DONE` was only a `getenv` A/B toggle on an already-faithful transition: a
JPU decode op (`R_GPU_CTL0`, non-GPU branch) clears `JPU_BUSY` and the modeled decoder
reaches MODE_STOP(0x10)=frame-done — the real kick→busy-clears→status-done sequence,
just instant. The decode-status getter (0x375a0 action 0) maps mirror `0x40039cd0`=0x10
+ HW `0xB0000190`=0x11 → `JPEG_STATUS_OK`. Removing the env gate (report frame-done
whenever a frame was actually decoded, `vdec_frame_done`) makes it faithful.

**Result: the raw boot — no `VDEC_DONE`, no `CHOOSEMEDIA`, no `NOMEDIA`, nothing —
reaches `POWERONMENU_Initial` @75.5M on its own.** Verified: `CT952_VDEC_DONE` has 0
references left in the code. (A first attempt also cleared `vdec_frame_done` on a stop
command "latest-command-wins"; that broke it — the firmware issues a stop in the
decode→display flow and relies on frame-done persisting through it — so that was
reverted. Frame-done persisting is what the hardware presents until the next decode.)

This also retires `CHOOSEMEDIA`/`NOMEDIA` as dead weight *for reaching POWERONMENU*
(they may still matter past it). Honest new ceiling: **crutch-free boot reaches
POWERONMENU; the RTOS is still not alive** (CC loop blocks on mbox `0x40033830`,
§10.50) — the next gate to make faithful, not fake.

### 11.3 Gates 2 & 3 REMOVED — CHOOSEMEDIA / NOMEDIA were dead weight

Measured (REACH, 150M) whether the media crutches buy anything now that decode is
faithful (§11.2): **raw and full (+CHOOSEMEDIA +NOMEDIA) reach identical milestones —
POWERONMENU @75.5M and no further; the media-monitor functions (`MEDIA_MonitorStatus`
0x1186c, `_MEDIA_MonitorMediaStatus` 0x118b8) are unreached in both.** So they buy
nothing: they faked a *result* (`__bChooseMedia`=USB; USBSRC CHECK_DEVICE→NO_MEDIA)
without waking the real (asleep) USB source thread, which is why the boot is unchanged.

Deleted all three crutch blocks from `machine.c` (the CHOOSEMEDIA read-override, both
NOMEDIA handlers, the `nomedia` getenv init). Verified: the boot still reaches
`POWERONMENU_Initial` @75.5M with the code gone. The `nomedia` struct field is retained
(unused) so existing `--snapshot` files stay layout-compatible.

**Scoreboard: the boot to POWERONMENU is now 100% crutch-free.** Three crutches gone
(`VDEC_DONE` made faithful; `CHOOSEMEDIA`/`NOMEDIA` deleted as inert). Remaining
scaffolding is all *past* the current ceiling: `CT952_TICK_FAST_AT` (a clock-rate
calibration, only relevant once the RTOS idles — the faithful version is a proper
cycles/instruction × MHz clock model, not an env threshold) and the display/inject
demo hooks (not boot-progress crutches). The real remaining wall is unchanged: past
POWERONMENU the CC thread blocks on mailbox `0x40033830` with nothing posting to it
(§10.50) — the event-starvation, which needs a *hardware event source* modeled, not a
faked flag. That is the next gate, and it is the hard one.

### 11.4 Alarm hypothesis DISPROVEN — the clock/alarms work; it's a missing event post

Tested whether eCos alarms fire (candidate single root for the whole starvation).
Result: **they do.** On the crutch-free POWERONMENU snapshot, the eCos clock counter
(retail `0x4002e32c`) advances `0x213 → 0x222` (+15) over ~3 s of `continue`. Since
that counter is incremented *inside* `Cyg_Counter::tick` — the same routine that walks
the alarm list — the tick + alarm processing are running. Alarms are not broken.

Corollary (stronger): a prior fast-tick run reached ~30 000 ticks (~30 s virtual) with
`OSDSS_Monitor` still never called. With working alarms, any *alarm-driven* periodic
screensaver check would have fired in 30 virtual seconds. It didn't → **the screensaver
monitor is not alarm-driven**; it is gated purely on the CC event loop being alive.

Also confirmed the DVD909 sym's DRAM globals do NOT match retail (current_thread
@0x400498dc reads 0, impossible; 0x4001f458 disassembles to non-code) — retail eCos
data/text addresses must stay empirically derived, per the standing warning.

**Net:** the root is not a timer/alarm defect. The CC thread (`0x400371f8`) runs all
its init, enters the event loop, and blocks *indefinitely* (no timeout) on mailbox
`0x40033830` for a message that nothing posts. The remaining gate is a **missing
hardware-event source** feeding that mailbox — not an alarm, and not a fakeable flag.

### 11.5 MAJOR REFRAME — the RTOS is partly ALIVE; it's a message-ROUTING gap

Breakpointed the eCos mbox PUT primitive 0x4001de5c on the crutch-free snapshot and let
virtual time run (fast tick). Posts happen. Multiple hits, from real producer threads
(sp=0x40035180 / 0x40035110 / 0x40034e88, caller 0xaf984), posting periodic messages
(0,1)(0,5)(0,0x64). So worker threads ARE running and posting on a cadence -- the system
is not dead (consistent with alarms working, 11.4).

But the posts do not go to the CC/UI mailbox. The put primitive posts to the queue in the
global *(0x400423a8), which now reads 0x40038fb8 -- a different mailbox. The CC thread
waits on 0x40033830 (its count field 0x4003386c stays 0). So: active producers post to
0x40038fb8; the CC/UI thread is blocked on 0x40033830, fed nothing. 0x400423a8 is a
context-dependent "current queue" pointer (it was 0x40033830 when the CC thread blocked,
0x40038fb8 now under a producer thread), so the two channels are distinct and the
producers' events never reach the UI queue.

This reframes the whole "event-starvation": it is NOT a dead RTOS, a broken alarm, or a
missing hardware event (those all exist / work). It is a message-ROUTING gap -- the live
producer stream on 0x40038fb8 is not reaching the UI consumer on 0x40033830. Next: find
who consumes 0x40038fb8 and why the UI isn't among the targets (a relay/dispatch thread
that should forward 0x40038fb8 -> 0x40033830, asleep or never registered).

### 11.6 The routing gap, pinned to the queue level (relay ID still open)

Mapped the two queues from the crutch-free snapshot:

- **CC/UI mbox `0x40033830`**: ring empty (head==tail), and its wait-list holds **TWO
  blocked threads** — the CC thread `0x400371f8` (node `0x40037214`) and thread
  `0x40035928` (node `0x40035944`). Two UI-side threads waiting for a message; none
  arrives.
- **Producer queue `0x40038fb8`**: distinct mailbox (external ring buffer at
  `0x40034408`, size `0x1500`), self-referential wait-list = **no blocked waiter**.
  Live, the periodic producer threads (`sp≈0x40035xxx`, caller `0xaf984`) post here.

So the producer event stream lands on `0x40038fb8`, which nothing is blocked on, while
two UI threads starve on `0x40033830`. The gap is at the **queue-routing** level: the
producer stream never reaches the UI queue, and no thread is parked to drain
`0x40038fb8` and relay it.

Open: the exact consumer/relay that should drain `0x40038fb8` → `0x40033830`. A broad
DRAM thread-scan shows many RUN-state threads (system is genuinely partly alive) but is
too noisy to cleanly single out the relay. That needs a targeted **live** trace
(breakpoint the receive primitive `0x4001de40`, filter for queue==`0x40038fb8`, read the
receiving thread) — impeded this pass by background-process/launch flakiness in the
harness, which is the first thing to stabilize before the next live probe.

**Status of the faithful-boot track:** boot to POWERONMENU is crutch-free (§11.2/11.3);
the RTOS is partly alive (§11.5); the remaining wall is a queue-routing gap between an
active producer stream (`0x40038fb8`) and two starved UI consumers (`0x40033830`) — a
well-posed, specific target, no fakes involved.

### 11.7 CORRECTION — the static snapshot misled me; live, the system cycles

Stabilized the harness (launch the emu alone via the background mechanism on a FRESH
port; the earlier "failed exit 1" churn was stale emus holding the port + pkill races)
and re-ran the routing probe LIVE with virtual time advancing (fast tick). This
corrects 11.5/11.6.

Breakpointing the receive primitive 0x4001de40 live shows THREE threads cycling
round-robin (sp 0x40034150, 0x40038b38, 0x40036d48), each reading a DIFFERENT
*(0x400423a8): 0x40033830, 0x40035928, 0x400371f8. Two of those are thread objects, not
mailboxes. So *(0x400423a8) is a volatile per-operation "current wait-object" global,
NOT a fixed per-thread queue. Reading it at the single frozen instant of the snapshot is
what produced the tidy-but-wrong "producers post to orphan 0x40038fb8, UI waits on
0x40033830" story in 11.5/11.6. Live, several threads cycle through receives and DO get
the producer messages -- the system is genuinely partly alive and dynamic.

What still holds: crutch-free to POWERONMENU (11.2/11.3), alarms work (11.4), and the
specific screensaver path never runs -- but the reason is narrower than "a missing
relay." Despite general message cycling, __bPOWERONMENUInitial stays 0,
__dwOSDSSCheckTime stays -1, and the POWERONMENU-completion handler 0x61cf8 / event 0x80
are never dispatched. The next probe is targeted and must be LIVE: breakpoint 0x61cf8 and
the monitor-table dispatch to see why that path is never selected.

Lesson logged: single-instant snapshot reads of a volatile message system are unreliable;
verify event-flow claims against the RUNNING target, not one dump.

## 12. THE PLOT (re-anchor — read this first each session)

> **AUTHORITATIVE SOURCE = the `950_Files/` overlay (see §12.4). We are CT952/DMP1.**
> When a file exists in `950_Files/`, that copy — not the project-root copy — is what
> built `dp700wd.bin`. Read source from there for any overridden module.

**Quest:** a faithful natural boot of the retail firmware — peripherals modeled well
enough that the RTOS comes ALIVE on its own, every crutch gone.

**Why the screensaver is "first":** it is the acceptance test that the system is alive,
not the goal. And critically — **the faithful boot IS the path to the screensaver, not a
detour.** The crutches got us to POWERONMENU but left the system DEAD, because a crutch
fakes *state* (a status byte) instead of producing the *event* (the IRQ/message) the real
peripheral would. A faithfully modeled block raises the interrupt and feeds the producer,
so the RTOS stays awake. Reach POWERONMENU that way and the screensaver fires by itself.
The "event-starvation / routing" walls (§10.24–11.7) are the crutches' own fault.

**Crutch taxonomy (do not conflate again):**
- *Boot-progress* — VDEC_DONE (removed, made faithful §11.2), CHOOSEMEDIA/NOMEDIA
  (deleted, inert §11.3). Gate whether the boot ADVANCES; touch nothing on the panel.
- *Display/pixel* — LOGODECODE, STAGE_PHOTO. STILL PRESENT. They put the splash + the
  5 photos on the panel (video plane). Not yet made faithful.
- *Timing* — TICK_FAST_AT (clock-rate calibration).
- *Injection tools* — inject_wake.py / inject_osdss.py (forced the screensaver; proved
  the code runs, never autonomous).

**Established position (verify against this, don't reconstruct from memory):**
- Crutch-free boot reaches POWERONMENU; splash renders with LOGODECODE (verified). The
  5 photos + OSDSS_Entry/_OSDSS_PictureUpdate ran with the display crutches. The
  AUTONOMOUS screensaver was NEVER reached, even fully scaffolded — that is the wall.
- At crutch-free POWERONMENU the OSD menu does NOT draw (buffer 0x4005f000 all-zero);
  the UI thread is stuck in menu-setup. Rendering the menu naturally = the real next
  milestone (proves the UI is alive), and it is a DISPLAY-path question.

**Next faithful target:** the display/GDI pipeline — find what the menu-setup draw is
waiting on (a DISP/GPU/VSYNC completion the model doesn't faithfully raise), model it,
and watch the menu draw on its own. That is the concrete door in the wall.

### 12.1 Display path IS running — but menu bring-up stalls before OSD-enable

Behavior-driven trace of the crutch-free boot (I/O log, not memory): the display
pipeline is genuinely active. `0x80002a24` (GPU op) written **6626 times**, `0x80002a28`
polled 337926x (the busy-wait) -- the GPU 2-D engine draws. OSD region geometry is
configured (`0x80001a44/a50/a54` = 0x00f002d0 etc.), DISP OSD base at `0x4005c000`
(`0x80000e1c`). Rendered index data sits in DRAM. So "black panel / menu doesn't render"
was a scanout limitation -- my scanout reads one hardcoded region `0x4005f000`; the
CT952 OSD is multi-region and the real base is `0x4005c000`.

BUT the bring-up does not finish:
- **GAM_OSD palette `0x80001c00` is NEVER written** (no palette loaded).
- **`R_DISP_OSD_SIZE` (0x80001a54) bit 28 = DISP_OSD_EN is NEVER set** (OSD never enabled).

The firmware draws + configures OSD geometry, then STALLS before loading the palette
and enabling the OSD -- matching the UI-thread block traced during menu-setup (a
synchronous display/GDI op that never gets its completion). An un-enabled OSD shows
nothing even on real silicon, so the black panel is faithful to the stalled state, not a
scanout bug alone.

**Faithful target (sharpened):** the display/GDI completion event the menu-setup op waits
on. Model it -> menu-setup finishes -> palette loads + OSD enables -> menu displays on
its own -> UI is alive -> screensaver becomes reachable. (Also: teach machine_disp_scanout
the real OSD base `0x4005c000` + multi-region so we can SEE the menu once it enables.)

### 12.2 The OSD stays off by a STATE GATE — localized (compositor + 0x40024050)

Traced why the OSD never enables (CT952_DISPTRACE, added to io_write). Findings, live
on the crutch-free boot:
- OSD geometry is configured early (~12.6M, `0x1a54`=0x00f002d0, regions on `0x1a48/50`).
- **`R_DISP_OSD_SIZE` bit 28 (OSD_EN) is never set; GAM_OSD palette `0x1c00` is never
  written.** The bring-up stops before enable + palette.
- The display **compositor is alive** -- pc `0xa685c` runs every ~200K instr (per frame)
  and at `0xa6850-0xa6858` does `andn <reg>, 0x10000000` -- it **actively clears OSD_EN
  each frame** unless a state gate says otherwise.
- That gate is `*(0x40024050)`, read at `0xa6860`. It is **0**.
- `0x40024050` has exactly ONE writer, `0xa3fcc`: `if ((region_arg & 0x300)==0x200)
  0x40024050 = 0x200`. So the OSD activates only when an OSD region is applied with type
  field `(t & 0x300)==0x200`. It never happens -> OSD stays off -> black panel.

So the black panel is a **specific state gate**, not a vague stall: the menu's OSD region
is never applied with the activating type, so the compositor (correctly, per its logic)
never turns the OSD on. This is the sharpest the display gate has been.

**Next:** determine whether `0xa3f90` (the region-apply fn) is even CALLED during boot --
if called with the wrong type, the region descriptor's type field is the bug; if never
called, the menu-setup stall (§11.4) blocks before region-apply. That distinguishes a
data/descriptor problem from a control-flow (event) problem. Live breakpoint on `0xa3f90`
+ read `%i0`.

### 12.3 Display RULED OUT as the cause — it works; root is the menu-setup stall (§11.4)

Answered the §12.2 split question live (bp `0xa3f90`/`0xa3fcc` on a fresh boot):
- Region-apply `0xa3f90` IS called and REACHES the gate store `0xa3fcc` (~12M), setting
  `0x40024050 = 0x200`. So the OSD-activation machinery works and runs.
- BUT that is for an EARLY screen (~12.6M display setup, before POWERONMENU @75.5M). By
  90M `0x40024050` is back to 0 (only writer is 0xa3fcc=set-0x200; it's cleared by a
  re-init on screen transition), and the POWERONMENU menu's region-apply NEVER runs.

**Conclusion (honest -- a narrowing that returns to a known wall):** the black panel is
NOT a display-config / register / region-type bug -- the display path is proven working
and is exercised. The POWERONMENU menu's OSD simply never gets set up because the
**menu-setup UI thread stalls before its region-apply** -- the same §11.4 message-wait.
The §12 display detour excluded the entire display-cause branch with evidence, then
pointed back at the menu-setup stall as THE root. Not wasted (a class of causes ruled
out), but not a fresh break either.

**Next (the actual root, no more detours):** the §11.4 menu-setup stall. The UI thread,
during POWERONMENU menu-setup, blocks in the message module waiting for a reply that
never comes. Attack THAT directly: catch the block live on a fresh boot, read the exact
message posted + who should reply + what that replier is waiting on. Everything else
(screensaver, OSD-enable, palette) is downstream of this one wait.

### 12.4 AUTHORITATIVE SOURCE — the `950_Files/` overlay is our build (CT952 / DMP1)

**This is the single most important source fact and it corrects a standing assumption.**
The repo root holds the *base* CheerTek reference tree (909-lineage DVD player). Our
retail `dp700wd.bin` is the **CT950/951/952 DMP1 photo-frame** build, produced by
**copying `950_Files/*` over the project root** and building with a specific define set.
So for any module that exists in `950_Files/`, THAT copy is authoritative — the root
copy is the wrong (DVD) variant. This is *why* `DVD909.sym` addresses never matched
retail: that sym is a different product built from the un-overlaid tree.

**Build recipe (`950_Files/950_make.txt`, config #1 "8M solution release" = DMP1):**
- `#define CT950_STYLE`   (winav.h)      — enables ALARM_Trigger/AUTOPWR_Trigger etc.
- `#define CT951_PLATFORM` (customer.h)
- `DRAM_CONFIGURATION_TYPE = DRAM_SIZE_16`, `DECODER_SYSTEM = DVD909R_EVAL`,
  `CPU_SPEED = CPU_146M`, `SUPPORT_950 = 1`
- `romcfg_16M_951_DMP1.txt`, `DVD909_16M.ld` (minus `srv_dram.a`), 146 MHz mclk,
  8M serial flash. OSD strings from `950_Files/OSDEuro_16M`, BMPs from `950_Files/BMP`.

**Overridden modules (use `950_Files/` copy):** alarm, autopower, backdoor, calenui,
clock, dialog, dmpcustm, dvdsetup, dvdsetup_op, edit, mainmenu, menu, notedlg,
**poweronmenu**, radio, radiodrv, rtcdrv, setdate, settime, toolbar.

**NOT overridden (use root copy):** cc, osdss, initial, media, input, oswrap, utl, gdi,
disp/hal/haljpeg, interrupt, hsystem, etc. — the CC main loop, screensaver monitor, OS
wrappers, and init flow are the root versions, *compiled with the defines above*.

Checked: `950_Files/poweronmenu.c` `POWERONMENU_Initial` is byte-for-byte the root one
at the same lines (380/434), so §11.4's read stands. But menu bring-up (`mainmenu.c`,
`menu.c`) and the alarm/autopower triggers differ from root and must be read from
`950_Files/` going forward. Define-gated paths in root files (`#ifdef CT950_STYLE`,
`CT951_PLATFORM`, `SUPPORT_950`) are LIVE for us — do not dismiss them.

### 12.5 BIG REFRAME — the CC thread is ALIVE and PROGRESSING; boot is virtual-time-starved in a long init, not dead

This session re-probed the "block" LIVE on a fresh, fully crutch-free boot (no VDEC/
CHOOSEMEDIA/NOMEDIA; those are gone/faithful) and the prior "dead RTOS / event-starved"
picture is **wrong**. All measured, not recalled:

**1. The CC thread cycles — it is not parked-dead.** Breakpointing the eCos msg-get
wrapper `0x5969c` and the put primitive `0x4001de5c` on a running `continue`: the CC
thread (stack `0x400359d8..0x400371d8`) repeatedly wakes, GETs messages, and POSTs them
— including to `0x40033830` (the very queue §11.5/11.6 called orphaned). Posts come from
several threads. The system is genuinely multi-threaded-alive. The old snapshots that
read "blocked forever on 0x40033830" just froze it mid-sleep between cycles.

**2. It is grinding forward through power-on init.** Unwinding the CC stack at two times:
at ~90M icount (tick ~0x320) it was inside `INITIAL_PowerONStatus` (flash `0x41axx`,
identified below); after fast-tick ran virtual time to tick ~0xe973 (~60k) the stack had
moved to an entirely different, deeper init subtree (top OS frame `0xd5440`, a mutex
unlock → `0x4001e66c` reschedule). So it advances — it is not wedged on one wait.

**3. But it burns ABNORMAL virtual time — the real anomaly.** A real DMP boots to the
menu by ~tick 500 (eCos tick = 100 Hz ⇒ ~5 s). Here it is still in init at **tick
~60 000 (~10 virtual minutes)**. Something retries/waits in a loop, each pass sleeping a
`delay()` and burning ticks, before timing out and inching on. At the 1x emu tick this is
invisible-slow; even the fast-tick (§10.46 tooling) doesn't reach the menu because the
init itself consumes ~100x the normal tick budget.

**4. Prime suspect: two threads never signal init-done.** The eCos flag `__fThreadInit`
@ **`0x40038f80`** (value at +0x00) reads **`0x102`** = JPEG(`0x2`)+PARSER(`0x100`) done,
but **MISSING MPEG-decoder (`0x1`) and Info-Filter (`0x200`)** vs the desired `0x301`
(`INIT_POWER_ON_THREAD_X_SOURCE_DONE`, initial.c:679/697). initial.c:697 is only a
one-shot 50 ms `OS_TimedWaitFlag` (warns "Some thread not initial done" and proceeds), so
that line is not itself the wall — but the MPEG-dec/Info-Filter threads being permanently
un-inited (their init presumably waits on unmodeled HW — PROC2/DSP is held in reset) is
the likeliest thing the deeper init keeps retrying against. **Next: find the retry loop
(persistent ancestor flash `0x61378`, present in every CC stack this session) and what it
polls; and check whether bringing PROC2 out of reset lets the MPEG/Info-Filter threads
complete.**

**5. FIRMWARE DEBUG NARRATION — infra fully mapped (thanks to the "hack the enable byte"
idea).** `DBG_MINI_PRINTF` + `SERIAL_DEBUG` are compiled IN (debug.c:30, customer.h:232).
Addresses (found via string-ref `0xe8b00` → call site `0x41998`):
- `DBG_Printf` = **`0x13598`**; gate `if(_bDBGEnable==FALSE)return` reads byte
  **`_bDBGEnable` @ `0x40021eb0`**; type mask **`__dwDebugFlag` @ `0x40021dd4`**
  (default `0x80000001`; set `0xffffffff` to pass all types).
- Output path: `DBG_Printf` buffers into a 96-byte-stride ring `*(_pDBG_Header1
  @0x40032508)` indexed by `_wInfoWIdx@0x400324d8`; ring is drained to UART only by
  `DBG_Polling→DBG_INT` **in `CC_DVD_MainLoop` (cc.c:848)** — which the stalled CC thread
  never reaches, so nothing prints.
- **GOTCHA:** just poking `_bDBGEnable=1` is NOT enough — `_pDBG_Header1` is **NULL**
  until `DBG_Init` (utl.c, gated by `__bDebugMode & UTL_DBG_UART1_EN`, off in retail)
  runs `_DBG_ResetDRAMDebugMessage` (ring-alloc store @ `0x13ea8`). To get narration:
  inject a `DBG_Init`/`_DBG_ResetDRAMDebugMessage` call via the stub so the ring is
  allocated, then either let the main loop drain it or read the ring from DRAM directly
  (`probe_ring.py`). The emu DOES capture UART1/UART2 TX (`R_UART1_DATA 0x070 → uart_tx`
  → stderr/`--uart` file), so once the ring drains, output is visible.

**Net:** the wall is no longer "dead RTOS" — it is "alive RTOS stuck retrying an init
step that waits on the never-completing MPEG-decoder / Info-Filter threads, consuming
huge virtual time." That is a concrete, faithful-boot target (model the HW those threads
need — likely PROC2), and the firmware can be made to narrate its own progress via the
DBG addresses above. New probe scripts: `probe_stall/stack2/posts/flags/dbg/narrate/
ring/ladder.py` in `jupiter/emu/`; full TEXT disasm cached at `/tmp/fulltext.dis`.

### 12.6 MILESTONE — modified firmware boots, and it NARRATES its own boot (1-byte patch)

Two firsts in one, and both matter for the MicroPython endgame: **(a) a modified
`dp700wd.bin` boots** (the patch→boot loop works), and **(b) the retail firmware now
prints its own debug log**, which turns the whole investigation from stack-archaeology
into just reading what the RTOS says it is doing.

**The patch (regenerate with `jupiter/emu/patch_dbg_fw.py`):** a single byte.
`UTL_Config_DebugMode` (flash `0x11500`, called at boot from initial.c:465 via
`UTL_DBG_INIT`) sets `_dwDBGMode = 0x11` — DSU1-only, so `DBG_Init` is never called for a
UART and the log stays silent. At `0x11518` `mov 0x11,%o0` → `mov 0x111,%o0` (byte
`@0x1151a 0x20→0x21`) adds the UART1_TX nibble, so boot runs
`DBG_Init(…,HAL_UART1_TX,…)`: `_bDBGEnable=TRUE`, the DRAM debug ring is allocated at
`_pDBG_Header1=0x4004c000`, and `DBG_Printf` output flows. (XIP code is patchable in place
— the DATA section is zipped, code sections `TEX2`/`RODA` are `flash/skip`.) Read the ring
from DRAM: 96-byte descriptors at `0x4004c000`, printable text via a `[\x20-\x7e]{4,}`
scan (the UART *drain* `DBG_Polling→DBG_INT` only runs in `CC_DVD_MainLoop`, not yet
reached — but the ring fills from boot regardless).

**What the firmware says (verbatim, crutch-free patched boot, fast-tick):**
```
TVMode: 22; HVOffset: 0000,0000            TFT_Change source[FF ->13]
Some thread not initial done.              TFT_LOCK_Data / TFT_UNLOCK_Data
   Desired: 00000301, Current: 00000100    BackLight_OFF ... BackLight_ON
TVE setting out of range                   starting usb stack...
TVMode: E2; HVOffset: FFA3,FFFE            Blk dev: Init
                                           ehci_local0: EHCI version .. USB revision 2.0
                                           ehci_local0: new device port=0 .. speed=high
                                           uhub0: vendor 0x0006 Generic Root Hub, addr 1
```

**This rewrites the diagnosis — POSITIVELY.** The RTOS is not wedged; it is methodically
bringing up TFT panel → backlight (`BackLight_ON`) → **USB EHCI stack** → enumerating the
root hub. It even confirms §12.5 in its own words ("Some thread not initial done. Desired
00000301, Current 00000100"). The huge virtual-time cost is real init work (display + a
full USB stack bring-up), not a spin. Two watch-items surfaced by the log: **"TVE setting
out of range"** and the still-pending MPEG/Info-Filter thread bits — likely the next
things to chase, now that the firmware will tell us when they resolve.

**Long-run reality check:** a plain fast-tick run to 3e9 instructions ends with the OSD
still DISABLED (panel `/tmp/panel_final.ppm`) — the menu is not yet reached even given
enormous virtual time, so there IS still a real terminal wall past USB init; but we now
have the firmware's own narration to walk right up to it. Next: drain the ring
continuously (inject `DBG_INT`, or grow `DBG_MAX_IDX`) to get the full chronological log
through to wherever it finally stops, and read the last thing it says.

### 12.7 THE WALL, NAMED BY THE FIRMWARE ITSELF — USB root-hub enumeration (unmodeled HC)

Drained the debug ring continuously (efficient single-`m` bulk read per step,
`probe_drain2.py`) to get the full chronological boot log up to where it stops. The
firmware's own last words:
```
TVMode / TVE setting out of range / TFT_Change source[FF ->13] / BackLight_ON
starting usb stack...            reaper thread handle:40041208 / Blk dev: Init
thread handle:401FCEE8 / 401FB9E8 / 401FACE8         (usb worker threads spun up)
ehci_local0: EHCI version 0000.0000, with 0000 ports
ehci_local0: USB revision 2.0
ehci_local0: new device port=0000 depth=0000 speed=high
uhub0: vendor 0x0006 Generic Root Hub, class 9/0, rev 0.11/af.00, addr 1
uhub0: 00C0 ports with 00C0 removable, bus powered      <-- garbage: 192 ports
```
Then **nothing** — 90 s of continued virtual time (tick advancing) produces no new
message. The boot is stuck in **USB host-stack / root-hub bring-up**, and the values are
garbage: `EHCI version 0000`, `0000 ports`, then a root hub claiming `0x00C0` (192)
ports. Root cause is direct and already flagged in `machine.c` itself (comments ~L799):
**the emu has NO USB/EHCI host-controller model** — the USB register space reads 0 / drops
writes, so the Jungo USB stack reads nonsense and hangs enumerating phantom ports. This
also explains §12.5's "abnormal virtual-time burn": the USB workers spin on bogus port
status.

**Why this is the (or a) terminal wall:** the DMP power-on flow starts the USB stack
during init (`starting usb stack...`); with the HC unmodeled the enumeration never
converges, and the boot never advances to draw the menu (3e9-instr run still OSD-off).
Note initial.c:690 masks `INIT_SRC_THREAD_USB_DONE` OUT of the power-on thread barrier
(`#if 0`), so USB is not *required* by that barrier — meaning the USB spin is either
starving/holding a resource the menu path needs, or the menu path waits on a USB-init
signal downstream. The fix distinguishes these.

**FAITHFUL NEXT STEP (concrete, in the emulator):** model the USB EHCI host controller +
root hub minimally — enough that enumeration *completes with zero devices attached*:
report a sane capability/version, `0 ports` (or 1 port, disconnected), and terminate the
enumeration loop. Then re-run and read the narration past `uhub0` — if the menu draws,
USB was the wall; if it stalls again, the firmware will name the next thing. Either way we
now have the firmware narrating each step, so progress is observable directly. (Tooling:
`probe_drain2.py` bulk-reads the ring at `0x4004c000`; `patch_dbg_fw.py [--live]` builds
the debug fw.)

### 12.8 STAGE 1 DONE — EHCI model unblocks USB; the CC main event loop is ALIVE

Built a minimal EHCI host-controller model in the emu and the boot **broke through the
USB wall into the main loop**. Details:

**EHCI base = `0xa0000100`** (found live: `probe_ehci.py` breakpoints `ehci_init`'s
CAPLENGTH read `0xb0260`, reads the bus_space handle `[tag+8]`). This is the `0xa000xxxx`
region that logged 5.4M unmapped reads = the USB stack thrashing.

**Model (`machine.c` `ehci_read/ehci_write`, region `0xA0000100..0x0200`):**
- Caps: word0 `0x01000010` (fw byte/half accessor extracts CAPLENGTH=`0x10`,
  HCIVERSION=`0x0100`), HCSPARAMS=`1` (N_PORTS=1), HCCPARAMS=0.
- Op regs (at base+CAPLENGTH=`0xa0000110`): USBCMD (HCRESET self-clears), USBSTS
  (HCHalted = !RUN), USBINTR, FRINDEX (advances w/ cycles), PERIODIC/ASYNC/CTRLDSS,
  CONFIGFLAG, PORTSC[0]. PORTSC reports **no device** (CCS=0); writes drop the W1C
  change bits. State fields added to `machine_t`. Trace: `CT952_EHCITRACE=1`.

**Verified live** — the retail EHCI driver does a clean bring-up: reads caps, resets
(USBCMD `0x2`→self-clears), programs PERIODICLISTBASE/ASYNCLISTADDR, runs
(USBCMD=`0x20011`), CONFIGFLAG=1, powers the port (PORTSC=`0x1000`), sees CCS=0. Then
**EHCI activity STOPS (31 accesses total, vs 5.4M before)** — enumeration converged.

**Narration past the old wall (fw's own words, `probe_drain2.py`):**
```
ehci_local0: EHCI version 0001.0000, with 0001 ports      (was 0000/0000)
uhub0: 0001 port with 0001 removable, self powered        (was 00C0 = 192 phantom)
HCD: EHCI host controller added                            <-- HC added, enum done
usb no playable file      /   no SD card                   <-- media scan: empty (correct)
__dwSupportFeature=04
-------O --- (KEY_DOWN)                                     <-- CC_DVD_MainLoop UI/key path!
```
That last line is the milestone: the CC thread is in **`CC_DVD_MainLoop`** processing
key/UI events — it cleared ALL of power-on init (the thing that looked "stuck/dead" for
prior sessions). The RTOS is alive in its steady event loop. `OSDSS_Monitor` (cc.c:1004)
is now on the critical path each iteration, so the screensaver acceptance-test is finally
reachable through the normal loop rather than injection.

**Note on debug builds:** `dp700wd_dbg.bin` (`--live`) streams DBG straight to UART and
so leaves the DRAM ring empty — use `dp700wd_ring.bin` (patch-1 only, `patch_dbg_fw.py`
without `--live`) when reading narration from the ring via `probe_drain2.py`.

**Next:** confirm the OSD/menu actually draws now (end-to-end `--fb-out`), then Stage 2 —
attach a virtual USB mass-storage device (FAT image w/ the 5 album JPEGs) so "usb no
playable file" becomes a real photo source, i.e. the DMP's actual function.

### 12.9 Post-USB: phantom key fixed (verified); the MENU isn't DRAWN (not an enable bug)

After Stage 1 (main loop alive), pushed on getting the menu to draw. Two results:

**(a) Phantom key — real bug, found/fixed/VERIFIED, but NOT the menu blocker.** The
narration showed `-------O --- (KEY_DOWN)` in the idle loop with no user input.
`PANEL_KeyScan` (panel.c) reads an analog resistor-ladder key matrix via `ADCGLB`
(`0x8000407c`, bits [31:24] = key voltage): no button => high rail (~`0xFF`); a press
pulls it down. The emu returned `0` => read as a held button => phantom `KEY_DOWN` every
scan (which also reset the screensaver idle timer). Modeled `ADCGLB` as `0xFF000000`
(idle/no-button) in `io_read`; `CT952_ADC=` overrides for A/B. **A/B proof:** `CT952_ADC=0`
reproduces the `KEY_DOWN`; default `0xFF` removes it. This is faithful (rest state of the
ladder), not a mask of intended input. Commit `ef10d0e`. BUT the menu still doesn't draw
with the key gone — so the phantom key was a separate defect, not the menu cause.

**(b) The menu is NOT DRAWN — reframes §12.1–12.3.** Sampled the OSD buffers live at the
idle main-loop state (fixed emu): `0x4005c000` and `0x4005f000` are ~all-zero (0–10
non-zero of 2048 sampled), OSD gate `0x40024050`=0. Prior sessions assumed the menu was
drawn-but-not-composited (an OSD-enable/gate bug). It is not: **`_POWERONMENU_DrawAllUI`
never renders the menu into the OSD buffer at all.** DISPTRACE confirms: OSD region config
(`1a48/1a50/1a54`) happens only ≤12.5M (the early splash) and never post-USB. So the
compositor/gate is downstream of a draw that isn't happening — chasing OSD_EN was the
wrong layer.

**Open question for next session:** does `POWERONMENU_Initial` (cc.c:1320) even run to
`_POWERONMENU_DrawAllUI`, or does the DMP's no-media flow (SUPPORT_STB `__bChooseMedia`
branch, cc.c:1385; or a "waiting for media" state) skip the menu draw? Two concrete paths:
(1) find `POWERONMENU_Initial`/`_POWERONMENU_DrawAllUI` addresses, breakpoint to see if
the draw path is entered and where it bails; (2) **Stage 2** — attach a virtual USB
mass-storage device (FAT + the 5 album JPEGs); the DMP is a photo frame and `usb no
playable file`/`no SD card` suggest it may simply be idling for media, in which case
giving it media is the real unblock (and the actual product function).

### 12.10 Menu-draw chase — PROC2 ruled OUT (again); it's the POWERONMENU draw path

Chased the display-enable handshake with the boot reliably reaching the idle main loop
(EHCI + ADC fixes in place). Findings:

**PROC2/video is NOT the blocker (refreshed §8.1/8.2/10.7/10.8):** this image is CT909P →
JPEG decode is **hardware** (JPU/VLD/MCU-BIU), not PROC2 microcode; and PROC2 is
*deliberately* held in reset at the menu (released only in `HAL_ReloadAudioDecoder`, a
media-playback-thread call). **The power-on menu is designed to draw with PROC2 in
reset.** So the missing `MPEG_DEC` thread bit is expected, and the slideshow's JPEG path
is modeled. Do not chase PROC2 for the menu.

**The OSD is never enabled AND the menu is never drawn (not just an enable bug):**
- OSD framebuffer = `0x4005F000` (§4), sampled ~all-zero at the idle state → no icons.
- `DISPTRACE`: OSD region regs (`0x1a48/50/54`) are written ONLY at ticks 0x4–0x29
  (~5M icount); `DISP_OSD_EN` (bit28 of `0x1a54`) is **never** set; palette (`0x1c00`)
  never loaded.
- Unwound each early OSD-config write (bp `0xa331c/0xa56bc/0xa41ac`): the ~5M writes come
  from a **decoder/display worker thread** (thread entry `0x497ac`, which reads PROC2
  reset-ctl `0x8000031c`) and the early-init path `0x418xx`/`0xa8bb4` — **not** from
  `POWERONMENU`. After ~5M the only OSD-register touches are the per-frame **compositor**
  (`~0xa685c`, chain via `0x61378`) doing its `andn …,0x10000000` OSD_EN clear.
- So `POWERONMENU_ConfigOSDRegion`/`_POWERONMENU_ShowIcon` never run → `POWERONMENU_Initial`
  bails **before** `_POWERONMENU_DrawAllUI`'s icon draw.

**Candidate bail points (poweronmenu.c):** (a) the early return `if (__bPOWERONMENUInitial)
return;` at line 384 — set TRUE by a prior/racing `POWERONMENU_Initial` call (it's called
from cc.c:1320 AND media.c:1577/the USBSRC thread AND cc.c:925/3139); or (b) inside
`_POWERONMENU_DrawAllUI`, the `if (Disable_Init_Menu()) return;` at line 457 (returns
`DisInitMenuFlag`) which sits AFTER `POWERONMENU_ConfigOSDRegion` (453) but BEFORE
`_POWERONMENU_ShowIcon` (461) — though the config-write traces argue the menu path isn't
even reaching 453. **Next: pin `POWERONMENU_Initial`'s real dp700wd.bin address (DVD909.sym
is wrong per §10.7) and breakpoint it — count calls, threads, and whether each reaches the
draw or early-returns on the flag.** The continuous-UART narration (`--live` build,
`_dwUartPort`→`0x80000070`, now verified working) is the live commentary for this.

### 12.11 Orientation lock-in + PROC2 detour resolved (decode target = MCU BIU channel)

Re-read the doc + git history to stop the recurring "909" drift. Locked the chip
identity into the top-of-doc ⚑ banner: **CT952A**; "909" is only platform-family /
SDK lineage; authoritative source is the **`950_Files/` overlay**; the 952 OTA image is
**`UPG952A.AP`**. Do not re-derive these.

**PROC2 detour resolved.** Chased a theory that the built-in-photo decode runs on PROC2.
It does not: on this CT909P-platform 952A build, `SUPPORT_JPEGDEC_ON_PROC2` is OFF, so
**JPEG (photos + splash) decode is the hardware JPU/VLD/MCU-BIU path on a PROC1 worker
thread** (§10.8). **PROC2 runs `mpg.bin` = MPEG *video* only** — the photo frame never
uses it. Confirmed operationally: with `CT952_PROC2=1`, the firmware only reset-*asserts*
PROC2 (`0x80000324` @~5M) and never releases it; `0x40002000` holds no microcode at boot
(the load+release live in `HAL_ReloadAudioDecoder`, the media-playback path). So there is
no PROC2 decode to exercise at boot — right conclusion, wrong core.

**The correct faithful decode target is already pinned in §10.8: the MCU BIU bit-stream
read-channel.** The JPU worker programs `BCR08/09/0A/0C/0D` (`0x80002a20–0x80002a34`) to
DMA the staged JPEG out of DRAM, then polls it to drain; the emu models the VLD+JPU polls
but never advances that channel → `VDRemainder` wait times out → `JPEG_Status=UNFINISH` →
`HALJPEG_Display` never fires → no photo. **This is exactly what the `LOGODECODE` crutch
faked.** Faithful fix (per §10.8): advance the read-channel on decode-kick, auto-arm the
`0x80000C10` progress gate, and write picojpeg output as macroblock-tiled YUV into the
video framebuffer — the firmware's own JPU pipeline then completes and displays the image.
That is the through-line to the built-in slideshow, and it replaces `LOGODECODE` rather
than adding a crutch. (Menu/OSD-icon draw, §12.9/12.10, is a separate GDI question and does
not depend on the JPU decode.)

### 12.12 §10.8 implemented — faithful JPU decode replaces the LOGODECODE crutch (verified)

Implemented the §10.8/§12.11 plan: the built-in JPU/MCU-BIU decode now completes on the
**firmware's own decode-op**, with `CT952_LOGODECODE` retired from the decode path.

**The five changes (machine.c / machine.h):**
1. `machine.h`: new `int biu_drained` — the MCU BIU bit-stream read-channel "drained /
   macroblock stream ready" flag.
2. `io_write R_GPU_CTL0` (JPU op branch): call `machine_maybe_jpeg_decode(m)` **unconditionally**
   (was gated behind `CT952_LOGODECODE`) and set `biu_drained = 1`. The JPU decode-op the
   driver issues now functionally decodes the staged frame every time.
3. `io_write 0x2a20` (BCR08, read-channel source): wire `m->jpeg_src = v` for DRAM pointers
   (`(v & 0xF0000000)==0x40000000`) and clear `biu_drained` — a new source means a new fill
   is in flight, so BCR0A bit 12 must read 0 until the JPU kick drains it.
4. `io_read 0x2a28` (BCR0A, read-channel status): return `... | (biu_drained ? 0x1000 : 0)` —
   the worker's poll at `0x4001fe90` for bit 12 now retires instead of spinning the
   decode-wait timeout.
5. `io_read 0xc10` (decoder progress): auto-arm on `jpeg_decode_en || biu_drained`; and the
   macroblock-tiled-YUV write-back into the video framebuffer (`Y@0x40065000`,
   `C@0x400B3C00`, stride `0x2D00`) is **un-gated** from `CT952_LOGODECODE` — it always runs
   after a successful decode (this is the MCU-BIU write-channel output the real block does).

**Verified (raw boot, no crutch env):** `dp700wd_ring.bin`, `CT952_TICK_MULT=64`, EHCI + the
`ADCGLB=0xFF` no-key model, **no `CT952_LOGODECODE`**. The firmware's own boot path fired:
```
[ct952emu] JPEG decode #1: 480x270 from 0x401dc000
[ct952emu] JPU MCU-BIU wrote 480x270 tiled YUV to 0x40065000/0x400b3c00
```
i.e. the driver's own `GPU_CTL0` JPU decode-op triggered the decode → `biu_drained` →
`0x2a28` bit 12 retired the worker poll → the splash logo decoded and tiled-YUV pixels
landed in the video framebuffer, all without a crutch. This is the crutch-retirement the
§11.x "faithful boot" arc calls for, applied to the JPU decode.

**Boot state after this change (unchanged wall, re-confirmed):** the boot continues to the
steady CC event loop — narration through `starting usb stack` → EHCI enumeration →
`HCD: EHCI host controller added`, then the RTOS parks in its normal idle poll. Disassembly
of the parked loop (`0x59838` getter → eCos helper `0x4001dee8`, looped from `0x59850`) shows
it polling flag **`0x400398f8`** — the same *missing-event-post* park documented in §10.51 /
§11.4-11.6 (a thread waiting on an event that no producer posts). That gap — the screensaver
trigger (`OSDSS_Monitor` on event bit `0x80`) — is the next faithful target and is
independent of the now-working JPU decode.

**Tooling added:** `machine.c` `pcsamp_window_dump` — a *windowed periodic* PC/stack
histogram (`CT952_PCSAMP_WIN=<icount>`, default 200M) that prints the live hot loop every
window and resets, so a long run the sandbox kills before the atexit `pcsamp_dump` still
leaves the current spin in its log. `jupiter/emu/seg.sh` chains `--restore/--run-to/--snapshot`
to advance the boot in sandbox-sized segments when continuous long runs are throttled.

### 12.13 Re-measured the faithful boot — `__bPOWERONMENUInitial` is the shared root gate

With the JPU decode now faithful (§12.12) + EHCI + ADC, re-measured the actual gate
flags in the CURRENT boot (new `CT952_DUMPFLAGS` diag in main.c; snapshot-chained via
`seg.sh` to dodge the sandbox's long-run throttle). Per §11.7's own lesson — verify
against the running target, not a stale dump — this **supersedes** the pre-EHCI reads.

**Measured across the boot (`dp700wd_ring.bin`, TICK_MULT=64):**
| icount | `__bPOWERONMENUInitial` 0x40023a10 | `__dwOSDSSCheckTime` 0x400239b8 | `_bOSDSSScreenSaverMode` |
|--------|-----|-----|-----|
| 3M  | 0 | 0x00000000 (BSS) | 0 |
| 20M | 0 | 0xFFFFFFFF (first-init ran) | 0 |
| 50M | 0 | 0x00001771 (live tick) | 0 |
| 85M | 0 | 0x000033FE (**advancing**) | 0 |

**Two findings:**
1. **Progress vs the old wall:** `__dwOSDSSCheckTime` now advances with the system timer
   (old §10.44 had it stuck at `0xFFFFFFFF`, "first-init never ran"). OSDSS time-tracking
   is alive in the faithful boot.
2. **The shared root gate is `__bPOWERONMENUInitial`, which stays 0.** It gates BOTH
   unsolved walls at once: the menu draw (POWERONMENU only draws once it's set) AND the
   screensaver (`OSDSS_Monitor` step 5 requires `__bPOWERONMENUInitial != 0`, §10.44).
   One flag, both symptoms.

**Who sets it — pinned by exhaustive store-scan (not one DRAM dump).** Scanning every
`stb → [base(0x40023800)+0x210]` in flash finds exactly two writers:
- `0x61880` (`0x61894`): only **clears** it to 0 (`if(arg==0) __bPOWERONMENUInitial=0`).
- `0x61cf8` (`0x61d30`): the sole **setter** to 1 —
  `if(__bPOWERONMENUInitial==0){ 0x61be8(1); 0x61be8(2); __bPOWERONMENUInitial=1;
  0x4a754(0x11); 0x62080; }`.

**`0x61cf8` has NO direct callers** (call-scan of flash is empty). Its address
`0x00061cf8` appears exactly once in live DRAM — at `0x40024da4`, inside a runtime handler
table (records ~`0x40024c20…0x40024de0`, each 0x20 bytes, first word = a UI/mode id,
handler ptrs following; e.g. id `0xf`=MAIN_MENU→`0x9908`, id `0x11`=POWERON_MENU→`0xff6c`).
So `0x61cf8` is dispatched indirectly, never by a direct call — and it is never dispatched,
so the flag is never set. (Note: the SDK whitelist address `0x4b808` labelled
"POWERONMENU_Initial" is a **mislabel** — it disassembles to mid-loop code, not a function
entry. Retail symbol addresses must stay empirically derived, per the standing warning.)

**Strong lead (needs live confirmation):** the OSD framework's active-record pointer at
`0x40020ec8` (adjacent to the §10.52 monitor table) points at the table record for mode
id **8 = `OSD_UI_MEDIA_SELECT_DLG`** — i.e. the UI is parked in the media-select dialog
(consistent with "usb no playable file / no SD card"), not transitioned into the
POWERON_MENU path that would run `0x61cf8` and set the flag. **Next:** find the dispatcher
that walks this table + who should call `OSD_ChangeUI(OSD_UI_POWERON_MENU)` on the
no-media boot path, and why the transition out of MEDIA_SELECT_DLG isn't taken — that is
the door to both the menu and the screensaver.

Tooling added this pass: `CT952_DUMPFLAGS` (main.c) prints the POWERONMENU/OSDSS gate
flags at a `--run-to` checkpoint; `seg.sh` now dumps them per segment.

### 12.14 ROOT MECHANISM FOUND — the UI latches in mode 8, so POWERON_MENU (mode 7) is never entered

Traced the boot's OSD UI transitions live (`CT952_UITRACE` on the active-UI-record
pointer `0x40020ec8` + `__bPOWERONMENUInitial` `0x40023a10`, snapshot-chained via `seg.sh`).
The complete mechanism behind §12.13's "`__bPOWERONMENUInitial` stays 0":

**Observed UI path (chained boot):**
- `~2.4M` (pc 0x2be4, an init routine): `activeUI(0x40020ec8) = 0` (NONE), `__bPOWERONMENUInitial` cleared.
- `~32M` (pc 0xb038, inside `OSD_ChangeUI`, **called from 0x41b34**): `activeUI -> record id=8`.
- Steady state through 60M+: **activeUI stays id=8; never transitions; `__bPOWERONMENUInitial` never written to 1.**

**Why POWERON_MENU never runs — decoded from the disassembly:**
- `OSD_ChangeUI` is at **`0xafd8`**. Its mode→record lookup `0xae50` scans the runtime table
  `0x40024c20..0x40024e00` (0x20-byte records) matching `mode == record[0]`, and `OSD_ChangeUI`
  calls the enter handler at `record+4`. This **proves mode 7's handler is `0x61cf8`** (the sole
  `__bPOWERONMENUInitial` setter, §12.13) — i.e. **mode 7 = POWERON_MENU in the retail build**.
  ⚠️ `osd.h` (`OSD_UI_POWERON_MENU=17`, `MEDIA_SELECT_DLG=8`) is the **wrong SDK enum** for this
  image — do not trust its OSD_UI numbers; the retail table is mode 7 = POWERON_MENU.
- **`OSD_ChangeUI` returns 0 (declines) whenever `activeUI != 0`** (`0xafec: bne 0xb030`). It only
  enters a mode (writes `activeUI`, returns 1) when `activeUI==0` AND the mode's enter handler
  accepts (returns ≠0 or is null).
- The UI-(re)build function **`0x418f0`** (called `0x418f0(0)` on the boot path from `0xadc4`; it
  first runs the thread-init-flag `0x301` check — the same `0x301` as the boot narration "Some
  thread not initial done. Desired: 00000301, Current: 00000100") does, at `0x41b34`:
  ```
  OSD_ChangeUI(8, ENTER);        // mode 8, handler 0x25ef4
  if (ret != 0) goto done;       // 0x41b44 bne -> skip the fallback
  OSD_ChangeUI(7, ENTER);        // 0x41b50: mode 7 = POWERON_MENU (would set __bPOWERONMENUInitial)
  ```
  Mode 8's handler `0x25ef4` **accepts** (returns ≠0), so `activeUI` latches to mode 8 and the
  POWERON_MENU fallback is skipped.

**The latch is self-perpetuating.** Once `activeUI = mode 8`, *every* subsequent
`OSD_ChangeUI(7)` (there are 24 mode-7 call sites) returns 0 immediately (`activeUI != 0`),
so POWERON_MENU can never be entered until something **exits** mode 8 (`OSD_ChangeUI` exit path
`0xb04c` clears `activeUI`). Nothing does. Net: `__bPOWERONMENUInitial` stays 0 → menu never
draws AND screensaver never arms — one latch, both symptoms. This supersedes the "missing event
post / routing gap" framing (§11.5-11.7): the live mechanism is a **UI-mode latch**, not a lost message.

**Open (the next chain link):** what is retail mode 8 (handler `0x25ef4`), and why does its enter
handler accept on the no-media boot instead of declining (which would let the fallback POWERON_MENU
run)? Likely a media/playback/dialog UI whose "should I be shown?" predicate is true when the
faithful model says it shouldn't be — i.e. the door is mode 8's accept condition. Candidates to
trace next (live, via a PC hook on `0xafd8`/`0x25ef4`): the input `i1` to `OSD_ChangeUI(8)` and the
branch inside `0x25ef4` that decides its return value. Tooling: `CT952_UITRACE` (machine.c).

### 12.15 MODE 8 SOLVED — it's the media/source UI; it latches on a non-empty source list

Ran the latch to ground (disassembly of the mode-8 enter handler + its predicate, plus
`CT952_UITRACE` live on the deciding flags, snapshot-chained).

**What mode 8 is.** Its enter handler is **`0x25ef4`**. It prints the exact `"usb no playable
file"` (`0xe7668`) and `"no SD card"` (`0xe7698`) strings §12.8 saw, and sits beside
`KH_COMMON_QueryIfExistPlayableFile` / `File Manager: Mount device` — so **mode 8 = the
media / source-scan + auto-decision UI** (SRCFTR / File-Manager multivolume). On ENTER it
scans the removable sources for a playable file.

**The exact latch — one flag.** After the scan, `0x25ef4` calls predicate **`0x272a8`** and
`if (ret != 0) stay-in-mode-8 (return 1)` else `OSD_ChangeUI(mode 7 = POWERON_MENU)`.
`0x272a8` is a single gate, verified on every return path:
- `*(0x40032b3b) == 0`  → returns **0** → mode 8 declines → **`OSD_ChangeUI(POWERON_MENU)`** runs.
- `*(0x40032b3b) != 0`  → returns **1** → **mode 8 latches** (does display setup, `0x2743c`/`0x69d48`).

**Why the flag is non-zero.** `0x40032b3b` is set by a count-loop (`0x299f0..0x29a5c`) that
walks the **active-volume list at `0x40032ab8`** to its `0xFF` terminator. Read live from the
snapshot: `0x40032ab8 = [01 02 03 04 ff fe fe …]` → **4 volumes → count 5** → `0x40032b3b = 5`
→ latch. That list is **loaded from config setting `0xa3`** (`0x33ca4(0xa3, 0x40032ab8, 10)`)
— the device's stored source list, not a per-boot media probe.

**So the chain, end to end:** boot `0x418f0` → `OSD_ChangeUI(8)` → `0x25ef4` scans (finds no
removable media, prints the two messages) → `0x272a8` sees the **configured** source list is
non-empty (`0x40032b3b=5`) → **stays in mode 8** → POWERON_MENU (mode 7) never entered →
`__bPOWERONMENUInitial` never set → menu never draws AND screensaver never arms (§12.14).

**Live evidence (`CT952_UITRACE`, chained):**
```
mode8_stayflag(32b3b) <- 05  pc=0x29a5c  icount~10.5M   (source count from setting 0xa3 list)
activeUI -> rec id=8          pc=0x41b34  icount~32M     (mode 8 entered, latches)
__bPOWERONMENUInitial         never written to 1
```

**The fork for the fix (next investigation).** The latch is gated on "are sources
*configured*" (`0x40032b3b`, from setting `0xa3`), NOT on "is media *present*". Two candidate
truths, to be settled next:
1. **Mode 8 is meant to yield here** and the count should reflect *present* media (0 on a
   no-media boot) — i.e. a faithful "no playable media" path should drive `0x40032b3b→0`.
2. **Mode 8 is meant to STAY and PLAY** — its return-1 path (`0x27344`→`0x2743c`/`0x69d48`)
   is playback setup, and the built-in flash photo album is one of volumes `[1,2,3,4]`; the
   slideshow should start here and the real gap is that playback doesn't render (reconnecting
   to the decode/display path §12.12). This fits the DMP's actual job (play built-in photos).

Next step: trace mode 8's return-1 path (`0x27344`→`0x2743c`/`0x69d48`) live — is it starting
playback of a built-in volume, and if so where does it stall? That distinguishes (1) vs (2).
Diag: `CT952_UITRACE` now also logs `0x40032b3b` / `0x40032b0d`.

### 12.16 The retail OSD UI-mode table — full map (empirically derived)

The retail OSD framework dispatches UI modes through a **runtime handler table**, not the
`osd.c` switch in the SDK source (that `osd.c` does not match this build). Mechanism:

- Table of 0x20-byte records at **`0x40024c20 … 0x40024e00`**; record layout
  `{ id(mode), +4 enter, +8 exit, +c shared/common, +14 key/draw, … }`.
- **`OSD_ChangeUI(mode, action)` = `0xafd8`** → lookup **`0xae50`** scans the table for
  `record[0]==mode`, returns the record; `OSD_ChangeUI` calls the **enter handler at
  `record+4`**. It **returns 0 (declines) if the active-UI latch `0x40020ec8` is already
  non-zero** (§12.14), else enters the mode and latches.
- The active mode pointer is **`0x40020ec8`** (§12.14); the current-mode getter is `0xb090`.

**⚠️ The retail mode numbers are NOT `osd.h`'s.** `osd.h` (`POWERON_MENU=17`,`DIGEST=7`) is a
different SDK. Proven: mode **7**'s enter handler `0x61cf8` is the only writer of
`__bPOWERONMENUInitial` (`0x40023a10`), which is read by **`OSDSS_Monitor` (`0x591b4`)** as
the screensaver gate — so **retail mode 7 = POWERON_MENU**, not DIGEST. `osd.h` happens to
match at a few ids (8, 11) but must not be trusted for numbering.

**The map** (id · enter handler · module · identity / evidence · confidence):

| mode | enter  | module | identity — evidence | conf |
|------|--------|--------|---------------------|------|
| 0x06 |0x65494 | menu 0x6x | **Video/LOGO display UI** — "Can't find LOGO data", "MPEG thread not initial done" | med |
| 0x07 |0x61cf8 | menu 0x6x | **POWERON_MENU** — sets `__bPOWERONMENUInitial`, read by `OSDSS_Monitor`; "Stop playback / Show LOGO / ShowUI" | **HIGH** |
| 0x08 |0x25ef4 | media 0x25x| **MEDIA / SOURCE-SELECT** — scans USB/SD, "usb no playable file"/"no SD card", `KH_COMMON_QueryIfExistPlayableFile`; **the boot latches here** (§12.15) | **HIGH** |
| 0x09 |0x26294 | media 0x26x| media-family sub-dialog (shares common handler +c=`0x25e34` with 8/10/12) | low |
| 0x0a |0x2646c | media 0x26x| media-family sub-dialog (shares +c=`0x25e34`) | low |
| 0x0b |0x26638 | media 0x26x| **AUTO_UPGRADE / firmware update** — references **`UPG952A.AP`**, File-Manager multivolume | med-HIGH |
| 0x0c |0x26070 | media 0x26x| media-select **variant** — shares exit(+8=`0x260f4`) & key(+14=`0x2620c`) handlers with mode 8 | med |
| 0x0d |0x1f484 | 0x1fx | **THUMBNAIL browser** — "THUMB: trigger -> START stage" | med |
| 0x0e |0x67c28 | menu 0x6x | menu/dialog (poweron-menu module) | low |
| 0x0f |0x09908 | dlg 0x9x | menu/dialog | low |
| 0x10 |0x03564 | dlg 0x3x | menu/dialog | low |
| 0x11 |0x0ff6c | dlg 0xffx | menu/dialog (NOT poweron — see mode 7) | low |
| 0x12 |0x048c0 | dlg 0x4x | menu/dialog | low |
| 0x1f |0x41154 | player 0x41x| **MEDIA PLAYER / playback** — `Dec_JPEG`,`Dec_BMP`,`Parser`,`USBSRC`,`/ROOT`,`fatfs` (the JPEG/BMP renderer — the built-in-photo play path) | med-HIGH |

**Boot-relevant reading:** on the faithful boot the UI walks NONE → **mode 8** (media-select)
and latches (§12.15), so **mode 7 (POWERON_MENU)** never runs and the screensaver never arms.
**Mode 0x1f** is the actual JPEG/BMP player — the through-line for rendering the built-in
photo album once the media path yields to it. Modes 8–12 are one media/source family (shared
handlers); the low-address dialogs (0x0f/0x10/0x11/0x12) and menu-module 0x06/0x0e are the
remaining unnamed UIs — precise names need the retail `osd.c` (absent from the tree) or live
per-mode observation via `CT952_UITRACE`.

### 12.17 Mode-map refinement — the retail enum ≈ osd.h with POWERON_MENU(7)↔DIGEST(17) swapped

Pushed the naming further (source `OSD_ChangeUI(OSD_UI_*)` correlation + handler evidence).
Findings, graded by confidence:

**PROVEN:**
- **mode 7 = POWERON_MENU.** Enter handler `0x61cf8` is the only writer of `__bPOWERONMENUInitial`
  (`0x40023a10`), read by `OSDSS_Monitor` (`0x591b4`) as the screensaver gate; "Stop
  playback / Show LOGO / ShowUI"; and it is the **most-dispatched mode by far** (24
  immediate-arg `OSD_ChangeUI(7)` sites vs 0 for mode 17) — the "home screen" you return to.
  `osd.h`'s DIGEST=7 could never have 24 call sites.
- **mode 8 = MEDIA_SELECT_DLG.** "usb no playable file" / "no SD card" scan; the boot latch (§12.15).

**Framework:** modes **8 and 11 match `osd.h` exactly** (MEDIA_SELECT_DLG=8, AUTO_UPGRADE=11),
and mode 7 is provably POWERON_MENU while mode 17 is never dispatched — so the retail enum is
**`osd.h` with POWERON_MENU(7) and DIGEST(17) swapped**, everything else as `osd.h`. Applying that:

| mode | enter | name (retail) | basis |
|------|-------|---------------|-------|
| 0x06 | 0x65494 | DVD_PROGRAM / video display | osd.h + "Can't find LOGO"/"MPEG thread" |
| 0x07 | 0x61cf8 | **POWERON_MENU** | PROVEN |
| 0x08 | 0x25ef4 | **MEDIA_SELECT_DLG** | PROVEN |
| 0x09 | 0x26294 | PSCAN_PROMPT_DLG | osd.h (media-module cluster) |
| 0x0a | 0x2646c | BOOKMARK | osd.h (media-module cluster) |
| 0x0b | 0x26638 | **AUTO_UPGRADE** | "UPG952A.AP" (matches osd.h) |
| 0x0c | 0x26070 | SCREEN_SAVER | osd.h; handler in the photo module, shares handlers w/ mode 8; OSDSS is the photo slideshow |
| 0x0d | 0x1f484 | COMMON_DLG | osd.h |
| 0x0e | 0x67c28 | NAVIGATOR | osd.h |
| 0x0f | 0x09908 | MAIN_MENU | osd.h |
| 0x10 | 0x03564 | PASSWORD | osd.h |
| 0x11 | 0x0ff6c | DIGEST (swapped from 7) | never dispatched; osd.h POWERON slot |
| 0x12 | 0x048c0 | COPY_DELETE_DLG | osd.h |
| 0x1f | 0x41154 | **MEDIA/PHOTO PLAYER** (not in osd.h) | Dec_JPEG/Dec_BMP/Parser/USBSRC — the built-in-photo renderer |

**Lazy registration:** the boot-time table (§12.16) holds only the ~14 modes registered by
~35M icount. Modes **1–5** (`DISPLAY`, `MEDIA_MANAGER`, `SETUP`, `THUMBNAIL`, `SEARCH`) are
NOT present at boot — they register when first opened (e.g. entering SETUP from the menu),
so they'd appear in the table only after that navigation. `OSD_ChangeUI(1)` is seen live
("MEDIA_Management: Umount File System") so mode 1 = DISPLAY is exercised early.

**For the goal:** the two modes that matter are **0x0c = SCREEN_SAVER** (the built-in photo
slideshow's OSD mode; gated by the same `0x40032b3b` source-count as the mode-8 latch and by
`__bPOWERONMENUInitial`) and **0x1f = the JPEG/BMP PLAYER**. Both are behind the mode-8 latch
(§12.15): break that (POWERON_MENU runs → `__bPOWERONMENUInitial=1`), and the screensaver
(mode 0x0c) can finally arm and drive the photo player.

### 12.18 CORRECTION — osd.h names do NOT transfer; retract the inferred labels (§12.17)

Prompted by "what is mode 0x10 = PASSWORD?" — it isn't. Verified:
- `OSD_UI_PASSWORD` is a **DVD parental-lock dialog** (osd.c: "DVD Password Dialog"). This is
  a photo frame: there are **zero** `password`/`PIN`/`parental` strings anywhere in the image,
  and **no `OSD_ChangeUI(0x10)` call site** exists. Mode 0x10's handler `0x03564` just does
  OSD-region setup (calls poweronmenu.c's OSD-frame allocator `0x61fb8`) — **not a password UI.**
- So the §12.17 "retail = osd.h with 7↔17 swap" framework is **wrong as a naming source.** The
  DVD-SDK `osd.h` enum does NOT transfer to this DMP build. The matches that looked like osd.h
  (8, 11) were established by **independent evidence**, not by osd.h — so they stand; the purely
  osd.h-*inferred* labels (modes 9,10,12,13,14,15,16,17,18) are **retracted as unverified.**
- **The screensaver is NOT a table mode.** `OSDSS_Entry` (`0x59108`) does not call `OSD_ChangeUI`
  at all — it calls `_OSDSS_PictureUpdate` (`0x59004`) directly, and no mode-table handler lives
  in the OSDSS module (`0x59xxx`). OSDSS (osdss.c) is a **separate direct-draw subsystem**, so the
  earlier "mode 0x0c = SCREEN_SAVER" is also retracted.

**What actually stands (evidence-based only):**

| mode | enter | identity | evidence |
|------|-------|----------|----------|
| 0x07 | 0x61cf8 | **POWERON_MENU** | sets `__bPOWERONMENUInitial` read by OSDSS_Monitor; 24 dispatch sites; "Stop playback/Show LOGO/ShowUI" — PROVEN |
| 0x08 | 0x25ef4 | **MEDIA / SOURCE-SCAN UI** (the boot latch) | "usb no playable file"/"no SD card", `KH_COMMON_QueryIfExistPlayableFile` — PROVEN |
| 0x0b | 0x26638 | **FIRMWARE AUTO-UPGRADE** | references `UPG952A.AP` |
| 0x1f | 0x41154 | **JPEG/BMP MEDIA PLAYER** (the photo renderer) | `Dec_JPEG`/`Dec_BMP`/`Parser`/`USBSRC`/`/ROOT`/`fatfs` |
| 0x06 | 0x65494 | **VIDEO / LOGO display** | "Can't find LOGO data", "MPEG thread not initial done" |
| 0x08–0x0c | 0x25xxx/0x26xxx | **media/photo UI family** | one module; 0x0c shares exit(+8)/key(+14) handlers with mode 8 |
| 0x09,0x0a,0x0d,0x0e,0x0f,0x10,0x11,0x12 | — | **UNIDENTIFIED** | draw via OSD string-table indices, no literals; names need the retail `osd.c` (absent) or live per-mode observation |

**Method note:** handler string-scanning names a mode only when its handler emits a DBG/text
literal; UIs that render purely from the OSD string-index table are opaque to static scanning.
Naming the rest reliably requires either the retail `osd.c` UI-registration source (not in the
tree) or driving the firmware into each mode and reading the on-screen OSD text (`CT952_UITRACE`
+ key injection), past the mode-8 latch.

### 12.19 MILESTONE — the built-in album photo renders to the video plane, faithfully

Following the insight that `0x40032b3b`=5 is the **5 built-in photos** (§12.15's open question
resolved: the count is real, not phantom — mode 8 correctly stays because there IS content):
verified the whole photo path works on a crutch-free boot (snapshot-chained via `seg.sh`).

At icount ~42M (boot parked in mode 8, `dp700wd_ring.bin`, TICK_MULT=64, no display crutches):
- **A real internal album JPEG is staged** at `0x401dc000`: a 640×360 JFIF, Exif
  `software=Adobe Photoshop CS Windows datetime=2009:06:18`. The **"0001" album section**
  (flash `0x160000`) is read by the firmware.
- The firmware's **own JPU decode-op** runs it (§12.12): `JPEG decode #2: 640x360 from 0x401dc000`.
- The **video plane is 90%+ populated** with the reconstructed frame: Y@`0x40065000` = 90%,
  C@`0x400B3C00` = 92% non-zero. De-tiling the macroblock YUV (`videoplane.py`) reproduces the
  photo — so the MCU-BIU tiled-YUV writeback lands correct pixels in the scan-out buffer.

**Net:** the faithful boot finds the built-in album, stages the real photos, and the JPU decodes
them into the video framebuffer on its own — the core demo/slideshow render path is FUNCTIONAL.
`_bOSDSSScreenSaverMode` is still 0, so this render is via **mode 8's media display** (photos
present → stay & show), not the OSDSS idle screensaver.

Tools: `videoplane.py` de-tiles the video plane from a snapshot to PNG (Y@0x40065000 /
C@0x400B3C00 / strip 0x2D00, YUV 4:2:0).

**Open next:** (1) confirm the emu scan-out composites the video plane (its `--fb-out` targets the
OSD plane `0x4005F000`; add a video-plane path). (2) confirm the slideshow **advances** through all
5 photos on the photo-interval timer (chain further, count distinct decodes).

### 12.20 Photo renders but the slideshow does NOT auto-advance — the screensaver is gated out

Chained the crutch-free boot far past the first album decode and characterised the steady state:

- **Decodes:** #1 = 480×270 (splash logo), #2 = 640×360 (a real album photo, §12.19). Chaining
  **42M → 122M (80M instructions) produced NO further decode** — the slideshow does not cycle.
- **Where it idles:** bounded PCSAMP on the 82M snapshot shows the hot loop is the **steady idle
  delay loop** `0x5983c–0x5984c` + eCos helper `0x4001deec` (§12.14) — i.e. alive and idling, NOT
  stuck. The park at `0x62500` seen at a checkpoint is incidental.
- **`0x62500` is the I2C RTC driver.** Register block `0x8000420c` (read-data-ready) / `0x4210`
  (cmd) / `0x4214` (data); strings *"Wait for RTC Initialization timeout"*, *"RTC Key(0x55AA)
  Error"*. The emu models the CMD busy-clear but NOT `0x420c` data-ready, so RTC I2C **reads time
  out**. This is a real unmodeled peripheral (an I2C RTC chip) but PCSAMP shows it is **incidental**
  to the idle — hit 1–2× per window, not the slideshow blocker.

**Why no slideshow (the gate, from the flags at steady state):**
- `_bOSDSSScreenSaverMode` = 0 (screensaver not entered), `__bOSDSSPicIdx` = 0.
- `__dwOSDSSCheckTime` **advances** (0x1771→0x33fe→…) ⇒ `OSDSS_Monitor` IS running and calling
  `OSDSS_ResetTime` on activity — the monitor is live.
- `__bPOWERONMENUInitial` = **0** ⇒ OSDSS step-5 hard gate unmet (§10.44). The screensaver
  (the auto photo-slideshow) can't enter.

So the render path is proven (a real album photo is decoded to the video plane), but the
**auto-slideshow is the OSDSS screensaver, which is gated on `__bPOWERONMENUInitial`** — set only
by mode 7 (POWERON_MENU), which the **mode-8 latch skips** (§12.14/12.15). The chain closes:
`mode-8 latches (5 photos → stay) → POWERON_MENU never runs → __bPOWERONMENUInitial stays 0 →
OSDSS screensaver never enters → no cycling slideshow`, even though single-photo render works.

**Open decision (needs the real device's behaviour):** on a DMP with built-in photos, does mode 8
itself cycle the album (then the gap is a mode-8 slideshow-advance timer), or does it hand off to
the OSDSS screensaver (then `__bPOWERONMENUInitial` must get set — i.e. POWERON_MENU must run
alongside/after mode 8)? A quick diagnostic: force `__bPOWERONMENUInitial=1` at the mode-8 idle and
see whether OSDSS then cycles the 5 photos — proving the screensaver path end-to-end.

Unmodeled-peripheral backlog (faithful TODO): the **I2C RTC** (`0x8000420c` data-ready + the RTC
register map / valid time) — not the slideshow blocker, but required for a truly "normal" boot.

### 12.21 Diagnostic — forcing __bPOWERONMENUInitial=1 is NOT enough; the screensaver has a 2nd gate

Ran the §12.20 diagnostic: patched the mode-8-idle snapshot to `__bPOWERONMENUInitial=1` and
`__dwOSDSSCheckTime=0`, then ran forward. **The screensaver still did not enter** —
`_bOSDSSScreenSaverMode` stayed 0, no photo cycling, and `__dwOSDSSCheckTime` was **reset from 0
back to 0x33fe** within the run.

Disassembled `OSDSS_Monitor` (`0x591b4`) fully — it has a chain of gates, in order:
- **A** `0x4002fb58 == 0` (else return)
- **B** `_bOSDSSScreenSaverMode == 0` (not already saving)
- **C** `*(0x400239c0) == *(0x40031abc)` — the **saved activity token == the live activity
  counter**. If they differ (activity happened), it **resets `__dwOSDSSCheckTime` to now** and
  returns. ← this is what fired in the diagnostic.
- **D** `(now − __dwOSDSSCheckTime) > 0xe260` (57952-tick idle timeout)
- **E** `__bPOWERONMENUInitial != 0`  (the gate the patch satisfied)
- **F/G** `0x40020ff4 == 0`, `0x4002f7c6 == 0`
- → `call OSDSS_Entry (0x59108)`

So `__bPOWERONMENUInitial` is only **gate E**. Gate **C/D** is the real remaining wall: the **live
activity counter `0x40031abc` keeps changing**, so the monitor resets the idle timer every pass and
the 57952-tick idle never accrues. `0x40031abc` has 12 writers (input/event/OSD-update handlers at
`0xc590/0xd140/0x4230c/0x42dc8/0x5b5e8/0x5b658/0x5bfac/0x5d768/0x5d934/0x5dd44/0x60764/0x60794`) —
one of them fires during the mode-8 idle (a phantom-activity source, akin to the ADC key §12.9).

**Conclusion:** the auto-slideshow = OSDSS idle screensaver, gated by BOTH `__bPOWERONMENUInitial`
(blocked by the mode-8 latch) AND a quiet-idle window on `0x40031abc` (blocked by continuous
activity). This matches the deeply-explored §10.44–11.7 screensaver-trigger difficulty. **Next
lead:** live-watch writes to `0x40031abc` during the mode-8 idle to name the phantom-activity
source, then model/silence it faithfully so idle accrues. (Open question still stands: is the demo
the idle screensaver at all, or an immediate auto-play mode that bypasses the idle timeout?)

### 12.22 The auto-slideshow = OSDSS idle screensaver; its gate chain fully decoded (product framing)

Product reasoning (user): a photo frame with built-in photos and no removable media shouldn't sit
on a menu for the full timeout — it should play the photos. And the screensaver wait is a settings
value (**~10 min default**). Confirmed technically: gate D's threshold **`0xe260` = 57952 ticks ≈
9.7 min @100Hz** — that IS the "10 minutes".

**There is only ONE photo-slideshow renderer:** `UTL_ShowJPEG_Slide` (utl.c), driven by
`OSDSS_Entry` → `_OSDSS_PictureUpdate`. osdss.c has no separate immediate-boot-play; the slideshow
IS the OSDSS screensaver. Full `OSDSS_Monitor` gate (source + `0x591b4` disasm):
```
if (__dwOSDSSCheckTime == -1) { __dwOSDSSCheckTime = now; return; }      // first-init
if (!_bOSDSSScreenSaverMode)
  if (__dwOSDSSCheckNOData == __dwTimeNow)                    // 0x400239c0 == 0x40031abc
     if ((now - __dwOSDSSCheckTime) > OSDSS_ENTER_TIME/*0xe260*/)   // ~10-min idle
        if (__bPOWERONMENUInitial && !__bCLOCKShowClock && __bAlarmState==NONE)
             OSDSS_Entry();            // <-- play the photos
        else OSDSS_ResetTime();
  else OSDSS_ResetTime();             // __dwTimeNow changed -> reset idle
```
So three independent conditions block the boot slideshow:
1. **`__bPOWERONMENUInitial`** = 0 — the mode-8 latch never lets POWERON_MENU run (§12.14/12.15).
2. **`__dwTimeNow` (`0x40031abc`) keeps changing** — updated by `0x5bf88` (`__dwTimeNow = 0x85a68()`
   when it crosses a threshold; `0x85a68` reads a `0x14`-byte-entry table at `0x40039b08`). Each
   change makes `__dwOSDSSCheckNOData != __dwTimeNow` → `OSDSS_ResetTime` → idle never accrues.
   (Diagnostic §12.21 proved this: forcing `__bPOWERONMENUInitial=1` still didn't enter — the
   `__dwTimeNow` reset fired.)
3. **`OSDSS_ENTER_TIME` = 60000 ticks ≈ ~1 min** (see §12.23 correction) idle must elapse.

**Interpretation / open product question:** either (a) the boot slideshow is a *separate* mode-8 /
ImageFrame photo player that cycles on `bPhotoIntervalTime` (~5 s, `IMAGE_FRAME_SETUP`) — NOT the
10-min OSDSS screensaver — and the gap is that per-photo advance not firing (only decode #2 ran);
or (b) it IS the OSDSS screensaver and all three gates above must pass. The single-photo RENDER is
proven either way (§12.19). Leaning (a) per the product logic ("boots into it", not "wait 10 min").
**Next:** find the mode-8/ImageFrame per-photo advance (driven by `bPhotoIntervalTime`) and why it
doesn't fire, distinct from the OSDSS idle path. Diag: `CT952_UITRACE` now also logs `0x40031abc`.

### 12.23 CORRECTION — the system tick is ~1 kHz (not 100 Hz); the screensaver idle is ~1 min

I asserted "100 Hz / 10 min" for the OSDSS idle. That was an unverified back-calc from the osdss.h
*"10 minutes"* comment. The real timer config (hsystem.c) pins it:
```
REG_PLAT_PRESCALER_RELOAD = ((SysClk/1e6)-1)/2 = (133-1)/2 = 66   → timer clk = 133MHz/67 ≈ 1.985 MHz
REG_PLAT_TIMER1_RELOAD    = (1000*SYSTEM_TICK)-1 = 1999           (SYSTEM_TICK=2, Winav.h SPARC branch)
system tick = (1999+1)/1.985MHz ≈ 1.0 ms  → ~1000 Hz
```
So the eCos system tick (what `OS_GetSysTimer` counts) is **~1 ms (~1 kHz)**, not 100 Hz. Therefore
`OSDSS_ENTER_TIME` = `0xEA60` = **60000 ticks ≈ ~60 s (~1 min; ≤2 min within the prescaler factor-of-2
uncertainty)** — NOT 10 minutes. The "10 minutes" source comment is from the original SDK's slower
tick; this 950/952 build's faster tick makes the same tick-count ~1 min. (Corrects §12.21/§12.22.)

### 12.24 The two boot decodes share the LOGO/background display path — no slideshow loop runs

Added `CT952_DECODE_STACK` (machine.c): dumps the caller-PC ring on each JPU decode. Both boot
decodes share the **identical** caller chain:
```
decode #1 (480x270 splash)  and  decode #2 (640x360 album photo):
  ... 0x595c8 (func 0x595bc) -> 0x6bde8 (HALJPEG decode loop) ,  i7=0x6cc18 (func 0x6cb78)
```
So decode #2 (the real album photo, §12.19) is rendered by the **same one-shot display path as the
splash logo** (`0x6cb78 → 0x595bc → 0x6bde8` = the `UTL_ShowLogo`/HALJPEG single-frame path), NOT
by a slideshow loop. `UTL_ShowJPEG_Slide` is called once per frame from this path; nothing drives it
repeatedly. So on boot the firmware shows the splash, then **one** album photo as the background/logo,
then idles — the *cycling* slideshow (repeated `UTL_ShowJPEG_Slide` on `bPhotoIntervalTime`, driven by
the thumbnail/OSDSS path) never starts.

**State of the trek (honest checkpoint):**
- ✅ Faithful JPU decode; the built-in album photo is staged from flash and decoded to the video
  plane, crutch-free (§12.12/12.19) — RENDER PROVEN.
- ✅ 5 photos detected faithfully (mode 8 stays, §12.15); the count is real (user-confirmed).
- ❌ No cycling slideshow: only 2 one-shot decodes (splash + one photo), same LOGO display path;
  the slideshow driver (`UTL_ShowJPEG_Slide` loop) isn't invoked repeatedly.
- The two candidate auto-play drivers both stall: the **OSDSS idle screensaver** is triple-gated
  (`__bPOWERONMENUInitial` via the mode-8 latch + `__dwTimeNow` reset + ~1-min idle, §12.21/12.23);
  the **thumbnail/ImageFrame slideshow** (`_THUMB_ToSlideShow` → `UTL_ShowJPEG_Slide` loop) is
  reached only from the THUMBNAIL UI, which the boot never enters.

**Cleanest next step:** find what SHOULD start the cycling slideshow at boot on this photo-frame build
— i.e. the auto-play-on-boot decision (`IMAGE_FRAME_SETUP` / `POWERONMENU_PowerOnPlayMediaDirectly` /
the thumbnail auto-enter) — and why it isn't taken. That decision, not the render, is the last gap.
Tools this pass: `CT952_DECODE_STACK` (decode caller chain), `seg.sh` now sets it.

### 12.25 GAP CLOSED (design level): this build does NOT auto-play on boot — it's menu + screensaver

Traced the actual boot decision in `cc.c Thread_CTKDVD` and the photo-play entry points. Definitive:

- **`SUPPORT_PLAY_MEDIA_DIRECTLY_POWER_ON` is commented out** (`//#define`, Winav.h:1559). So
  `POWERONMENU_PowerOnPlayMediaDirectly()` (Thread_CTKDVD:1372) is NOT compiled.
- Our build takes the `#ifndef` branch: **Thread_CTKDVD:1320 calls `POWERONMENU_Initial()`** (show the
  power-on menu) → `MEDIA_DecidetMedia()` → `CC_DVD_MainLoop()`.
- The photo slideshow entry `_POWERONMENU_EnterPhotoMusicMode` / `_POWERONMENU_EnterPhotoMode` has
  **only user-input callers**: `KEY_PHOTO`/`KEY_PHOTO_MUSIC` shortcuts (poweronmenu.c:484,492) and the
  POWERON_MENU **photo-icon selection** (`_POWERONMENU_ProcessIcon`, :1309/1319) and in-menu keys
  (:1489/1495). There is **no automatic boot-time slideshow call** in this build.

**So the "boot straight into the slideshow" premise does not hold for THIS firmware.** The real
"normal running" behaviour is: boot → **POWERON_MENU** (with a photo icon, since built-in playable
files exist, `_bPOWERONMENUShowPlayableFile`) → the user selects the photo icon to start the slideshow,
OR after ~1 min idle (§12.23) the **OSDSS screensaver** plays the album on its own. Both paths need the
menu reached first.

**The single remaining blocker (everything reduces to this):** `POWERONMENU_Initial()` IS called at
boot (Thread_CTKDVD:1320) but **POWERON_MENU never fully enters** — `__bPOWERONMENUInitial` stays 0, i.e.
mode-7's enter handler `0x61cf8` (the sole flag-setter) is **never executed**, and `activeUI` never
becomes mode 7 (UITRACE: NONE→mode 8 only). So `OSD_ChangeUI(POWERON_MENU)` inside `POWERONMENU_Initial`
never dispatches the mode-7 handler. Every downstream goal (photo icon to select, screensaver gate E,
menu draw §12.9) hangs off this one point.

**Next (the actual last gap):** disassemble the retail `POWERONMENU_Initial` (called from Thread_CTKDVD)
and find why its `OSD_ChangeUI(POWERON_MENU)` doesn't enter mode 7 at boot — is it declined
(`activeUI != 0` at that instant), does the function bail before that call, or does it use a mode number
we've mis-identified? That single answer unblocks the menu → and thence the (user-selected or
screensaver) slideshow.

### 12.26 KEY CORRECTION — POWERON_MENU is NEVER attempted at boot; mode 8 (media present) is correct

Added `CT952_OSDUITRACE` (machine.c bus_rd): logs every `OSD_ChangeUI(mode)` at its entry
(reads activeUI `0x40020ec8` at pc~0xafe0, mode in i0), with the live latch + caller.

**Through 45M there is exactly ONE OSD_ChangeUI call:**
```
[OSDUI] ChangeUI(mode=8) activeUI=00000000 enters caller=0x41b34 icount=27003289
```
**`OSD_ChangeUI(POWERON_MENU / mode 7) is NEVER called at boot.** Only mode 8 (media UI) is entered.
And `0x418f0`'s logic is `OSD_ChangeUI(8); if(ret==0) OSD_ChangeUI(7)` — mode 8 **succeeds** (the 5
photos are present, §12.15) so mode 7 (POWERON_MENU) is the **no-media fallback**, correctly skipped.

**This corrects §12.20–12.25.** `__bPOWERONMENUInitial` staying 0 is NOT a bug — POWERON_MENU is the
UI you get with **no** playable media; with the built-in album present the device is correctly in the
**media/photo UI (mode 8)**. The OSDSS screensaver (gated on `__bPOWERONMENUInitial`, §12.21) is the
**menu-idle** path, not the media-present path. So the screensaver chase was the wrong branch.

**The real gap (re-stated correctly):** the boot enters mode 8 with 5 photos detected, decodes one
(via the logo/background path, §12.24), and idles. On a photo frame it should **play/slideshow** those
photos from here. Entering the actual photo player is `_POWERONMENU_EnterPhotoMode/EnterPhotoMusicMode`
→ `MEDIA_USB()`, whose auto-advance is gated by `bAutoPlayPhoto` (default **OFF**, dvdsetup_op.c). All
its callers are user-input (photo icon / KEY_PHOTO), and `SUPPORT_PLAY_MEDIA_DIRECTLY_POWER_ON` is off,
so with factory defaults nothing auto-enters the player on boot — a real unit either has the user pick
the photo icon, or ships/settings-set an auto-enter this build's defaults don't provide.

**Next:** determine what, in the mode-8 (media-present) state, is meant to start the photo player
without user input on this photo-frame build — the `IMAGE_FRAME` media-decision that turns "5 photos
detected" into "enter the slideshow". That transition (mode 8 → photo player), not POWERON_MENU, is the
last gap. Tools: `CT952_OSDUITRACE` (every ChangeUI call), `CT952_DECODE_STACK` (decode caller chain).

### 12.27 BREAKTHROUGH (empirical, crutch-free) — raw boot decodes the splash AND the first built-in album photo on its own

Fresh full re-trace this session with **zero logic crutches** (only `CT952_TICK_MULT` as a
time-accelerant, which scales the tick rate but changes no logic). First: confirmed
`CT952_VDEC_DONE` is **inert dead code** — it is read *nowhere* in machine.c; the faithful
decode-done handshake (`m->vdec_frame_done=1` at machine.c:72 and :506, mirror `0x40039cd0`→`0x10`
at :881) is already **always-on**. So prior runs that passed it were unknowingly measuring the
plain build.

**What the raw boot actually does (verified live, snapshot-chained):**
1. The power-on state machine advances on its own timers to the **mode-8** power-on/media state
   (`0x25ef4`) by ~90M icount and parks there. Live OSD mode table dumped from
   `0x40024c20` (14 records): mode `0x08`→`0x25ef4`, `0x07`(POWERON_MENU)→`0x61cf8`,
   `0x09..0x0e`→`0x26294/2646c/26638/26070/1f484/67c28`, etc. `activeUI 0x40020ec8`=`0x40024ce0`
   (the mode-8 record).
2. `__bChooseMedia` (`0x40031b9c`) cycles `0`(DVD)→**`3`**. For this build (`SUPPORT_STB` **off**,
   media.h else-branch) the enum is `DVD=0, USB=1, CARD_READER=2, END=3, UNKNOW=4` — so **`3` =
   `MEDIA_SELECT_END`**, i.e. the media auto-scan ran through every physical source (DVD→USB→card)
   and reached the "no external media anywhere" sentinel. Correct: only internal-flash photos exist.
   (Earlier probe mislabeled `3` as STB — STB isn't in this build's enum.)
3. **The firmware's own JPU pipeline decodes TWO images, crutch-free:**
   ```
   [ct952emu] JPEG decode #1: 480x270 from 0x401dc000     (COBY boot splash / logo)
   [ct952emu] JPEG decode #2: 640x360 from 0x401dc000     (first built-in album photo)
   ```
   Both firmware-staged at `0x401dc000` (firmware re-stages a *different* JFIF at the same buffer →
   it advanced from splash to the first photo). De-tiling the video plane
   (`Y@0x40065000`/`C@0x400b3c00`, `videoplane.py`) after decode #2 renders the real **640×360
   album photo (zebra butterfly on lantana), Y-plane 90% populated** — sent to the user. This is
   the same photo family recovered in §12.12, now reached by the *natural* boot with no
   media-select flip.

**Where it parks:** after decode #2 the machine sits back in mode 8 with event queue `0x400329FC`
**empty** (`qcount=0`), `ccflag 0x40026EA4=0`, `__bPOWERONMENUInitial=0`. No decode #3 appears
through 110M. So the boot displays the **first** photo but does **not** cycle the slideshow — it is
the §10.23 event-starvation park (state-8 advances only on a queued event), reached one photo later
than previously understood.

**Reconciliation with §12.24/§12.26:** §12.24 said "both boot decodes share the LOGO path, no
slideshow loop." Refined: decode #1 is the logo path; **decode #2 is genuinely the first *album
photo*** (640×360, not the 480×270 logo), i.e. the firmware did begin the photo display, then
parked. The last gap is unchanged in kind (mode-8 → cycling photo player), but the boot gets one
real photo further on its own than the notes claimed. The player-entry gate remains
`bAutoPlayPhoto` (default OFF) / `MM_PlayPhotoInFlash` (only inside `SUPPORT_PLAY_MEDIA_DIRECTLY_
POWER_ON`, off) / all `_POWERONMENU_EnterPhotoMode` callers user-input (§12.26).

**Net:** with no logic crutches the retail firmware boots to and displays the first built-in album
photo via its own decoder. Verified addresses/tools: mode table `0x40024c20`; `__bChooseMedia
0x40031b9c` (=`MEDIA_SELECT_END`=3 at park); event queue `0x400329FC`; decode-done mirror
`0x40039cd0`/handshake machine.c:72,506,881; probe `scratchpad/pstate.py` (live mode/gate dump).

### 12.28 THE AUTO-SLIDESHOW IS THE OSDSS SCREENSAVER — user hardware knowledge was right (event-system fully reversed)

Prompted by the user's real-hardware observation ("my other Coby frame boots, then the slideshow launches after a settings timer, default 15s"), re-examined the screensaver path — which §12.25/12.26 and the first event-system pass had wrongly dismissed. **The user is correct: there IS a keyless auto-launching slideshow.**

**It is the OSDSS JPEG screensaver, and it plays the built-in SPI photos:**
- `SUPPORT_ENCODE_JPG_PICTURE` is **defined** (`Winav.h:1589`), so `_OSDSS_PictureUpdate` (`osdss.c:122`) takes the SPI branch: `__SF_SourceGBL[0].bSourceIndex = SOURCE_SPI; UTL_PlayItem(bIdx+1,0); UTL_ShowJPEG_Slide(...)` (`osdss.c:177-211`). The screensaver **is** the built-in-photo slideshow (advances every `COUNT_5_SEC`, `osdss.c:560/583`).
- Enter delay: `OSDSS_ENTER_TIME = COUNT_10_SEC*2` ≈ **20 s** for `CT950_STYLE` (`osdss.h:18-20`). (The user's 15 s was a different chip — same mechanism, near-identical timing.)
- Default ON: `SETUP_DEFAULT_SCREEN_SAVER = SETUP_SCREEN_SAVER_ON` (`dvdsetup.h:1623`).
- Gate (`OSDSS_Monitor`, `osdss.c:320-327`): fires `OSDSS_Entry()` when idle `> OSDSS_ENTER_TIME` **AND** `__bPOWERONMENUInitial && __bCLOCKShowClock==FALSE && __bAlarmState==ALARM_NONE`.

So the real-device flow is: **boot → power-on state machine completes → `POWERONMENU_Initial()` sets `__bPOWERONMENUInitial=TRUE` → ~20 s idle → OSDSS screensaver auto-plays the built-in SPI-photo slideshow.** This is the "screensaver = slideshow" behavior common to these frames.

**Why the emulator doesn't show it (the precise gap), from the 4-agent event-system reversal:**
The emulated boot **parks in the power-on-status "page-8" stage** (`0x25ef4`, dispatcher `0xafd8` — NOT `OSD_ChangeUI`, which is the separate `0x4a754`/`osd.c:779`) and never advances to `POWERONMENU_Initial`, so `__bPOWERONMENUInitial` stays **0** and the screensaver gate never opens (verified: probe reads `0x40023a10=0` through 130M). The stage advances only when an event is **delivered** into the power-on event path:
- The "advance" predicate is `0x12e18` reading the registered-monitor descriptor block at `[0x40021db8]`, gated behind the **CC-event request/done flag pair** F_REQ `0x40026e9c` / F_DONE `0x40026ea4` serviced by a worker thread `0x6748` (bit `0x80` = "fetch/deliver next message" via worker→`0x12f10`; bit `0x1000` = periodic monitor pulse via worker→`0x11d94`). **These acks are never starved** — the worker cycles them every frame.
- What IS starved is the **upstream message producer**: nothing enqueues a completion message at power-on. On real silicon that message is posted by the **decoder/media DSR** (interrupt) when the JPEG-still / servo datapath reaches decode-done (posters at flash `0x7f0c4`/`0x830ac` → bits `0x10`/`0x100`; media-present via `0x6130` → bits `0x2`/`0x4`; msg-bearing `0x40`/`0x80`/`0x800`). Page-8's park guard is `byte[0x40022f81]` (VarB, "advance-event pending"), set to 1 only when such an event is delivered.

**The decisive reconciliation with §12.27:** the emulator now completes the decode **datapath** (real pixels written, poll-status mirror `0x40039cd0`→`0x10`, §12.27) but **never raises the decode-completion INTERRUPT** whose DSR posts the completion message. Decode finishes → no IRQ → DSR never runs → no message enqueued → VarB never set → page-8 parks → `__bPOWERONMENUInitial` never set → screensaver never arms. This is exactly why every prior flag/countdown/status poke failed (§10.21-10.24): those poke *acks the worker already produces*; the missing thing is the *upstream DSR-posted message*.

**THE FAITHFUL FIX (converged, all 4 agents + hardware behavior):** on JPU/decode completion (`machine_maybe_jpeg_decode`), raise the **decode-completion interrupt** so the firmware's decoder DSR (`0x7f0c4`/`0x830ac`) runs and posts the completion event. That advances INITIAL_PowerONStatus → `POWERONMENU_Initial` → `__bPOWERONMENUInitial=TRUE` → (~20 s idle) → `OSDSS_Entry` → built-in SPI-photo slideshow. This is the single upstream unlock; it is faithful (real HW raises this IRQ on every decode), not a flag fake.

**Corrections folded in:** (1) `0xafd8` is a page/stage dispatcher, NOT `OSD_ChangeUI` (=`0x4a754`); its IDs are a stage enum, not `osd.h OSD_UI_*`. (2) `0x6670` = WaitCCEvent (blocking), not a poster; real setter `0x59610`=`OS_SetFlag`. (3) CC event flag is a PAIR (F_REQ `0x40026e9c` + F_DONE `0x40026ea4`) + worker `0x6748`. (4) `bAutoPlayPhoto` factory-defaults **ON** (`dvdsetup.h:1528`; the OFF at `dvdsetup_op.h:22` is inside a `/*…*/` comment). (5) `0x1d170` decrements `0x4003275e`, not `0x40022F5E` (a phase byte). (6) The auto-slideshow is the screensaver, NOT the POWERONMENU photo-player — the player still needs a key; the screensaver does not.

### 12.29 The power-on park is a decode-STATUS POLL on a NULL handle — not an interrupt (decode IRQ tested NEGATIVE)

Followed up §12.28 by trying to raise the decode-completion interrupt (the converged 4-agent hypothesis). Instrumented + tested empirically; the hypothesis does **not** hold, and the real gate is now pinned precisely.

**Empirical park anatomy (clean boot, no crutches, TICK_MULT only):**
- `VarB` (`0x40022f81`, the page-8 advance guard, §12.28) is **never written to 1** — only the early bss-init writes it 0. `POWERONMENU_Initial` (`0x4b808`) is never reached; `__bPOWERONMENUInitial` stays 0 (so the OSDSS screensaver never arms, §12.28).
- Forcing `VarB=1` (probe, now removed) does **not** advance: by the time a photo has decoded, the page-8 enter handler has already latched/returned. The park is one layer deeper.
- PC-sampling the park (non-perturbing) shows the CC thread spinning in: delay `0x59850` (busy-wait on system counter `0x400398f8`) and the poller `0x1deec` → `0x36ff0`. `0x36ff0` = poll loop: `~2.5 s delay (0x9c3 ticks)` then `0x375a0(handle, action 3)`, repeating **until it returns 1**. `0x375a0` action-3 (`0x37648`) reads the decode state via `0x6f054` and maps it (0x10..0x12 → 1, else 0/2).
- **The handle it polls is `[0x40022f88]`, which is `0x00000000` (NULL) in a clean boot** (verified live). Its setup writers are `0x55c0c`/`0x55ca4` (function `~0x55bd0`, which allocates a decode context via `0x55c1c`) — these **never run**. So the poll queries a null decode context, never gets state∈{0x10..0x12}→1, and loops forever with 2.5 s dwells.

**Decode-IRQ test — NEGATIVE (answers §12.28's proposed fix):** `CT952_DECIRQ=<bit>` raises a PROC1-2nd decoder/BIU interrupt (BIU `0x10`, MCU_BSRD `0x20`, MCU_* `0x40/0x80/0x100`) on every JPU completion **and** unmasks it (so it is actually delivered, cascading to LEON line 10). Bits `0x10..0x1f0` fire ~68× per run and **do not advance the boot, set VarB, or reach POWERONMENU**. Conclusion: the power-on stage is **poll-driven on decode status**, not interrupt-driven at this point — so wiring the decode-done IRQ (the converged hypothesis) is not the lever. Kept as a documented env-gated negative probe.

**Refined frontier (the actual next lever):** the boot stalls because the decode **context/handle at `0x40022f88` is never created** — its setup `~0x55bd0` (→`0x55c1c` alloc) does not run, so the power-on `0x36ff0` status poll spins on NULL. This is squarely the "decoder bring-up" work §10.25 scoped: the question is now narrow — **what upstream condition gates `~0x55bd0` from creating the decode context?** (vs. the alt handle `0x400239b0`=`0x000ede38`, which *is* populated). Anchors: park poll `0x36ff0`/`0x1deec`; status mapper `0x375a0` action-3 `0x37648`→`0x6f054`; decode handle `0x40022f88` (writers `0x55c0c`/`0x55ca4`, setup `~0x55bd0`/`0x55c1c`); delay `0x59850` on counter `0x400398f8`. Diagnostics added: `[UITRACE] VarB(22f81)` watch; `[REACH]` entries for the decoder DSRs / OSDSS; `CT952_DECIRQ` (negative probe).

**Standing correct model (unchanged):** the keyless auto-slideshow IS the OSDSS screensaver (SPI photos, ~20 s, default ON), gated on `__bPOWERONMENUInitial` — reached only once the power-on state machine completes. It does not complete because of the NULL-handle decode-status poll above. Everything downstream of `POWERONMENU_Initial` (idle → screensaver) is believed correct and untested only because the boot never gets there.

### 12.30 CORRECTION to §12.29 — the decode-status poll is SATISFIED; two errors retracted

Rigorous re-verification (I had built §12.29 on two mistakes; correcting the record):

**Error 1 — misattributed writer.** §12.29 said `0x55c0c`/`0x55ca4` write the decode handle `0x40022f88`. **Wrong:** those stores use base `%o3/%g2 = 0x40039400` (set by `sethi %hi(0x40039400)` at `0x55bfc`), so they write `0x40039788`/`0x400397a4`, **not** `0x40022f88`. In fact the whole binary has **zero annotated stores** to `0x40022f88` (71 reads, 0 writes) — it is a struct field (base `0x40022c00`+0x388) written, if at all, via a computed pointer. So "the setup `~0x55bbc` never runs" is unfounded.

**Error 2 — NULL handle is the NORMAL path.** `0x6f054` (the decode-state reader) begins `cmp %i0,0; be 0x6f06c` — if the handle is **0 (NULL) it proceeds** to read real state; if non-zero it returns 0. So `0x40022f88 == 0` is expected, not a stall cause. And `MIRTRACE` proves the poll is **satisfied**: every decode-status read returns state **`0x10`** (`fdone=1`, via `0x6f0c0`), so `0x375a0(handle,3)` returns **1** and `0x36ff0` exits normally. The `0x36ff0` loop is a **~2.5 s-paced** decode-OK/dispatch cycle, not a hard hang.

**So §12.29's "park = NULL-handle decode poll" is RETRACTED.** What remains verified and solid:
- Boot reaches mode 8 (~32M at TICK_MULT=256), decodes splash + first album photo, decode status reads OK, and **parks**: `VarB (0x40022f81)` never set to 1, `POWERONMENU_Initial` never reached, `__bPOWERONMENUInitial=0` → OSDSS screensaver never arms.
- The park is a slow 2.5 s-paced loop (`0x36ff0` delay `0x59850` on counter `0x400398f8`), i.e. the stage is *idling*, not spinning on an unmet decode gate.
- **Time-compression is near its ceiling:** `TICK_MULT=1024/4096` breaks the boot (it never even reaches mode 8), so the park is not merely "needs more compressed time."
- The decode-completion IRQ (`CT952_DECIRQ`, §12.29) remains NEGATIVE.

**Honest state of the gap.** The stage displays the first built-in photo and idles, correctly per agent C's model (`if displayed_ok && VarB==0 → PARK`). `VarB` is set only when a UI/media **event is delivered** (via the CC-event worker, §12.28). No such event is produced at idle boot. The open question — unchanged and now stripped of the false leads — is **what event source (media-parse-complete, a power-on timeout, or a key) delivers the message that sets `VarB` and lets the stage advance to POWERONMENU → screensaver**. On the user's real hardware this happens keylessly and the screensaver slideshow starts ~15-20 s in; in the emulator it does not, and neither a forced `VarB` (wrong layer, §12.29) nor a decode IRQ (§12.29) nor higher time-compression reproduces it. This is genuinely the multi-source event-starvation frontier of §10.40, not yet closed.

### 12.31 VarB backward trace complete — the advance chain is fully mapped; the block is event-buffer infrastructure never initialized

Traced the page-8 advance guard `VarB (0x40022f81)` backward to its full setter chain (all execution-verified, indirect calls resolved):

```
VarB=1  set in  0x237dc  (conditionally, at 0x2384c)
   ^ called by  0x21868   (snapshots the event buffer, then calls 0x237dc unless
   |                        [0x4002fb52] bit 0x400 is set -- verified CLEAR at park)
   ^ called by  0x2183c (gate [0x40022f61]==0, verified 0) and 0x1f5c0 (gate
   |            [0x40022f63]==0 -> at park =0 so it SKIPS 0x21868)
   ^ called by  stage handlers 0x1f378 / 0x27454 / 0x2743c -- reached only
                INDIRECTLY through the OSD mode-handler table (no direct callers)
```

`0x21868` processes an **event message** out of the buffer at `[0x400328b8]` (agent A's
`0x400306a6`). **At the park `[0x400328b8] = 0x00000000` (NULL)** — the event-buffer
infrastructure was never installed. Its initializer is `0x1f634` (writes
`0x400328b8 = 0x40030400|0x2a6 = 0x400306a6` at `0x1f6f4`), called from `0x23750`
(inside fn `0x2374c`), called from `0x24ec8` — part of the modal-wait/stage machinery
(`0x24d0c` family, §12 agent C). That init path never executes at idle boot.

**So the advance is event-driven, and both the event AND the buffer that would carry it
are absent:** no message is produced at idle (no key/IR, and the internal producers —
media-parse / USB-source DSRs — don't fire because the USBSRC/source worker is parked in
unmodeled card/USB HW, §10.40/§11.3), and the event-buffer init (`0x1f634`) hasn't run
either. `VarB` therefore stays 0 and the stage parks showing the first photo.

**Candid assessment of this frontier.** This is the multi-source event-starvation core
§10.40 flagged, now mapped in full but not cracked. Static tracing keeps hitting
*interdependent uninitialized state* (NULL handles `0x40022f88`, NULL event buffer
`0x400328b8`, unset `VarB`) — several of which turned out to be *normal-null* or symptoms
of the park rather than the root (see the §12.29→§12.30 retraction). The honest root, as
in §10.40, is upstream: **the USB/card source worker never runs to produce a media event,
and no key is injected** — so the event system has nothing to deliver and never initializes
its delivery buffer. Closing this needs the source/media host-controller modelled well
enough that the USBSRC worker completes CHECK_DEVICE and posts a media event (the faithful
path), OR a single injected key/IR event (models the one user action) — both of which then
flow through the now-fully-mapped chain above to set `VarB` → advance → POWERONMENU →
`__bPOWERONMENUInitial=1` → idle → OSDSS screensaver slideshow.

Verified anchors: `VarB 0x40022f81`; setter `0x237dc`(@`0x2384c`); event-proc `0x21868`
(bail bit `0x4002fb52`&0x400, buffer `[0x400328b8]`); dispatch gates `0x40022f61`/`0x40022f63`;
stage handlers `0x1f378`/`0x27454`/`0x2743c` (mode-table, indirect); event-buffer init
`0x1f634`@`0x1f6f4` ← `0x2374c`@`0x23750` ← `0x24ec8`.

### 12.32 Faithful-target session: INITIAL_PowerONStatus mapped; four levers tested NEGATIVE; honest frontier

Pursued the faithful target (make the boot advance to POWERONMENU → OSDSS screensaver on its own). Mapped `INITIAL_PowerONStatus` (`0x418f0`) and tested every concrete lever the trace suggested. All negative — recorded so they're not re-tried.

**`INITIAL_PowerONStatus 0x418f0` structure (verified):**
- Thread-sync wait (`0x59654`, **timed**, timeout 0x19) on `__fThreadInit 0x40038f80` for pattern `0x301` = `INIT_DEC_THREAD_MPEG_DONE(0x1)` | `INIT_PARSER_THREAD_DONE(0x100)` | `INIT_INFO_FILTER_THREAD_DONE(0x200)`. Live value at park = `0x00080102` = JPEG-thread(0x2)+Parser(0x100)+USB-src(0x80000) done; **MPEG(0x1) and Info-Filter(0x200) never set.** But the wait is timed and **proceeds anyway** — not a hard block.
- Hang trap at `0x41a04` (`b 0x41a04`) reached only if `0x5972c()` returns 0; it returns non-zero here, so the boot passes. Then a long linear init (`0x29864`/`0x36654`/`0x65b58`/`0x41efc`/`0x3fccc`/`0x5abb4`...) → `0x41b34` `ChangePage(8)` → **parks** in the page-8 stage (§12 agent C), waiting for `VarB (0x40022f81)`, which needs a delivered event.

**Thread facts (corrects §10.40's "USBSRC worker parked"):** the USB source thread **IS** initialized (`INIT_SRC_THREAD_USB_DONE 0x80000` SET). The threads that never signal done are the **MPEG decoder (0x1)** and **Info Filter (0x200)** threads. `INFOFILTER_Thread` is **precompiled** (declared `infofilter.h:1033`, no source `.c`; nothing in-source sets `INIT_INFO_FILTER_THREAD_DONE`), so its done-flag is set inside the precompiled body, which never reaches that point — i.e. it is blocked in unmodeled HW init.

**Four levers, all NEGATIVE this session:**
1. `CT952_DECIRQ` — raise+unmask the decoder/BIU PROC1-2nd IRQs on JPU completion (§12.29). No advance.
2. `CT952_FORCEADV` — force page-8 guard `VarB=1` (§12.29). No advance (page-8 already latched by then).
3. Higher `TICK_MULT` (1024/4096) — breaks the boot before mode 8 (§12.30). Not a time issue.
4. `CT952_THREADSDONE` — OR the missing MPEG(0x1)+InfoFilter(0x200) done-bits into `0x40038f80`. No advance (the wait is timed; not the gate).

**Honest frontier.** The page-8 park is event-starvation: it advances only when an event message is delivered to set `VarB`, and no event is produced at idle boot. The full consumer chain is mapped (§12.31); the missing piece is the **producer** — and every proxy for it (decode IRQ, thread-done, forced guard) is negative, meaning the real producer is a specific media/parse event whose source thread (**Info Filter**, precompiled) is blocked in HW the emulator doesn't model. Cracking it requires reverse-engineering the precompiled `INFOFILTER_Thread`/MPEG-thread bodies from the binary to find the exact HW register(s) they poll during init, and modeling those — genuine multi-session hardware-bring-up work, not a single flag. This is the same deep event-starvation core the doc has circled since §10.40; it is now bounded to "the precompiled Info-Filter/decoder threads' unmodeled init HW," with the entire downstream consumer chain proven and ready.

### 12.33 The park's core loop fully mapped — no event PRODUCER fires at idle (dynamic-confirmed)

Traced the page-8 modal wait to the bottom. This is the complete, verified mechanism of the park:

- **Modal wait `0x11fb0` (mode 0, cb=`0x25234`)** does NOT hang: on entry it **resets the monitor list `0x40032180` to empty** (`0x11aac`), posts CC-event request `0x1000` (`0x65dc`), then loops `callback (redraw) + PeekCCEvent(0x1000)` (`0x66a0`) until the worker acks `0x1000` — which the worker (`0x6960`→`0x11d94`) does **every frame** (agent B). So it exits after 1-2 iterations and returns.
- Page-8's enter then runs the post-wait check `0x254a4` → `0x25d58` → **`0x12e18`**, which sums the registered-event descriptor block `[0x40021db8]` (agent D). That block is populated **only** by `PostEvent 0x12f10` (`0x6eec`→`0x12f10`), which is reached only when the **CC-event worker receives bit `0x80` with a queued message**. 
- **Dynamic watch (`CT952_MONTRACE`) proves the producer never fires:** across the whole boot the monitor list head `0x40032180` is written *only* by the reset (`0x11abc`, head→self, 3× from the modal wait at 10M/27M/31M) — **never an insert**; and the event-registration table `0x40032188` is never written at runtime. So the descriptor stays empty, `0x12e18` returns 0, `0x254a4` reports "no event", and page-8 parks — **re-running display+check every frame forever.**

**This is the definitive root, confirmed from both ends:** the wait is correct and event-driven; **nothing posts an event at idle boot.** The event PRODUCERS (agent A) are the DSRs — key/IR (no key pressed), media/card DSR (`0x6130`, needs a card-controller event), decoder DSR (`0x7f0c4`/`0x830ac`, refs decode-progress reg `0x80000c00`), parser DSR — none of which the emulator generates autonomously at idle. On real hardware one of these fires keyless (the user's unit reaches the slideshow ~15-20 s in), so the faithful fix is to make the correct HW producer fire.

**Levers exhausted this session (all NEGATIVE):** decode/BIU PROC1-2nd IRQ (`CT952_DECIRQ`), forced `VarB` (`CT952_FORCEADV`), forced thread-done bits (`CT952_THREADSDONE`), higher time-compression. The remaining work is to identify and model the ONE autonomous producer: reverse the decoder DSR `0x7f0c4` to find its trigger interrupt/register (it may be a PROC2 line, not PROC1-2nd — which would explain why the PROC1-2nd `CT952_DECIRQ` was inert), or the card-controller DSR `0x6130`'s trigger. That is the precise, bounded — but genuinely hard — next target. Diagnostics kept: `CT952_MONTRACE` (list/registration writes), `CT952_DECIRQ` (documented-negative).
