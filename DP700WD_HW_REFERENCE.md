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

### 12.34 Decode-completion handler 0x7efdc pinned as the unfired producer candidate; trigger unresolved (tooling limit)

Drilled into the decode/media DSRs (agent A's `0x7f0c4`/`0x830ac`) to find the unfired producer.

- The handler entry is **`0x7efdc`**; `0x7f0c4` is inside it. Near its top it does `OS_SetFlag(0x40038f80, 0x10)` (`0x7f0bc`: `o1=0x10`) — posts **bit `0x10`** — then reads the decode context `0x4003c400` (state at `0x4003c570`, dispatches `0x21`/`0x80`) and reg `0x80000f30`. So if `0x7efdc` runs at all, it sets bit `0x10`.
- **Bit `0x10` is never set** in `0x40038f80` (live value `0x00080102`), so **`0x7efdc` never runs** at boot. (Caveat: bit `0x100`, which sibling `0x830ac` posts, IS set — but `0x100`=`INIT_PARSER_THREAD_DONE` may be set by the parser thread's own init, so "0x830ac ran" is not proven.)
- `0x7efdc` has **no static reference** anywhere (no `call`, no address-word in any ISR/vector table found for either its flash `0x0007efdc` or DRAM `0x4007efdc` image) — it is registered via a computed pointer / eCos interrupt vector this static pass can't resolve. So its **trigger interrupt is unidentified**.

**Tooling limit hit:** the gdbstub (`--gdb`) is the tool that would give a definitive live stack unwind / breakpoint-on-`PostEvent 0x12f10` to catch the producer, but in this environment the gdb-mode process does not stay alive when backgrounded (one attempt connected and stopped at `0x4001d480` — the DRAM stall loop — confirming the `0x1xxxx` region executes from DRAM, but subsequent launches died before listening). So the decisive "does `PostEvent` ever run, and who calls it" test could not be completed here.

**Net for the session:** the producer is pinned to a decode-completion handler (`0x7efdc`, posts flag bit `0x10`) that never executes, plus the media/card DSR path. Both need their **trigger interrupt** identified and modeled — which requires either a working live tracer (gdbstub in an env where it stays up) or reversing the eCos `cyg_interrupt_create` registration (computed-pointer, precompiled). The complete downstream consumer chain (§12.28–§12.33) is proven and ready; the single remaining unknown is the hardware interrupt that fires `0x7efdc`/the card DSR. Diagnostics retained: `CT952_MONTRACE`, `CT952_DECIRQ` (both documented-negative/observational).

### 12.35 Config-read hypothesis (AM7338 parallel) tested — DISPROVEN; settings load correctly

Prompted by a sibling project (different Coby frame, AM7338/µC-OS) whose persistent-splash bug turned out to be an **app-level boot-complete handoff** that never posts (its `gui_close` has no caller in the boot module; funcnavi is meant to call it and doesn't) — after every timer theory died there too. Their sharpest lead was "maybe a wrong config read makes the app skip the handoff." Tested the exact analog here.

**The settings ARE read correctly.** The retail settings live in the flash **SETD section @ `0x1000`** (ROMLD table entry at flash 0x10; size 0x1000), read via `SPF_ReadData` = `lduba [addr] ASI 0x7` (spflash.c:959), which the emulator's SPARC core routes to a normal bus read (sparc.c:513-529) → the mapped flash image. Verified live: the SETD byte signature `43 00 01 0d 03 01 05 7d 01 03 01 01 22 b8 00` is present in DRAM at **`0x400325a0`** (`__SetupInfo`, a copy at `0x40033318`) after boot — so the config load works. Decoded: magic `0x43`, UIStyle 0, brightness 1, contrast 0x0d, aspect 3, TVSystem 1, videoOut 5, OSDLang 0x7d, password 0x0103, defaultType 1, lastMode 1. The IIC/EEPROM board-config (0x80004204/10/14) is separately stubbed to `0xAA55` by the emulator, but that is a board EEPROM, not the user settings.

**So the config-gating explanation is ruled out** — the boot is not being steered wrong by a bad settings read. This is the same place the sibling project's investigation landed: **not a config issue, but a never-posted app/subsystem boot-complete handoff** (their gui_close ≈ our page-8 advance event). Both frames, different chips/OSes, converge on the identical root: an event/message that a higher layer is supposed to post when it reaches "ready," which never fires in emulation. Our frontier is unchanged: identify the producer of the page-8 advance event (§12.33) — now with config-read positively excluded as the cause.

### 12.36 MAJOR RELOCATION — the park is in POWERONMENU_Initial's display path, NOT page-8

Applying the sibling project's "find the handoff" technique surfaced the real boot structure and relocated the park.

**Boot handoff structure (cc.c:1312-1396, Thread_CTKDVD):**
```
1312  INITIAL_System(...)
1315  INITIAL_PowerONStatus(...)   -- runs the page-8 stage, then RETURNS
1320  POWERONMENU_Initial()        -- the boot->menu handoff
1396  CC_DVD_MainLoop()            -- media scan + OSDSS screensaver
```

**INITIAL_PowerONStatus returns** — page-8 is a transient, not the park. Its enter handler's modal wait `0x24d0c` returns 0 when the monitor list is empty (path `0x24d0c`→`0x24e4c`→ret, NOT a loop), so page-8 falls through to ADVANCE→PARK (return 1), ChangePage(8) returns, and `INITIAL_PowerONStatus` returns at `0x41b58`. So control reaches **`POWERONMENU_Initial` (line 1320)**.

**POWERONMENU_Initial IS running — proven live.** Watching the real `OSD_ChangeUI` UI-stack (`0x400391d8`/index `0x40039509`, `CT952_UISTACK`): OSD-stack writes fire at **pc `0x4b844`/`0x4b964` at icount 13.4M** — OSD-region-setup helpers (`0x4b6xx-0x4b8xx`) called from the POWERONMENU code at `0x61xxx` (`0x61e98`→`0x4b67c`). So the menu bring-up runs, then **blocks before `__bPOWERONMENUInitial=TRUE` (poweronmenu.c:434)** — that flag stays 0 forever. The pcsamp park (`0x36ff0` decode-poll + `0x59850` delay) is the **logo/menu display path** (`OSD_ChangeUI(POWERON_MENU)` line 409 / `UTL_ShowLogo` line 419), executed by POWERONMENU_Initial *before* line 434.

**Address-label correction:** the inherited REACH label `POWERONMENU_Initial = 0x4b808` is WRONG — `0x4b808` reads/writes no `__bPOWERONMENUInitial` and is an OSD array-init helper. `__bPOWERONMENUInitial` (`0x40023a10`) is read at `0x59250`/`0x61bf0`/`0x61ebc` (the real POWERONMENU code lives at `0x61xxx`), and its set (line 434) is a computed-pointer store (no annotated `stb`), which is why static search missed it and UITRACE only ever caught the bss-init 0.

**Reframed frontier:** the boot reaches the menu handoff and stalls in the **menu/logo DISPLAY** (`UTL_ShowLogo`/`OSD_ChangeUI`), before completing POWERONMENU init — analogous to the sibling frame's DE/display investigation. Since the decode-status poll is *satisfied* (§12.30, mirror=0x10), the block is a **different wait in the display path** — most likely a display-engine/panel/VSYNC completion the emulator doesn't drive. That is the new, better-localized target: reverse what `UTL_ShowLogo`/`OSD_ChangeUI(POWERON_MENU)` waits on after 13.4M. Diagnostic added: `CT952_UISTACK`.

### 12.37 Live stack unwind at the park — EEPROM/IIC read nested under the display/media machine

Got a clean 14-frame `%fp`-chain unwind from a 40M snapshot (static, from the CC-thread stack at `sp~0x40036aa8`) — the first reliable live call-stack this session (gdbstub exits on client-detach in this env; the static unwind sidesteps it).

**Call stack (outer→inner):**
```
thread-entry → 0x4001ea40 → 0x4001ea60 (media/display machine, 0x1exxx)
  → 0xade0 → 0xa780 → 0xa6bc → 0xa690 (OSD/boot layer, 0xaxxx)
  → 0x31a8 (fn 0x3170: gated on flags 0x4002f7c7/0x4002fb58) → 0x627c0
  → 0x62a48 → 0x624d8 → 0x62514 (IIC/EEPROM config-read WAIT loop)
  → 0x59848 → 0x4001d4c0 → 0x4001dab0 (OS timer/delay)
```

**The park instant is the IIC/EEPROM config read** (`0x62514`, the `~1000-tick` bit-4 poll on the 0x80004200 EEPROM master — emulator-stubbed to `0xAA55`), called from a flag-gated monitor `0x3170` nested under the `0xaxxx` OSD layer and the `0x1exxx` media/display state machine.

**This conflicts with two earlier reads, and the conflict IS the finding:** pcsamp (58-66M) said the hot loop is `0x36ff0` (decode-poll); §12.36's UISTK write said POWERONMENU OSD helpers ran at 13.4M; this unwind (40M) says the EEPROM read. Reconciliation: the CC thread is in a **complex multi-phase steady state** — it cycles through OSD/menu draw, a periodic EEPROM/RTC monitor (`0x3170`), and the decode-status poll (`0x36ff0`), none of which is a single clean hang. `__bPOWERONMENUInitial` stays 0 because POWERONMENU_Initial never *completes* (it's threaded through this cycle), not because one call blocks forever. So §12.36's "park is one specific display wait" was too clean — it's a cycle, and the unwind caught the EEPROM phase.

**Concrete lever surfaced:** the `0x80004200` IIC/EEPROM master is **stubbed to `0xAA55`** (machine.c:200-213), yet it sits in the live boot stack under the display/OSD layer. On these SoCs that EEPROM commonly holds **panel/board config** — directly relevant to the sibling frame's DE/panel dive. If the boot validates the EEPROM contents (checksum at `0x62a04-0x62a38`) and the `0xAA55` stub fails that check, the display/menu init would keep re-reading/retrying — a faithful-modeling gap. Next: reverse `0x627c0`/`0x3170` — what the EEPROM read returns, whether its result is checksum-validated, and whether a stub-mismatch drives the re-read cycle. Tooling note: static `%fp`-unwind from a snapshot works where the gdbstub doesn't in this env.

### 12.38 CT952_FORCEPOM retired — a read-forcing probe of `__bPOWERONMENUInitial` is self-defeating (CONFOUNDED)

Tested the last "force the flag" lever: present `__bPOWERONMENUInitial` (`0x40023a10`) reads as `1` past 40M (+ freeze `__dwTimeNow 0x40031abc` reads to 0) and check whether the OSDSS screensaver then arms. **NEGATIVE and confounded — the probe breaks the very routine it was meant to unblock:**

- `POWERONMENU_Initial`'s own guard is `poweronmenu.c:384 if(__bPOWERONMENUInitial) return;`. Forcing the *read* to 1 makes that guard trip on every call, so the menu never initializes.
- With the menu init suppressed, the watchdog-pet path stalls and the run dies at **~63M with `watchdog_fired`** (loop exit in `machine_run`), never reaching 80M.
- The acceptance witness stayed dead: `_bOSDSSScreenSaverMode 0x400239c4 == 0` in the 63M snapshot; no new JPEG decode.

**Lesson:** a memory-*read* override cannot validate a gate whose setter reads the same address — it inverts the setter's guard. The only faithful validation is making `POWERONMENU_Initial` run to its line-434 store on its own. Probe removed from `machine.c` (`bus_rd`). Joins `CT952_FORCEADV`/`CT952_THREADSDONE`/`CT952_DECIRQ` in the documented-negative pile.

### 12.39 §12.37 EEPROM lever CLOSED empirically — the IIC master is an unmodeled RTC; real fidelity gap, but BENIGN for the boot

Added an IIC access trace (`CT952_IICTRACE`, logs rd/wr of `0x80004204/0c/10/14`) and captured the full boot. This settles §12.37's "does the EEPROM read gate POWERONMENU" question with data, and corrects two static mis-reads.

**What the firmware actually does (empirical, crutch-free boot):**
- **~4.9M — one-time signature init.** `0x625b0` reads the signature (`0x80004204`) = **`0x0000`** (not `0xAA55` — `0xAA55` is the firmware's *timeout fallback* in `0x625b0`, never the emulator's value). Consumer `0x62c40`: `if sig==0xAA55 →ok; elif sig==0x55AA →ok; else write 0x55AA to init`. Since `0x0000` matches neither, the firmware **writes `0x55AA`** to initialize a blank board device (trace: `[IICwr] 80004214=000055aa`). One-shot.
- **~40.2M — an RTC set.** A write-burst (`0x62a94`…`0x62b18`) pushes time-like values `0x09fd`, `0x0000` via the `0x4214`/`0x4210` command regs — the `0x627c0` **clock updater** (seconds→0x3b, min→0x3b, hr→0x17, month→0xc rollovers) driving an **IIC-connected RTC**. This is the phase §12.37's 40M `%fp`-unwind caught (`0x627c0→0x62a48→0x624d8→0x62514`).
- **steady state — one status read every ~4.1M instructions**, forever (`pc 0x62904`, `0x4210=0x38`, bit2 clear): 46M, 50M, 54M, … 109M. A lightweight periodic RTC-monitor poll, **not a spin, not a block.**

**The fidelity gap is real:** the emulator models the *command* self-clear (`0x4210` bit2) but **not the RTC/EEPROM read-ready bit (`0x420c` bit2)**, so the `0x624d8` read path times out (~1000 ticks, seen once ~32M) and reads return stale `0`/`0x38`. **But it is benign:** the firmware handles blank/failed reads gracefully (writes a default signature, falls back to the software clock), accesses are rare and bounded, and the periodic poll is a single read per monitor cycle. **The IIC/EEPROM does NOT gate `POWERONMENU_Initial` completion.** §12.37's checksum-retry hypothesis is disproven — the `0x627c0` monitor is the RTC clock tick, not a config-validate-retry loop.

**Net.** §12.37's EEPROM lever is closed. Modeling the IIC RTC faithfully (present `0x420c` bit2 + real time bytes) would remove the ~1000-tick timeouts and give live time, but would **not** advance the boot. The reason `__bPOWERONMENUInitial` stays 0 is upstream, unchanged: the CC main loop cycles alive (the ~4.1M-period RTC poll proves it) but the menu-handoff event that would complete `POWERONMENU_Initial` is never posted (§12.33 event-starvation core). Diagnostic retained: `CT952_IICTRACE` (observational).

### 12.40 Mode-8 latch RE-CONFIRMED as the mode-7 gate (empirical) — but forcing it is a crutch, and it does NOT arm the screensaver

Put the §12.14/12.15 mode-8-latch mechanism (which §12.26 had walked away from) to an **empirical** test instead of an argument. Added `CT952_MODE8DECLINE` (machine.c bus_rd): force reads of the configured-source count `0x40032b3b` to 0 so the mode-8 predicate `0x272a8` declines and `0x418f0`'s fallback `OSD_ChangeUI(POWERON_MENU/mode 7)` runs. This forces an UPSTREAM condition and lets the real mode-7 handler set the flag — NOT the confounded FORCEPOM (§12.38), which forced the flag's own read.

**What the crutch proved (the mode-8 latch IS the mode-7/flag gate):**
```
[OSDUI] ChangeUI(mode=8) activeUI=0        enters   caller=0x41b34  icount=27M
[OSDUI] ChangeUI(mode=7) activeUI=0        enters   caller=0x2604c  icount=34M   <- mode 7 ENTERS (never seen crutch-free)
[OSDUI] ChangeUI(mode=7) activeUI=40024da0 DECLINED caller=0x41b50  icount=68M   <- mode 7 now LATCHED
DUMPFLAGS @100M: __bPOWERONMENUInitial=0x01  __dwOSDSSCheckTime=0x396e (was 0xFFFFFFFF)  activeUI=0x40024da0
```
So yielding mode 8 → mode 7 enters → **`__bPOWERONMENUInitial=1`** (first time crutch-free-downstream) → **`OSDSS_Monitor` runs** (`__dwOSDSSCheckTime` is now a live stamp, no longer the "never called" `-1` sentinel §12.28 relied on). This settles the §12.26-vs-§12.14 oscillation: the mode-8 latch **is** what gates the mode-7/POWERONMENU/screensaver path.

**But the crutch is a dead end — three reasons, all honest:**
1. **It is a logic crutch**, not hardware modeling (forces `0x40032b3b` reads to 0) — exactly what §11 says to delete.
2. **It does NOT arm the screensaver.** Restored the 100M snapshot and ran +200M at `TICK_MULT=4096`: `__dwOSDSSCheckTime` stayed **frozen at 0x396e** and `_bOSDSSScreenSaverMode` stayed **0**. So `OSDSS_Monitor` stamped the idle baseline **once and never ran again** — the CC main loop stops cycling after entering mode 7. Mode-8 is *one* blocker, not *the* blocker; the CC-loop event-starvation (§10.50/§12.33) is still underneath.
3. **The way it leaves mode 8 is fake.** The volume list `[01 02 03 04 ff]` at `0x40032ab8` is loaded by the config-loader `0x33ca4` from flash setting `0xa3` (callers `0x299b8/cc/e0`) — genuine retail config, read faithfully (§12.35). The predicate `0x272a8` is *purely* the count (`count==0`→decline `0x273a8`; `count!=0`→stay via `0x27344`/`0x2737c`, no present-media check). Forcing the count read to 0 is not how the real device leaves mode 8.

**The slideshow IS the OSDSS screensaver (user-validated, §12.28) — so the real device DOES leave mode 8 and reach mode 7 and set `__bPOWERONMENUInitial`.** This crutch reaches mode 7 by the wrong means, but its endpoint (mode 7 → screensaver gate) is the correct target. The open faithful question for *how* the real device leaves mode 8 (source count that reflects present media, or an advance event) remains — but this experiment exposed a **more fundamental blocker downstream of it**:

**Even at mode 7 with `__bPOWERONMENUInitial=1`, the screensaver never arms because `OSDSS_Monitor` runs exactly ONCE and stops** (`__dwOSDSSCheckTime` frozen at `0x396e` across 100M→300M). `OSDSS_Monitor` is called from the CC main loop (`CC_DVD_MainLoop`, cc.c:1396); it running once means **the CC loop stops cycling after one iteration** — precisely the §10.50 finding that the CC thread blocks in `Cyg_Thread::sleep` on mailbox `0x40033830` waiting for a message that never arrives. On real HW a periodic producer posts that wake every frame, so the loop keeps cycling, `OSDSS_Monitor` runs each pass, the idle timer accumulates, and at ~20s the screensaver arms. In the emulator nothing posts it, so the loop runs once and sleeps.

**Net.** Mode-8-decline is retired as a fix (kept as a documented diagnostic). It re-confirmed the latch mechanism (§12.14/12.15, disputed by §12.26) AND, more valuably, isolated the true remaining blocker past the mode-8 gate: **the CC event loop does not keep cycling** — it needs the periodic mailbox-`0x40033830` wake message that no producer posts (§10.50/§10.51-proven-revivable/§12.33). Identifying and faithfully modeling that periodic producer (timer/VSYNC/frame tick → `0xad4cc` post) is the single remaining unlock for the screensaver-slideshow. Diagnostic retained: `CT952_MODE8DECLINE` (documented dead-end).

### 12.41 VSYNC-delivery lead investigated and CLOSED — the firmware deliberately masks VSYNC (faithful); the CC-loop wake is elsewhere

Chased the §12.40 "CC loop runs OSDSS_Monitor once then stops" blocker along the per-frame-interrupt angle: for an idle-timer screensaver the UI loop must re-run every frame, and the natural per-frame wake on a display SoC is VSYNC. Added an IRQ-state `DUMPFLAGS` dump (`machine_io_get`) and `CT952_VSMTRACE`.

**Interrupt state at the mode-7 park (m8b.snap, 300M):** `LEON_MASK=0x2d00` (TIMER1 bit8, PROC1-2nd bit10, PROC1-1st-cascade bit13 all enabled); **`P1_1ST_MASK=0xfffffffe`** — every PROC1-1st source enabled *except* bit0; `P1_1ST_PEND=1` — **VSYNC is firing but masked**, so `(pend&mask)=0` and it never cascades to LEON 13. The per-frame VSYNC ISR is dead at the park.

**But that mask is the firmware's own doing (empirical, `CT952_VSMTRACE`):**
```
[VSM] MASK<-ffffffff  vsync ENABLED   pc=0x3fac8  icount=4.9M   (display init enables it)
[VSM] MDIS clear 1    vsync CLEARED   pc=0xa4218  icount=9.8M   (firmware disables it)
[VSM] MDIS clear 1    (already off)   pc=0xa4218  icount=11.3M
```
`0xa4214` writes 1→`P1_1ST_MDIS` inside a PSR interrupt-critical section (a **display-stop** routine `0xa41f0` that clears display fields + sets `0x4002401c`). So VSYNC-off at the park is **faithful firmware behavior** — the firmware masks VSYNC when it stops/reconfigures the display and never re-enables it because the boot never reaches a "display running" state. **VSYNC is ruled OUT as the missing CC-loop wake** (it's correctly off; re-enabling it would itself require the boot to advance — circular). Red herring, closed with data, not assumption.

**Consolidated honest frontier (post-session).** The keyless slideshow IS the OSDSS screensaver (user-validated, §12.28). Reaching it needs two things, and both reduce to the SAME missing piece:
1. **Leave mode 8 → mode 7 → `__bPOWERONMENUInitial=1`.** Gated by the mode-8 latch (§12.14/12.15), which faithfully clears only when an advance event is delivered (§12.31). (Forcing it — `CT952_MODE8DECLINE` — is a crutch, §12.40.)
2. **Keep the CC loop cycling** so `OSDSS_Monitor` runs each frame and the idle timer accumulates. Gated by the CC-thread mbox-get block on mailbox `0x40033830` (§10.50) — confirmed still present at the mode-7 state (`[+3c]=0`, thread on waitlist). `OSDSS_Monitor` ran exactly once (`__dwOSDSSCheckTime` frozen 100M→300M).

Both are the **same event-starvation core** (§12.33): a periodic message/event producer that a live system posts and the emulator does not. Ruled out this session and prior: VSYNC (faithfully masked, §12.41), decode-completion IRQ (§12.29 negative), IIC/RTC (benign, §12.39), config-read (§12.35), forced flag (§12.38 confounded). The producer posts to mbox `0x40033830` via `0xad4cc`←`0x6430/0x6798/0x75d0`, all indirectly dispatched with **no static caller or memory pointer** (same wall as `0x7efdc` §12.34 and the mode-table handlers). The one tool that could name it live — a breakpoint on `PostEvent 0x12f10` / the posters — needs a gdbstub that survives backgrounding; the current stub's Ctrl-C interrupt is unreliable in the slow park (§10.49). Making that trace reliable, or reversing the eCos interrupt-vector registration that installs the producer DSR, is the next concrete step. New diagnostics: `machine_io_get`+IRQ `DUMPFLAGS`, `CT952_VSMTRACE`.

### 12.42 PROVEN by full-speed PC trace — the CC mailbox is posted exactly ONCE at boot; the bit-0x80 event dispatch never runs

Added `CT952_PCHIT` (sparc.c `sparc_run`): a full-speed execution-PC watch (a few integer compares per instruction, no single-stepping) — reliable where the gdbstub's Ctrl-C interrupt is not (§10.49). Watched the CC-mailbox posters, `PostEvent`, the bit-0x80 dispatcher, and the mode-7 flag handler over a full 120M crutch-free boot. This converts §12.33's event-starvation from inference to **hard fact**:

```
0x6798 (CC-mbox post -> 0xad4cc)   fires EXACTLY ONCE  @ icount 25.97M  (from worker 0x6748, under 0x4001ea60)
0x12f10 (PostEvent)                NEVER fires (0 hits / 120M)
0x6eec  (EvtDispatch_bit80)        NEVER fires
0x61cf8 (mode-7 flag setter)       NEVER fires
0x6430, 0x75d0 (other posters)     NEVER fire
```

**The two channels, disentangled:**
- **CC mailbox `0x40033830`** (mbox-put via `0xad4cc`) — the CC thread's command queue. Posted **once** at 25.97M by worker `0x6748` (a message `(3, 0x11)`; `0x6748` also sets thread-init flag `0x80000` and touches the F_REQ/F_DONE pair). The CC thread wakes once, processes it, then sleeps in mbox-get forever (§10.50 confirmed).
- **Event flag pair F_REQ `0x40026e9c` / F_DONE `0x40026ea4`** — the bit-0x80 monitor/event dispatch. Handler `0x6eec` clears F_REQ bit 0x80, sets F_DONE bit 0x80 (ack), calls `PostEvent 0x12f10` (populates the registered-monitor descriptor → page-8 advance / `OSDSS_Monitor` dispatch), drains a message queue at `[base+0x220]`. `0x6eec` runs only when a **producer sets F_REQ bit 0x80** — which **never happens** in the whole boot. So the OSDSS/page-8 event dispatch is dead at the producer.

**Dispatch structure (why it's unresolvable statically):** `0x6748` (worker) and `0x6eec` (dispatcher) both have **no direct caller and no memory pointer** — eCos thread bodies / indirectly-registered handlers, same wall as `0x7efdc` (§12.34) and the mode-table handlers. `PostEvent 0x12f10` is called only from `0x6eec`.

**Net.** The boot is definitively event-starved after 25.97M: one CC-mailbox post, then nothing sets F_REQ bit 0x80, so `0x6eec`→`PostEvent`→(page-8 advance / OSDSS dispatch) never runs and mode-7 (`0x61cf8`) never enters. The faithful unlock is the producer that should set F_REQ bit 0x80 (and/or re-post the CC mailbox) periodically — an interrupt/timer-driven eCos handler whose trigger the emulator doesn't deliver. Next lever: determine whether worker `0x6748` is a thread meant to LOOP (and blocks after one post) vs. a one-shot, and what HW event feeds the bit-0x80 producer. New diagnostic: `CT952_PCHIT` (sparc.c).

### 12.43 Flag-bit-level proof — F_REQ bit 0x80 is NEVER set; the bit-0x80 setters are all inside the event worker, gated on messages that never arrive

Followed §12.42 with a full-speed `bus_wr` watch (`CT952_FREQTRACE`) on the CC-event flag pair F_REQ `0x40026e9c` / F_DONE `0x40026ea4` over a 120M crutch-free boot, then mapped every static F_REQ setter.

**Dynamic (proven):** across 120M, the *only* bits ever newly-set in the pair are **F_DONE `0x04000000`** (bit 26, 23×) and **F_DONE `0x1000`** (2×). **F_REQ bit `0x80` is NEVER set.** All flag ops go through the eCos primitives at flash `0xe0328/0xe049c/0xe0380`. So the message-delivery request that would drive `0x6eec`→`PostEvent 0x12f10`→(page-8 advance / `OSDSS_Monitor` dispatch) is never produced — confirmed at the bit level, not inferred.

**Static (the setter map):** every `OS_SetFlag(0x59610)` / `ClearFlag(0x59628)` / `WaitFlag(0x597d0)` site that targets F_REQ lives in the **CC-event worker module `0x6618`–`0x75f8`** (the `0x6748` worker's own code). The bit-`0x80`-bearing setters are `SetFlag@007554` (mask `0x1fdf` ⊃ `0x80`) and several `o1=(reg)` computed-mask sites; the worker waits at `WaitFlag@0067d4` (mask `0x1fff`). These setters run only when the worker dispatches a specific incoming **message** — and after the lone CC-mailbox post at 25.97M (§12.42) no message arrives, so they never execute.

**Net — the root cause, fully proven and bottomed-out.** The autonomous screensaver/slideshow is dead because the CC-event worker never receives the message that would make it set F_REQ bit `0x80`. Both the mode-8→mode-7 advance and the `OSDSS_Monitor` dispatch hang off that one bit. The producer of that message is an external hardware-event DSR (decode/media/monitor-tick) that the emulator does not generate or deliver — the same event-starvation core the doc has converged on from six independent angles (§10.40, §12.31, §12.33, §12.34, §12.42, §12.43), now proven at the flag-bit level. Every alternative has been ruled out with data: forced flag (§12.38), IIC/RTC (§12.39), mode-8 decline crutch (§12.40), VSYNC (§12.41), decode IRQ (§12.29), config-read (§12.35).

**The genuine next phase (scoped honestly).** Naming the exact missing HW event requires work of a different kind than the probes done so far: either (a) reverse the eCos `cyg_interrupt_create`/VSR registration to map which interrupt vector installs the producer DSR (computed pointers, precompiled kernel — hard), or (b) make a live tracer reliable enough to breakpoint the worker's message-receive and read the sender under the live scheduler (the gdbstub's Ctrl-C is unreliable in the slow park, §10.49). New diagnostic: `CT952_FREQTRACE`. F_REQ setter map recorded above.

### 12.44 TOOLING (phase b) — the gdbstub is now reliable; first live look confirms a fully event-starved idle

Hardened the gdbstub into a usable live tracer (the tool §12.34/12.43 said was the gate). Four fixes (committed):
- **Client `TCP_NODELAY` (rsp.py):** Nagle + delayed-ACK added ~40 ms per RSP round-trip → single-step went **22 → 11 258 steps/s (500×)**. This was the dominant unusability, not emulation speed.
- **`GDB_BATCH` 2 000 000 → 20 000** (tunable `CT952_GDB_BATCH`): in the IO-heavy park (~10 K instr/s single-step) a 2 M batch left a Ctrl-C unseen for ~200 s — the hang. 20 K → ~2 s worst-case interrupt latency.
- **`check_interrupt` drains all pending bytes**; **`cont_to` rewritten** to use socket timeouts (not `select`, which fired on the `+` ack and then blocked on the not-yet-arrived stop packet) with bounded 0x03 retries.
- Verified: continue+interrupt returns in exactly the timeout, breakpoints hit in 0.00 s (`bp 0xd9474`), stub re-usable across cycles.

**First live look at the crutch-free-mode-7 park (m8b.snap):** 150 interrupt-samples land on **one thread only** (`sp~0x40026000`), cycling uniformly through eCos scheduler internals (`0x4001d3xx–daxx`) + HAL interrupt-restore (`0x13d8c`) + a scheduler wrapper (`0xd947c`). **Every firmware thread is blocked; only the eCos scheduler/idle spins.** This is live confirmation of the §12.42/12.43 event-starvation: no producer fires, so no thread is ever made runnable, and the kernel idles. Phase (a) next: read each blocked thread's saved context live to enumerate what each waits on, and breakpoint the eCos interrupt VSR/clock DSR to see which HW interrupts actually fire in the park vs. which the producer needs. New knobs: `CT952_GDB_BATCH`; reliable `rsp.py`.

### 12.45 Phase (a) — live tracing resolves the fork: timer is ALIVE, the gap is a missing HW-event message

Used the hardened live tracer (§12.44) to settle whether the screensaver stall is a dead timer/alarm subsystem or a missing event message. Decisive, all live on the crutch-free mode-7 park (m8b.snap):

- **Clock ISR fires.** Breakpoint on the TIMER1 trap vector `0x40000180` (`tbr=0x40000050`; IRQ8→TT0x18→vector `0x40000180`, an eCos interrupt stub `rd %psr; mov 0x20,%l5; b VSR`) is hit repeatedly. The eCos clock interrupt is delivered.
- **`OS_GetSysTimer` (`0x59838`) tick is NOT frozen.** It reads via pointer `[0x400398f8]=0x4002e320`; the tick at `0x4002e354` advances live (it is not stuck). `OSDSS_Monitor 0x591b4` arms when `OS_GetSysTimer()-__dwOSDSSCheckTime(0x396e) > OSDSS_ENTER_TIME`, and the threshold `0xe260`=58464 is exactly the **default ~20 s** enter time (`COUNT_10_SEC*2`, §12.28) expressed in ticks → real tick rate ≈ 2923/s, consistent with the ~1–few-kHz system tick (§12.23). (NB: the raw advance rate seen under the gdbstub is a wall-clock artifact of emulation speed, NOT the device tick rate — do not derive a real-time from it.) The point stands: the tick advances, so the screensaver WOULD arm ~20 s after the last activity **if `OSDSS_Monitor` re-ran** — but it does not re-run.
- **The clock drives no firmware producer.** Single-stepping 400 instrs from the clock-ISR entry stays entirely in eCos kernel (`0x4001xxxx` ×315) + trap vectors + the two idle wrappers (`0xd9474`, `0x13d8c`) — **no firmware alarm callback runs, no thread is woken.**
- **The CC thread never wakes.** Breakpoint on mbox-get `0x5969c`: 0 re-entries in 15 s of live run. Its sleep is untimed (§10.50: enqueued on the mbox wait-list, no timeout), so it waits for a real *message*, and none arrives.
- **Only the eCos scheduler spins** (150 samples, one thread `sp~0x40026000`); every firmware thread is blocked.

**Conclusion (fork resolved).** The timer/clock subsystem is fully alive — this is NOT a frozen-tick or dead-alarm problem. `OSDSS_Monitor` runs exactly once because the CC main loop (`CC_DVD_MainLoop`, which calls it, cc.c:1396) is blocked in an untimed mbox-get on `0x40033830`, and nothing posts the message that would cycle it. The producer is a hardware-event DSR (media/decode/monitor-tick class, §12.42/12.43) that never fires in emulation — the correctly-scoped (a) frontier. Next: enumerate each blocked thread's wait object live, and identify which modeled-but-undelivered (or unmodeled) HW interrupt should drive the producer that posts to `0x40033830` / sets F_REQ bit 0x80. Tooling: reliable gdbstub (§12.44).

### 12.46 Phase (a) cont. — CC_DVD_MainLoop is a POLL loop that shouldn't block; a poll-call is stuck in mbox-get (multi-thread deadlock)

Live-traced (hardened stub) plus the authoritative source (`cc.c`):

**`CC_DVD_MainLoop` (cc.c:836) is a `while(1)` busy-poll loop** — `MONITOR_CheckWatchDog`, `DBG_Polling`, `DSR_IR` (poll mode), `CC_MainProcessKey`, timed `OS_GetSysTimer` housekeeping (volume/keyscan), then a series of non-blocking `*_Trigger` polls (`PANEL/TFT/AUTOPWR/ALARM/GAMEMAIN`), `MEDIA_Management`, `SETUP_Trigger`, `OSDPROMPT_Trigger`, and finally **`OSDSS_Monitor()` at line 1004**. By design it never blocks; each iteration must reach line 1004 to advance the screensaver idle. `OSDSS_Monitor` ran exactly once (`__dwOSDSSCheckTime` stamped) → the loop completed one iteration, then on the next iteration **blocked inside one of its poll-calls in an mbox-get** — a call that is supposed to poll is instead sleeping. That is the concrete faithful bug: a poll waiting on a subsystem that never answers.

**Multiple threads are deadlocked, each on its own input mbox (blocked-thread stacks read from m8b.snap):**
- **CC thread** (thread struct `0x40035928`): sleeping in mbox-get on `0x40033830` (§10.50), reached from inside the `CC_DVD_MainLoop` poll body.
- **Parser/InfoFilter thread `0x83098`**: on entry sets `__fThreadInit |= 0x100` (INIT_PARSER_THREAD_DONE, via `OS_SetFlag 0x59610` at `0x830ac`), then loops and sleeps in mbox-get on `0x4003cc00` (`call 0x5969c` at `0x841f4`, gated `if [fp-68]==0`) — waiting for parse jobs that never come.
- Only the eCos scheduler runs (§12.44); every firmware thread is parked on an input queue.

So the stall is a **producer/consumer deadlock across the media pipeline**: each worker sleeps waiting for a message from the previous stage, and the pipeline is never primed because the source-stage HW event (media/decode) never fires (§12.42/12.43). `OSDSS_Monitor` is collateral — it can't re-run because its host loop (`CC_DVD_MainLoop`) is stuck in a sibling poll-call's mbox-get.

**Clean next step (avoids fragile stack-scraping):** boot fresh to mode-7 under the live stub, breakpoint mbox-get `0x5969c`, and catch the CC thread the FIRST time it enters the blocking get — read `%i7`/`%o7` live to name the exact `CC_DVD_MainLoop` poll-call that blocks (candidate: `MEDIA_Management`), then trace what that call waits for. That names the precise unmodeled HW response. Caveat recorded: manual stack-walks here hit stale/`0x5a5a5a5a`-fill words — trust `%fp`-chain / live regs, not value-shaped scans.

### 12.47 MPEG decoder STRUCK from the boot-stall suspect list — the MPEG decoder thread is never created in this build (bit 0x1 cannot be set faithfully)

Targeted the open `__fThreadInit` (`0x40038f80`) bit `0x1` = `INIT_DEC_THREAD_MPEG_DONE`,
which is never set (live value `0x00080102` = JPEG `0x2` + Parser `0x100` + USB-src
`0x80000`). Question: what MPEG/video-decoder HW does the MPEG-init thread poll that the
emulator fails to satisfy? **Answer: none — there is no MPEG-init thread in this build.**

**Static chain (all in `dp700wd.bin`; `dp700wd_ring.bin` differs by exactly ONE byte at
`0x1151a`, the narration patch, so the disassembly is authoritative):**

1. `INITIAL_PowerONStatus` (`0x418f0`, confirmed §12.32) calls `INITIAL_ThreadInit(id)`
   for ATAPI(1), USBSRC(9), INFO_FILTER(8), **MPEG_DECODER(3)** (`0x4190c`: `mov 3,%o0`),
   PARSER(2) — matching `initial.c:640-647`.
2. `INITIAL_ThreadInit` is at flash **`0x41b60`** (NOT the SDK symbol). Its compiled
   dispatch handles **only** id `5`→`0x41c34` (JPEG, entry `0x7007c`=`JPEG_ThreadMain`),
   id `2`→`0x41bf0` (PARSER, entry `0x83098`=`PARSER_ThreadMain`), id `9`→`0x41ba0`
   (USBSRC), id `0xb`→`0x41cc4`. **Every other id — including `3` (MPEG) and `4` (DivX)
   and `8` (INFO_FILTER) — falls through the `cmp` chain to `b,a 0x41d50` (a bare
   `ret; restore`) and creates NOTHING.** (`0x41b88`: `b,a 0x41d50` for id<5≠2;
   `0x41b9c`: same for id>5 ∉{9,0xb}.)
3. Root cause in source: `initial.c:32` `#define SIMP_INITIAL`, and the retail DMP
   photo-frame build compiles OUT the MPEG/DivX (and, via `#ifndef SIMP_INITIAL`, the
   INFO_FILTER) decoder-thread cases. Confirmed by the binary: **`decoder.a`'s
   `MPEG2_ThreadMain` / `DivX_ThreadMain` bodies are NOT linked** — their byte signatures
   (`sethi %hi(0xb0000000),%i5` MCU-descriptor table fill; `stb 0x10,[0xb0000190]`) are
   absent from the ROM, while the same archive's `JPEG_ThreadMain` (`0x7007c`, `save
   %sp,-776`) and `PARSER_ThreadMain` (`0x83098`) ARE present. `MPEG2_ThreadInit` /
   `MPEG2_ThreadExit` are `retl;nop` stubs (`0x6f8f4`).
4. Exhaustive scan of all 52 `OS_SetFlag` (`0x59610`→`cyg_flag_setbits 0x4001dffc`)
   callsites: the ONLY writers of `__fThreadInit` are `0x2`@`0x7023c` (JPEG body),
   `0x10`@`0x7f0c4` (decode-completion DSR, §12.34), `0x100`@`0x830ac` (Parser body).
   **There is no `OS_SetFlag(0x40038f80, 0x1)` anywhere in the image.** In the SDK build
   bit `0x1` would be posted from inside `MPEG2_ThreadMain` (mirroring JPEG's `0x7023c`);
   that body is absent here, so no code path can set it.

**Consequence.** `INIT_DEC_THREAD_MPEG_DONE` (bit `0x1`) is never set **by design**, not
because of any unmodeled MPEG/video-decoder register, interrupt, or PROC2 handshake. The
MPEG decoder engine is simply not instantiated at boot in the photo-frame build (the photo
path is the hardware JPU/VLD/MCU-BIU still-decode on a PROC1 worker, §12.11-12.12; PROC2
`mpg.bin` video is never loaded at boot). The `INITIAL_PowerONStatus` thread-sync wait that
includes bit `0x1` (pattern `0x301`) is a **timed** `OS_TimedWaitFlag` (`0x59654`,
`COUNT_50_MSEC`) that proceeds on timeout regardless (§12.32); forcing the bits
(`CT952_THREADSDONE`, §12.32) was already "no advance." So bit `0x1` is neither a boot gate
nor satisfiable by hardware modeling.

**What was modeled / changed.** Nothing faked. Forcing bit `0x1` would be a crutch (the
already-inert `CT952_THREADSDONE`) and there is no HW handshake to model — the decoder HW
the MPEG-init thread would poll is never programmed because that thread never runs. Added
`__fThreadInit` (`0x40038f80`) to the `CT952_DUMPFLAGS` table (`main.c`) as honest
instrumentation so the bit state is measurable at any `--run-to` checkpoint, with an inline
note pointing here. Measured after this pass (raw crutch-free boot, `--run-to 90000000`):
`__fThreadInit = 0x00080102`, **bit `0x1` = 0 (unchanged, as expected)**.

**VERDICT — MPEG/video decoder engine STRUCK from the boot-stall suspect list.** It cannot
be the blocker: the MPEG decoder thread is not created, so nothing waits on MPEG-decoder
hardware at boot, and bit `0x1` is architecturally unsettable in this build. This is the
honest outcome (b) of the investigation. The real, unchanged boot frontier is the
event-starvation producer deadlock (§12.42-12.46), which is unrelated to MPEG decode.

### 12.48 SOFT JPEG DECODER RULED OUT + decode-completion "DSR" 0x7efdc reframed as a non-boot codec thread; both decode-IRQ paths now measured NEGATIVE

Pursued the standing hypothesis (§12.28/§12.34) that a *decode-completion interrupt* whose DSR
is flash `0x7efdc` (sets `__fThreadInit 0x40038f80` bit `0x10`) is the missing boot producer.
Two independent results, both decisive.

> **CORRECTION (§12.51, binary-proven): (A) below is WRONG. `SUPPORT_JPEGDEC_ON_PROC2`
> IS defined in this build.** The shipped image is the CT909R-family / DMP952A config
> (root `platform.h`'s `CT909P_IC_SYSTEM` is stale — it does not reflect the linked
> binary). Proof from the running image, not the header: the `#ifdef
> SUPPORT_JPEGDEC_ON_PROC2` AIU-GR2..16 reset loop (`chips.c:1374`/`utl.c:5194`)
> **executes** (`--iolog`: GR2..GR16 `0x80000788`–`0x800007c0` each written 0); the
> firmware **stages PROC2 for the JPEG code** (`PROC2_START` GR22 `0x800007d8` =
> `0x40002000`, `PROC2_SP` GR21 `0x800007d4` = `0x4001cf00`, boot-cmd GR25
> `0x800007e4` = `0x10003` — the exact `hdecoder.c:714`/`724-737` handshake); and
> `jpegdec.bin` in the build tree **is** the real PROC2 SPARC image (trap table at
> `0x40002000`, reset→`0x40003000`, boot handshake spins on GR25 low-bits==`0x10003`
> then clears GR25). So a PROC2 software JPEG decoder **does exist**. **BUT §12.48's
> operational conclusion still holds** — measured in §12.51, PROC2 (`cpu2`) executes
> **zero** instructions during the boot: it is staged-and-held once and **never
> released** (no `REG_PLAT_RESET_CONTROL_DISABLE` bit0 = `0x80000304`=1, no DSU2
> release, in 90M), so the boot photos still decode on the **PROC1** JPEG worker
> (`0x7007c`) + HW-JPU path (poll-satisfied, §10.8/§12.48B). The PROC2 JPEG offload is
> real but is only activated by a runtime slideshow decode that is downstream of the
> event-loop deadlock (§12.49/§12.50), so it is **not** the missing CC-event producer.
> The paragraph below reasoned from the stale header and reached the right operational
> answer for the wrong reason — read it with that correction.

**(A) There is NO software (PROC2-code) JPEG decoder in this build — preprocessor-proven.**
`SUPPORT_JPEGDEC_ON_PROC2` is defined **only** inside `#ifdef CT909R_IC_SYSTEM`
(`Winav.h:1075-1077`); this image is `CT909P_IC_SYSTEM` (`platform.h:26-27`, CT909R commented
out), so the macro is **undefined**. Confirmed downstream: the PROC1→PROC2 JPEG handoff via
`REG_AIU_GR(2..17)` in `HAL_ReloadAudioDecoder` (`chips.c:1372-1381`) is `#ifdef
SUPPORT_JPEGDEC_ON_PROC2` and compiles **out**; `CT909R_JPEG_AND_MP3` is in the `#else` of the
909P branch (`Winav.h:1630-1633`), also **off**. The `chips.c:1313` "LOGO (JPEG code) → play
MP3+JPEG" comment describes the *CT909R* DMP flow, not this build. So the logo/photo JPEG decode
is the **hardware** JPG/VLD/MCU-BIU block driven by a PROC1 thread — exactly §10.8's conclusion.
This **retires the task premise** (that `Winav.h:1076` enables PROC2 JPEG for this image; the
`#ifdef` guard was misread). The emulator's `machine_maybe_jpeg_decode` (host picojpeg in the
`JPU_GO` handler) correctly models that HW block and correctly does **not** run PROC2 code for
JPEG; PROC2 (`cpu2`, `proc2_on`) is the audio-DSP stand-in only.

**(B) `0x7efdc` is NOT a "decode-completion DSR" — it is a codec decoder THREAD the boot never
creates (static-proven).** The address `0x7efdc` has **zero** `call` sites and **zero** pointer
words in flash (confirmed), but it *is* materialized once — at `0x41d04/0x41d10` (`sethi
%hi(0x7ec00); or ,0x3dc`) — inside `CreateThreadByType(type)` (`0x41b60`), a `%i0`-indexed
thread factory. It builds a thread descriptor {entry=`0x7efdc`, prio 3, stack `0x40037378`,
size `0x1c00`} and calls the create/resume wrappers `0x596f4`/`0x596e0`. So **`0x7efdc` is a
thread body**, structurally identical to the JPEG decoder thread `0x7007c` (same prologue: read
channel byte `0x40039d2c`, build per-channel callback tables, then `OS_SetFlag(0x40038f80,
<initbit>)`). Its `OS_SetFlag(0x40038f80, 0x10)` at `0x7f0c4` is its **thread-init-done flag**,
not a completion post.

The factory's type→thread→init-bit map (verified):

| CreateThreadByType | entry | init bit | role |
|--------------------|-------|----------|------|
| 2 | `0x83098` | `0x100` | Parser/InfoFilter (§12.46) |
| 9 | `0x6748` | `0x80000`(+) | CC-event worker (§12.42) |
| 5 | `0x7007c` | `0x2` | **JPEG decoder** (`INIT_DEC_THREAD_JPEG_DONE`) |
| 11 | `0x7efdc` | `0x10` | a **non-JPEG codec** decoder |

`CreateThreadByType(11)` (→`0x7efdc`) is reached from **one** site, `0x5a7a4`, inside
`HALDEC_CreateThread(decoderType)` (`~0x5a660`): case `type==4` creates it and waits on bit
`0x10`; `type==0`→JPEG(`0x7007c`,bit`0x2`), `type==1`→MPEG(bit`0x1`), `type==2`→DivX(bit`0x4`).
The decoder type is chosen from the media codec (`0x400399f0` vs `0x96/0x97/0x98`, live `=0`).
**The keyless photo boot only ever needs the JPEG decoder (type 0 → `0x7007c`), so
`HALDEC_CreateThread(4)` is never called, `0x7efdc` is never created, and bit `0x10` never
sets** — same category of finding as §12.47 (MPEG thread never created). Bit `0x10` isn't even
in the power-on thread-sync mask `0x301` (§12.32); its only waiter is its own creator at
`0x5a790-0x5a7d0`. **Forcing bit `0x10` / running `0x7efdc` would be a crutch** (spawning a codec
thread the firmware deliberately doesn't). §12.28's "raise the decode-completion IRQ so
`0x7f0c4` posts the completion message" is therefore built on a **misidentification** and is
struck.

**Live confirmation (osdss.snap, icount 44.99M, gdb):** `__fThreadInit=0x00080102` — bit `0x2`
(JPEG `0x7007c`) **SET** (that thread ran and completed init), bit `0x10` (`0x7efdc`) **CLEAR**;
codec selector `0x400399f0=0`. The JPEG decoder thread is alive and the two boot photos decode
and render (§12.27/§12.30) — the JPEG decode-completion is **already faithful and poll-based**
(§10.8: VLD/JPU/BIU polls, all modeled and satisfied). Nothing about JPEG completion is missing.

**Both decode-completion INTERRUPT paths are now measured NEGATIVE.** New env-probe
`CT952_DECDONE` (machine.c, `JPU_GO` handler) raises the PROC1-**1st** decode-done sources
`RL_DONE 0x20 | MC_DONE 0x40 | INT_16L 0x80` (LEON line **13**) on each *actual* new-frame
completion (never VSYNC bit0, which the firmware masks faithfully §12.41). §12.41 proved these
bits are unmasked at the park and line 13 is enabled, so the raise really cascades to
`INT_Proc1_1st_isr`. Result over a crutch-free boot (`TICK_MULT=256`, `--run-to 90M`):

```
JPEG decode #1: 480x270 ; DECDONE frame#1 -> P1_1ST 0xe0 (mask fffffffe) @11.2M
JPEG decode #2: 640x360 ; DECDONE frame#2 -> P1_1ST 0xe0 (mask fffffffe) @40.3M
DUMPFLAGS @90M: __fThreadInit=0x00080102  __bPOWERONMENUInitial=0  _bOSDSSScreenSaverMode=0
```

No advance: the line-13 display/decode ISR clears the decode-done bits (display housekeeping)
without posting a CC-event completion. Together with §12.29's **PROC1-2nd** (line 10, BIU/MCU)
negative, **every decode-completion interrupt line is now tested negative** — line 7
(`INT_PROC2_1ST`, base+`0x1b0`) is audio-only (its sources are MIC/PCM/SPDIF underflows,
`ctkav_platform.h:193-200`), so it is not a decode-completion line either. `CT952_DECDONE` kept
env-gated as a documented-negative probe alongside `CT952_DECIRQ`.

**VERDICT — the soft-decoder + decode-completion-IRQ is NOT the boot blocker.** (1) No PROC2
software JPEG decoder exists in this build. (2) The JPEG decode-completion is already faithful
(thread `0x7007c`, poll-satisfied, photos render). (3) `0x7efdc`/bit-`0x10` is a non-boot codec
thread, not a completion DSR — forcing it is a crutch, and it is not in any boot wait. (4) Both
decode-completion IRQ paths (line 10 §12.29, line 13 §12.48) are measured negative. The real,
unchanged frontier remains the CC-event producer deadlock (§12.42-12.46): no HW event sets
F_REQ bit `0x80` / re-posts CC mailbox `0x40033830`, and that producer is a media/source DSR
(not a decode-completion one). This closes the "decode IRQ" class of leads the doc had kept open
since §12.28.

### 12.49 The media/source producer chain mapped end-to-end; media path RULED OUT as the blocker (source-proven); the CC-loop wake localizes to a display-restart that leaves VSYNC masked

Task: find/name/model the **media/source** producer that should keep the CC event
system alive (re-post CC mbox `0x40033830` / set F_REQ `0x40026e9c` bit `0x80`) so
`CC_DVD_MainLoop` cycles, `OSDSS_Monitor` re-runs, and the ~20 s idle screensaver arms.
Static RE (`dis.sh`, full-image objdump) + the authoritative firmware C sources. Live
gdbstub is **unusable in this harness** — the sandbox kills any process that opens a
listening socket (every `--gdb` launch dies before `[gdb] listening`; foreground
non-gdb `--run-to`/`DUMPFLAGS` runs work and are used below).

**The producer chain, fully mapped (addresses).**
- **Worker `0x6748`** (created by the thread factory `CreateThreadByType(9)`, §12.48): at
  startup it `OS_SetFlag(__fThreadInit 0x40038f80, 0x80000)` (`0x6754`), inits the flag
  pair F_REQ `0x40026e9c` / F_DONE `0x40026ea4` (`0x595e8` ×2), then **dispatches ONE
  message `(3,0x11)` via `0xad4cc`** (`0x6798`) — the single boot post §12.42 saw — and
  enters its loop: `OS_WaitFlag(F_REQ 0x40026e9c, mask -1)` at `0x67d4`, then a `btst`
  ladder over the returned bits (`0x800`→drain msg-queue at `[0x4002f820]`; `0x20`,
  `0x80`, `0x200`, …). `0xad4cc` is a **command dispatcher** keyed on the 2nd arg
  (`0x11`→`0xad590`, `0x21`, `0x22`…), not a raw mbox-put.
- **Bit `0x80` handler `0x6eec`**: `call 0x12f10` (PostEvent) → `ClearFlag(F_REQ,~0x81)` →
  `SetFlag(F_DONE,0x80)` (ack). So **F_REQ bit `0x80` is the "deliver a UI message /
  PostEvent" event**; it drives the page-8 advance / OSDSS dispatch.
- **Poster family** (the functions that SET F_REQ bits): `0x65dc(bit)` = "media-insert"
  poster — reads `__fThreadInit` bit `0x80000` (returns 0 if USB-src thread not up, it
  IS up), then `OS_SetFlag(F_REQ 0x40026e9c, bit)`. `0x6634(bit)` = "media-remove". Both
  are called by **`MediaPresentPost 0x6130`** `(sourceIdx i0, present i1)`: present→
  `0x65dc(2|4)`, absent→`0x6634(2|4)`, and it stashes `i0` at `0x40026e98`
  (`MediaInfo[i0].present` at `0x40039b08 + i0*25 + 0x30`). **MediaPresentPost sets F_REQ
  bits `2`/`4` (media insert/remove), NOT bit `0x80`.** The bit-`0x80` setter is a
  separate precompiled "post-message" poster.

**Media/source is NOT the direct blocker — the CC-loop poll-calls in that path are all
non-blocking (source-proven; corrects §12.46's `MEDIA_Management` guess).** Every
media/source poll `CC_DVD_MainLoop` runs uses flag-peek/-set, never a blocking get:
- `MEDIA_Management` (media.c:631) and `MEDIA_MonitorStatus` (media.c:1016)→
  `SrcFilter_TriggerUSBSRCCmd`/`SrcFilter_PeekUSBSRCCmd` (srcfilter.c:1224/1229) →
  `USBSRC_TriggerCmd`/`USBSRC_PeekCmd` (usbsrc.c:191/239) → **`OS_PeekFlag`/`OS_SetFlag`
  only** (no mbox-get, no wait). The CHECK_DEVICE handshake with the USBSRC thread is
  fire-and-forget flags; if the USBSRC thread never answers, the peek just returns FALSE
  and the loop continues — it does **not** block.
- `OSD_Trigger` (osd.c:1253) is a message-timeout scanner (`OSDND_Update`/
  `OSDND_GetMessagePos`), non-blocking.
So the CC thread's blocking untimed mbox-get on `0x40033830` (§10.50/§12.45/§12.46) is
**inside the precompiled OSD/event framework** (a callee of `DISP_MonitorVBICtrl` /
`DBG_Polling` / the CC thread's own top-level command loop), **not** in any media/source
call. The entire event framework — worker `0x6748`, `PostEvent 0x12f10`, the posters, the
CC/OSD mbox — has **no counterpart in the `.c` sources** (`grep` for `OS_*Mbox`/`cyg_mbox`
across every firmware `.c` returns nothing): it is linked from a precompiled library, the
same wall §12.34/§12.42 hit.

**SOURCE_SPI clarified — the keyless-screensaver source is built-in flash and produces NO
media event.** `srcfilter.c` handles `SOURCE_SPI` only as a **read-data** source
(`case SOURCE_SPI:` sets `__dwSFSPIStartAddr = SRCFTR_SPI_ENCODE_ADDR + pos*2048`,
srcfilter.c:516) — there is no `SOURCE_SPI` insert/detect/mount path and no
`MediaPresentPost(SOURCE_SPI,…)` anywhere. The built-in album is not "announced" via the
media-event system; it is played directly by the screensaver (`OSDSS_Entry`→
`_OSDSS_PictureUpdate`→`UTL_ShowJPEG_Slide`, osdss.c). Its **content** count is set at boot
by `MM_EncodeFile_Init` (mm_play.c:2402, called from `_INITIAL_...`, initial.c:1326):
`HAL_ReadStorage(SETUP_ADDR_JPG_ENCODE_MASK)` → if the setup-storage mask ≠
`MM_JPG_ENCODE_HAVE_DATA` (fresh EEPROM) it sets `__bMMJPGEncodeNum = BUILD_IN_JPG_ENCODE_NUM`
= **3** (`SUPPORT_BUILD_IN_ENCODE_JPG`, Winav.h:1590/1594); else it counts the stored list.
`OSDSS_Entry` bails immediately if `__bMMJPGEncodeNum==0` (osdss.c:235). So the screensaver
**content** is available on a fresh boot, and **arming depends only on the CC loop cycling**
(so `OSDSS_Monitor` re-runs and the >`OSDSS_ENTER_TIME`≈20 s idle timer accumulates) with
`__bPOWERONMENUInitial`=1. The media/source system is a red herring for the SPI screensaver;
the deadlock is purely "the CC loop stops cycling."

**Where the per-frame CC wake really is — VSYNC, gated behind a display-restart the boot
never performs (refines §12.41; new measurement).** The CC get on `0x40033830` is untimed
(§12.45), so only a **post** can wake it — i.e. a HW-event producer. At idle (no keys, no
media insert, no decode, no audio) the **only** recurring HW event is the panel/TG **VSYNC**
(`P1_1ST` bit0 → LEON 13 → `INT_Proc1_1st_isr`). VSYNC is enabled once by the display-init
interrupt block **`0x3fac0`** (`st -1,[0x800000b0]`=`P1_1ST_MASK`, ~4.9 M, §12.41), then
**masked** by the display-STOP routine **`0xa41f0`**: `st 1,[0x800000bc]` (`P1_1ST_MDIS`
bit0) **and** `ld [0x4002401c]; or 2; st` (sets display-state **bit1 = stopped**) inside a
PSR-critical section, at ~9.8 M. The display-enable block `0x3fac0` **never re-runs** after
that — so VSYNC stays masked for the whole park. Measured live (rebuilt emulator,
`--restore osdss.snap`, `DUMPFLAGS` at the ~90 M park, new fields added this session):
```
disp_state(bit1=stopped) @4002401c = 0x00000027   <- bit1 SET: display left STOPPED
F_REQ(worker wake)       @40026e9c = 0x00000000   <- bit 0x80 never set
P1_1ST mask@0b0=fffffffe pend@0b4=00000001         <- VSYNC firing (pend=1) but masked (bit0=0)
__bPOWERONMENUInitial=0   _bOSDSSScreenSaverMode=0
```
The emulator **faithfully generates VSYNC** (`P1_1ST_PEND` bit0 set) and **faithfully
respects the firmware's mask** — so re-arming VSYNC in the model would be a crutch. The real
gap is that the boot is left **display-STOPPED** (`disp_state`=0x27, bit1) and the
display-RESTART that would re-run `0x3fac0` (re-arm VSYNC → per-frame CC wake) never fires.
That restart is itself gated behind the boot advancing (mode-8→mode-7→POWERONMENU→"display
running"), which is gated behind the very event post that VSYNC would produce — the
**same event-starvation circle** (§12.33), now pinned to the **display-stop/restart** state
(`0xa41f0` / `0x3fac0`, flag `0x4002401c`) rather than to any media/source register.

**What was modeled/added (faithful only).** No crutch. Added the two measurable roots to
`CT952_DUMPFLAGS` (`main.c`): **F_REQ `0x40026e9c`** (worker-wake flag; bit `0x80` = the
missing event) and the **display-state flag `0x4002401c`** (bit1 = display-STOPPED). These
make the deadlock's two independent halves — "event never posted" and "display never
restarted" — measurable at any `--run-to` checkpoint. Measured values recorded above.

**VERDICT / honest gap.** The media/source producer the task asked for is **structurally
absent for the SPI screensaver**: SOURCE_SPI is built-in flash with no insert event, and the
media-detect polls (`MEDIA_Management`/`MEDIA_MonitorStatus`/USBSRC-CHECK_DEVICE) are all
non-blocking flag ops that cannot post the missing message. The genuine missing producer is
the **VSYNC / per-frame display interrupt**, whose DSR (in the precompiled OSD/event
framework) re-posts the CC/OSD mbox each frame on real hardware; in emulation it is masked
because the boot is left display-STOPPED and the display-restart (`0x3fac0`) never runs — a
circle that closes only when the CC event fires. There is **no single unmodeled register**
whose faithful modeling breaks the circle from the HW side (VSYNC is already generated and
correctly masked; the media path is already satisfied). Breaking it requires either
(a) reversing the precompiled display state-machine to find the specific
poll/HW-completion that gates the display-restart after the boot's still-photo display
(`0x4002401c` producers, `0xa41f0`↔`0x3fac0`), or (b) a live tracer that can breakpoint the
`0x40033830` mbox-get and read the caller — impossible in this harness (listeners are
killed). **Precise next step:** in a live-capable environment, break on the `0x40033830`
get, read `%i7` to name the precompiled poll-call that blocks, and break on writes to
`0x4002401c` to catch whether/when the display-restart runs. New instrumentation:
`CT952_DUMPFLAGS` now dumps `F_REQ 0x40026e9c` + `disp_state 0x4002401c`.

### 12.50 LCM / display-controller raster modeled FAITHFULLY (live MEM_LINE + N-hsync line-int + DMA scan-address); display-restart is COMMAND-driven, not raster-driven — LCM RULED OUT as the boot blocker

Task: stop stubbing the LCM / display timing-generator register block to
last-written constants and model it faithfully — a **live raster line**
(`REG_DISP_MEM_LINE 0x80001A68`), the **display-line/hsync interrupt**
(`REG_DISP_N_HSYNC_INT 0x80001A6C`), and a **live DMA scan-address**
(`0x80000E0C/E10`) — then measure whether a firmware that now sees a live display
engine advances the display state machine (re-runs the display-restart `0x3fac0`,
re-arms VSYNC, clears `disp_state 0x4002401c` bit1, cycles the CC loop, arms the
screensaver). The `--gdb` stub **works in this harness** (contrary to §12.49's
"listeners killed" note — every `--gdb` launch here reaches `[gdb] listening` and
accepts a client); it is used below. NB the stub's memory reads
(`machine_dbg_read`) only cover flash/DRAM/BRAM — **IO-page reads via gdb return
0**, so live IO-register values must be read by native trace, not the stub.

**Raster timing decoded from the firmware's own programming (measured `--iolog`).**
The DISP timing generator is programmed once during bring-up and the values are
stable through the park:
```
TGEN_TOTAL 0x1A38 = 0x120d035a   bit28 DISP_TGEN_EN set; (Vtotal<<16)|Htotal
                                 -> Vtotal = (v>>16)&0xFFF = 0x20d = 525 lines
                                 -> Htotal =  v     &0xFFFF = 0x35a = 858 pel
SYNC_WH    0x1A3C = 0x0014000a   Hsync width 0x14=20, Vsync height 0x0a=10;
                                 bit28 DISP_PSCAN_EN CLEAR -> interlaced
SCREEN_SIZE 0x1A44 = 0x00f002d0  720 x 240 (one field); SCREEN_POS 0x1A40=0x00170087
```
i.e. a standard NTSC-class 858x525 interlaced raster, 720x240 active per field.
`SYNC_WH` matches `disp.h` `TVSYNC_WIDTH=20`/`TVSYNC_HEIGHT=10`, confirming the
`(H<<16)|V` field order (and hence the same order for `TGEN_TOTAL`).

**What was modeled (faithful, `machine.c` — no crutch).**
- **Live `REG_DISP_MEM_LINE 0x1A68`** (`io_read` case): when `DISP_TGEN_EN` is set,
  return `disp_cur_line(m)` — the scan line derived from the *same*
  `vsync_cnt/vsync_div` field clock that raises VSYNC in `machine_cycle` — masked
  `&0x7FF`, OR'd with `DISP_EVEN_FIELD (0x10000000)` on odd fields. Because the
  line is derived from the VSYNC counter, **`MEM_LINE==0` coincides exactly with
  the top-of-frame VSYNC-pending assertion**, so `spflash._VSYNCPolling`'s
  `(P1_1ST_PEND & VSYNC) && (MEM_LINE==0)` fires at the real vblank and gdi's
  `GDI_CHECK_DISP_MEM_LINE` sees the raster sweep out of the draw rectangle. When
  `DISP_TGEN_EN` is clear (generator off, e.g. early boot), return the last-written
  value (the firmware's idioms then see the static 0, as before). Field lines =
  `Vtotal` (progressive) or `Vtotal/2 ≈ 262` (interlaced, this panel).
- **N-hsync line interrupt** (`machine_cycle`): if `REG_DISP_N_HSYNC_INT 0x1A6C`
  is programmed non-zero AND the generator is enabled, raise `P1_1ST` bit1
  (`INT_PROC1_1ST_HSYNC 0x02`) each time the raster crosses a multiple of N lines.
  Guarded so it is free when unused. **This firmware never programs 0x1A6C** (0
  reads/writes in the iolog) and the compiled `INT_Proc1_1st_isr` HSYNC handler
  (`interrupt.c:151`) is **empty**, so the machinery is faithful-but-inert here.
- **Live DMA scan-address `0x80000E0C/E10`** (`io_read` cases): when the generator
  is enabled and a real DRAM start pointer is programmed (`[0xE04]≥0x40000000`,
  `[0xE08]>[0xE04]`), walk a current pointer from start toward end in step with the
  raster line; else return last-written (so the shared `0x80000E00` DMA-descriptor
  block, also touched by non-display DMA, is left untouched). These are read
  **only** by the display-enable safe-scan check (flash `0x3fa40`, 2 reads), which
  compares them against `0x1fff`/`0x1cf00`; a framebuffer pointer (`≥0x40000000`)
  is always `>0x1cf00` → the "scan is safe, proceed" branch, unchanged from the
  frozen `0x40000000`. New struct fields `disp_field`, `disp_hsync_grp`; new
  env probe `CT952_MLTRACE` (logs MEM_LINE reads + PC). Verified live: at ~40M with
  the generator on, `MEM_LINE` reads `0x10000071` (even field, line 0x71=113) from
  poller `pc=0x4001fb14` and sweeps across the frame — **the raster is genuinely
  live**, no longer a constant.

**Measured A/B (crutch-free boot, `TICK_MULT=256`, `--run-to 90M`) — the live
raster changes NOTHING about the deadlock.**
```
                     baseline (frozen)     with live raster (§12.50)
disp_state 0x4002401c   0x00000027            0x00000027   (bit1 STOPPED)
P1_1ST mask@0b0         fffffffe (VSYNC off)  fffffffe     (VSYNC off)
P1_1ST pend@0b4         00000001              00000001     (VSYNC firing, masked)
F_REQ 0x40026e9c        0x00000000            0x00000000   (bit0x80 never set)
__bPOWERONMENUInitial   0                     0
_bOSDSSScreenSaverMode  0                     0
```
Identical. The display stays STOPPED, VSYNC stays masked, the CC loop never cycles,
the screensaver never arms. The display-STOP still fires at the same instants
(`MDIS clear` at 9.78M and 11.3M, `CT952_VSMTRACE`), and the display-RESTART's
VSYNC re-arm (`MASK<-ffffffff` at `pc=0x3fac8`) still runs **exactly once at 4.94M**
(initial bring-up) and never again.

**Why the raster can't break the circle — the display-restart is COMMAND-driven,
not raster/interrupt-driven (proven by static call-graph + live `%i7` trace).**
- **STOP** `0xa3d50` (sets `MDIS` VSYNC-off + `disp_state|=2`) is called
  **synchronously** from the display-mode functions `0x33c70` / `0x3529c` — it is
  a leaf of a display reconfiguration; it does **not** call the restart and simply
  returns.
- **RESTART** `0x3f7bc` (whose tail `0x3fa40` runs the E0C/E10 safe-scan and
  `0x3fac0` re-arms VSYNC `st -1,[0x800000b0]`) has **no static caller**; it is
  reached only via `0x41728` inside the mode function `0x416f4`, which runs the
  restart only when its command arg has **bit2 set (0x14)**. `0x416f4` is called
  from `0xa8c0` (cmd `0x11`, bit2 clear → no restart) and from `0xadb0`
  (cmd `0x14`, bit2 set → restart). `0xadb0` has **no static caller** — it is an
  indirectly-dispatched command handler. Live `--gdb` trace (restore `pre2.snap`
  @4.5M, break `0x416f4`/`0x3f7bc`) caught the one dispatch:
  ```
  HIT MODEfn(0x416f4) i7=4001ea60  cmd(o0)=0x14      <- 0xadb0 handler, cmd 0x14
  HIT RESTART(0x3f7bc) i7=0000adbc  ...              <- 0x416f4 -> 0x41728 -> restart
  ... then only "[idle]" — restart command NEVER dispatched again.
  ```
  So the restart is dispatched by the **precompiled OSD/event command loop**
  (dispatcher frame `0x4001ea60`) delivering **command `0x14`** to handler
  `0xadb0`. That is the *same* CC/event command framework that §12.45-49 proved is
  blocked in the untimed mbox-get on `0x40033830`. The restart is gated on a
  **message dispatch**, not on any raster poll or display interrupt — so a live
  `MEM_LINE`/scan-address (which only feed *polls*, all of which already completed
  on the boot path — `0x1A68` is read a bounded ~63-104x, never in an infinite
  wait) **cannot** cause command `0x14` to be dispatched.

**VERDICT — the LCM / display controller is faithfully modeled and RULED OUT as the
boot blocker.** The timing generator, raster line, field parity, line-interrupt,
and DMA scan-address are now live and self-consistent (VSYNC ⇔ `MEM_LINE==0` at
top-of-frame), and the emulator continues to generate VSYNC and faithfully respect
the firmware's VSYNC mask. Making the raster live is measurably **inert** to the
deadlock because the deadlock's gate is upstream of all display polling: the
display-RESTART (`0xadb0`→`0x416f4(0x14)`→`0x3f7bc`→`0x3fac0`) is a **command**
posted through the precompiled CC/OSD event loop, and that loop is the one blocked
in the `0x40033830` mbox-get with no producer to wake it (§12.49). **The precise,
unchanged next gate:** the producer that should re-post CC mbox `0x40033830` / set
`F_REQ 0x40026e9c` bit `0x80` — a DSR in the precompiled event framework — is never
generated in emulation; on real hardware the per-frame VSYNC DSR does it, but here
VSYNC is (faithfully) masked because the boot left the display STOPPED, and the
restart that would re-arm VSYNC is itself gated behind that same event loop. To
break it one must either reverse the precompiled framework's `0x40033830`-get
caller (break there live, read `%i7`) and the command-`0x14` producer, or deliver
the missing per-frame event. New instrumentation this session: `CT952_MLTRACE`;
the live-raster model is unconditional (faithful HW), not env-gated.

### 12.51 PROC2 SOFTWARE JPEG DECODER — confirmed present (corrects §12.48A), but STAGED-AND-HELD, never released at boot; its PROC1 completion is a POLLED AIU-GR mailbox, not the missing CC-event producer

Task: on the hypothesis that a PROC2 "frame-done" handshake is the unmodeled
per-decode event producer that should wake the CC/OSD loop (F_REQ `0x40026e9c`
bit `0x80` / re-post CC mbox `0x40033830`, §12.42-12.50), determine empirically
what PROC2 (`cpu2`) does during a photo decode and model its completion signal
faithfully. Static RE (`dis.sh`, `jpegdec.bin` disasm) + full-speed native trace
(new `CT952_P2FULL`, `machine.c`); the `--gdb` stub was not needed.

**(1) Binary reality — `SUPPORT_JPEGDEC_ON_PROC2` IS defined (corrects §12.48A).**
§12.48A reasoned from the *stale* root header (`platform.h` `CT909P_IC_SYSTEM`) and
wrongly concluded the macro is off. The **linked image** proves otherwise:
- The `#ifdef SUPPORT_JPEGDEC_ON_PROC2` AIU-GR2..16 reset loop (`chips.c:1374`,
  `utl.c:5194`) **executes** — `--iolog` shows GR2..GR16 (`0x80000788`–`0x800007c0`)
  each written `0` (one write apiece). (The `initial.c:1874` copy is guarded by
  `CT951_PLATFORM`, a different macro, and is *not* the source — the two that fired
  are the `SUPPORT_JPEGDEC_ON_PROC2` ones.)
- The firmware **stages PROC2 to run the JPEG code**: at pc `0x3f830` (called from
  the display-restart chain `0x41728`, §12.50) it writes SP GR21 `0x800007d4` =
  `0x4001cf00`, START GR22 `0x800007d8` = `0x40002000`, boot-cmd GR25 `0x800007e4`
  = `0x00010003` — the exact `HAL_ReloadAudioDecoder` handshake (`hdecoder.c:702-714`).
- `jpegdec.bin` in the build tree **is** the real PROC2 SPARC image: a trap table
  at load-addr `0x40002000` (`reset → 0x40003000`), and its boot handshake (offset
  `0x131c`) spins reading GR25 until `(GR25 & 0x1ffff)==0x10003`, then `clr [GR25]`
  to ack — the PROC2 side of `hdecoder.c:724-737`. So a PROC2 **software** JPEG
  decoder unquestionably exists. §12.48A's premise ("no PROC2 JPEG decoder") is
  **struck**; its *operational* conclusion survives (see below).

**(2) MEASURED: PROC2 executes ZERO instructions during the boot.** Full-speed
`CT952_P2FULL` trace (every `0x80000304`/`0x324`/`0x98000000`/GR21/22/25 write, any
value) over a 90M crutch-free boot, cross-checked against the reset-bit map
(`ctkav_platform.h:302/340`: `PLAT_RESET_PROC2_*` = bit0 `0x1`; **not** the VPU/JPU
reset bit23 `0x00800000` that shares `0x304/0x324` via `MACRO_RESET_JPU`, 541× per
JPU op — that distinction is what hid the picture from bit-0-only filters):
```
icount 4,943,791  GR21(0x7d4)=4001cf00   pc=3f830   ; stage SP
icount 4,943,794  GR22(0x7d8)=40002000   pc=3f830   ; stage START (JPEG load addr)
icount 4,943,801  GR25(0x7e4)=00010003   pc=3f830   ; boot-cmd
icount 4,943,860  0x80000324 = 00000001  pc=3f9c4   ; PLAT_RESET_PROC2_ENABLE (assert = HOLD)
       DRAM@40002000: 02e4056b 02c20593 ...          ; NOT jpegdec.bin (a0100000...) -> code NOT loaded
icount 4,943,862  GR25(0x7e4)=00000000   pc=3f9cc   ; pre-clear ack slot
--- and then, in all 90M: ZERO writes of 0x80000304=..01 (PROC2 release),
    ZERO 0x98000000 (DSU2 release). Only bit23 VPU resets recur. ---
[EXIT, CT952_PROC2=1] PROC2 on=0 pc=00000000 icount=0 halted=1
```
So `proc2_boot` is **never** triggered: the firmware asserts PROC2 reset **once**
(hold) and never deasserts it. The staged JPEG code is not even present at
`0x40002000` at that point (the ROMLD "JPEG" decompress hasn't run), and the
"wait-for-boot-ACK" poll right after the hold is satisfied *trivially* because
PROC1 pre-cleared GR25 itself (`GR25>>16 == 0`), so the firmware reads "boot OK"
without PROC2 ever running. **`cpu2` executes 0 instructions the entire boot.** The
host-`picojpeg` model runs the boot photos (#1 480×270 logo, #2 640×360 album)
*instead of* PROC2 — faithfully, since PROC2 isn't the decoder here.

**(3) The real PROC2→PROC1 completion mechanism (for the record — never exercised).**
It is a **polled shared-register mailbox**, not an interrupt (PROC2's `bus2` has no
async IRQ wiring, and `HAL_ReadInfo`/JPEGDEC read status by polling): boot-ack via
**GR25 `0x800007e4`** (PROC2 clears it → PROC1 polls `>>16==0`, `hdecoder.c:728`),
and per-decode command/status via the **AIU-GR(2..17) bank `0x80000788`–`0x800007c4`**
(`chips.c:1372` "REG_AIU_GR(2)~(17) connect PROC1↔PROC2"); in `jpegdec.bin` the
workhorse status register is **GR15 `0x800007bc`** (72 accesses). Because completion
is **poll-consumed by the JPEG worker thread `0x7007c`** — exactly like the HW-JPU
`JPU_BUSY`/BCR0A polls the emulator already models and satisfies (§10.8/§12.48B) —
a PROC2 completion would raise **no DSR and post no message**, so it structurally
**cannot** be the producer that sets F_REQ bit `0x80`. That producer is a
message/DSR in the precompiled CC/OSD event framework (§12.49/§12.50), a different
class entirely.

**(4) What was modeled / changed.** Nothing faked — fabricating a PROC2 completion
IRQ/mailbox post would be a crutch (the event never occurs). The emulator's PROC2
model is already faithful for the release case: when the firmware *does* deassert
PROC2 reset (`0x80000304` bit0) or DSU2-release (`0x98000000`), `proc2_boot` seeds
`cpu2` at GR22/GR21 and `machine_run`/`machine_step_bp` step it natively on the
shared bus, so it would drive the real GR15/GR25 handshake itself (no stand-in).
Added **`CT952_P2FULL`** (`machine.c`, env-gated): the complete PROC2 lifecycle
trace (bit0-vs-bit23 reset disambiguation + GR21/22/25 staging + a DRAM dump of the
staged entry on each core-reset, to check whether the "JPEG" section was actually
loaded) that produced the measurements above. The default PROC2 gate
(`CT952_PROC2`) is left as-is: it is measurably inert to this boot (no release → no
`cpu2` step), and flipping it risks running unloaded `0x40002000` garbage if a
release ever appears.

**(5) Measured after this pass (raw crutch-free boot, `TICK_MULT=256`, `--run-to
90M`, `CT952_PROC2=1`):**
```
[EXIT] PROC2 on=0 icount=0 halted=1        <- cpu2 never ran
__fThreadInit          = 0x00080102        (JPEG 0x2 + Parser 0x100 + USB 0x80000; unchanged)
__bPOWERONMENUInitial  = 0x00
_bOSDSSScreenSaverMode = 0x00              <- screensaver NOT armed
F_REQ 0x40026e9c       = 0x00000000        <- bit 0x80 STILL never set
event_flag 0x40026ea4  = 0x00000000
disp_state 0x4002401c  = 0x00000027        (bit1 stopped, unchanged)
```
No PROC2 completion fires because PROC2 never runs; F_REQ bit `0x80` stays clear,
the CC mbox is not re-posted, and the screensaver does not arm — identical to the
pre-existing baseline.

**VERDICT — PROC2-JPEG-completion STRUCK from the boot-stall suspect list** (joining
MPEG §12.47 and the `0x7efdc` codec-thread / decode-IRQ class §12.48). The PROC2
software JPEG decoder **does exist** in this build (correcting §12.48A's reason),
but it is staged-and-held and **never released** at boot, so it executes nothing and
produces no completion signal; and even if it ran, its completion is a **polled**
AIU-GR mailbox consumed by a worker thread, not a DSR that could set F_REQ bit
`0x80`. Its activation is gated behind the *same* command-driven display-restart /
CC event loop that is deadlocked in the untimed `0x40033830` mbox-get (§12.49/§12.50)
— it is collateral of that deadlock, not its cause. The unchanged frontier remains
the CC/OSD event-framework producer (§12.49/§12.50). New instrumentation:
`CT952_P2FULL` (`machine.c`).

### 12.52 eCos-timeout hypothesis CLOSED — the CC mbox-get is UNTIMED (needs a real message, not a timeout)

Disassembled the live DRAM eCos kernel (from m8b.snap) along the CC-thread block path to settle whether the wait is timed (and a failing eCos alarm/DSR delivery could explain the stall) or untimed. It is **untimed**, definitively:
- Flash wrapper `0x5969c`: `save; call 0x4001de40; ret` — passes NO timeout.
- `0x4001de40`: `o0 = *(0x400423a8) = 0x40033830` (the CC mbox); `call 0x4001e498(obj+0x1c)` — no timeout arg.
- get-primitive `0x4001e498`: bumps sched-lock (`0x40024974`), reads mbox `[+0x3c]` (message-count); if `!=0` returns the message, **else dequeues the thread from the ready-list at `0x4002e3f0` and reschedules (`call 0x4001e66c`)** — a plain untimed sleep. No alarm object, no timeout deadline.

So the CC loop blocks until a message is **posted** (put-primitive `0x4001f2e4`/`0x4001de5c`/`0xad4cc` sets `[+0x3c]` and wakes the waiter), not until a timer expires. The eCos clock/alarm subsystem is NOT the gate — a working or broken timeout is irrelevant to an untimed wait. This eliminates the last "kernel-timing" explanation and re-confirms: the stall is a **missing message POST**, and every hardware event source that could drive that post has been ruled out (§12.28-§12.51). The producer is a software post gated, circularly, behind the same loop it would wake.

### 12.53 CT909R correction does NOT reopen a producer thread — §12.47/§12.48's thread reading holds (chip-config-independent)

The CT909R discovery (SUPPORT_JPEGDEC_ON_PROC2 real, correcting §12.48A) raised the
question: does the corrected chip config reopen a viable event *producer* thread —
specifically the §12.32 InfoFilter hypothesis (a producer blocked in unmodeled HW
init, never signaling `__fThreadInit` bit `0x200`)? **Re-verified: no.**

**(1) The thread-creation dispatch is a direct disasm, independent of chip config.**
`INITIAL_ThreadInit` (flash `0x41b60`) is a `switch(id)` over a subsystem id, valid
cases **{2, 5, 9, 0xb}** (id 3/4 and >0xb fall through to a bare `ret` at `0x41d50`
— no-ops). Each case builds a fixed descriptor set and calls the module creator
`0x41d58` for ids drawn from **{3,4,5,0xa,0xb,0xc,0xd,0xe}**. **Thread/module id 8
(INFO_FILTER) is not produced by any branch.** This is machine code in the flash
image — the same bytes regardless of whether the source was compiled CT909P or
CT909R — so §12.47's "which threads are created" reading stands on the corrected
footing.

**(2) The runtime ground truth already settles it.** Across every crutch-free boot
(measured to 90M ticks, §12.51) `__fThreadInit = 0x00080102` = JPEG(`0x2`) +
Parser(`0x100`) + USB(`0x80000`). **InfoFilter bit `0x200` is never set** — and
neither is any bit that would correspond to a CC/OSD event producer. Whether the
InfoFilter thread is "created-but-blocked" or "never-created" is moot: its
init-complete bit never asserts under any stimulus or time budget we can apply,
and it is not the consumer/producer of the starved `0x40033830` CC mbox anyway.

**(3) Boot-time dispatch id.** The mode-0 boot path (`0x418f0`) calls
`INITIAL_ThreadInit(9)`, `(3)` [no-op], `(2)` — i.e. the minimal subsystem set. The
larger JPEG/photo sets (ids 5, 0xb, creating modules 3..0xe) are reached from the
later product-init cluster (`0x5a0a4`/`0x5a73c`/`0x5a7a4`/`0x5a80c`/`0x5a87c`), all
gated behind the same CC command/event framework that is deadlocked in the untimed
mbox-get (§12.52). Nothing here is an independent producer that runs *before* the
deadlock and could break it.

**VERDICT — the CT909R correction changes the *decoder* story (PROC2 soft-JPEG is
real, §12.51) but NOT the *producer* story.** No new event-producing thread is
opened by the corrected config. The InfoFilter hypothesis (§12.32) is closed on
correct footing: the bit never asserts, the thread is not the CC-mbox producer, and
the dispatch that would create the larger subsystem sets is itself downstream of the
deadlock. **Frontier unchanged: the missing message POST to `0x40033830` / F_REQ bit
`0x80`, whose every candidate hardware and software source (§12.28-§12.52) has been
ruled out or shown to be circularly gated behind the same starved loop.** We are at
the genuine limit of the pure-emulation reproduction: the producer is a software
post that is, itself, waiting on the loop it would wake.

### 12.54 AUTHORITATIVE pinout from the PF7301 board schematic (CT952A, 128-pin QFP)

User supplied the real board schematic (SHEN ZHEN MTC MULTIMEDIA, model **PF7301**,
DWG PF7301-YL01-01) — the shipping photo-frame board around the **CT952A** (`U4`,
128-pin QFP). Full pin↔net map extracted and cross-checked against `hio.c` routing.
This is ground truth for the physical console and human-input pins.

**Console = UART1, routed on the CARD_READER path** (nets suffixed `_CR`):
| Signal | Pin | Pad | Net | Firmware (`hio.c`) |
|---|---|---|---|---|
| UART1 TX (console out) | **2** | `GPC[7]/SDCMD` | `TX1_CR` | 2137 "USE GPC[7] for TX"; `GPCMux[12]=1`, `SYS_PIN_USE0[12]=1` |
| UART1 RX (console in)  | **1** | `GPC[8]/SD_D0` | `RX1_CR` | 2213 "USE GPC[8] for RX"; `GPCMux[16]=1`, `SYS_PIN_USE0[8]=1` |

Both emerge on the **SD/card-reader connector**. Emulator captures TX at
`R_UART1_DATA = 0x80000070` (STAT `0x74`) regardless of pad; RX is a live polled
input (STAT bit0 → DATA read) — a faithful path for a serial-monitor stimulus.

**Hardware debug port = SPARC DSU, strap-selected by J102/J103**
(schematic: "J102,J103 SHORT = DSU MODE; OPEN = PROCESS MODE"):
| Signal | Pin | Pad | Net |
|---|---|---|---|
| DSU1 TX | **37** | `GPD[3]/STVD` | `DSU1TX_CR` |
| DSU1 RX | **1** | `GPC[8]/SD_D0` | `DSU1RX_CR` (shares pin 1 with UART1 RX) |

This is the "DSU1/UART1 shared case (128-pin, ex 909R)" the firmware handles
explicitly (`hio.c:2061`) — one RX pin (pin 1) feeds either the DSU debugger or the
UART1 console per strap. **Normal boot = straps OPEN = PROCESS mode** (what the
emulator models); DSU MODE diverts to the hardware debug unit. The presence of a
128-pin DSU/UART-shared part independently **corroborates the CT909R-family / 909R
build** already established from the binary (§12.51).

**Human-input event sources (front panel) — now physically pinned:**
- **IR receiver → pin 23 (`IR`)**, net `IR_INT` (→ the IR/GPIO int the emulator models).
- **Analog resistor-ladder keypad** on the KEY_DET ADC inputs: **pin 25
  `GPC[0]/KS_IN0` (`KEY_DET0`)** and **pin 24 `GPC[5]/KS_IN1` (`KEY_DET1`)**, with
  documented rail voltages: KEY_DOWN 0V, KEY_UP 0.6V, KEY_LEFT 1.5V, KEY_STOP/
  KEY_RIGHT 2.2V, KEY_PAUSE_PLAY/KEY_FUNCTION 2.94V.
These two are the real operator-input producers — the leading remaining candidate for
a "~20s in, something fires" stimulus that a passive emulator never generates.

**Selected other CT952A pins (for reference):** 22 `/RESET`, 19/20 `XTALI/XTALO`,
89 `MCLK`, 21 `DFTEN` (test), 12/13 `DN/DP` (USB), 9 `VBUS`, 26-29 `GPA[0..3]/SPI-
flash SF_CSN/SFCLK/SFDIO/SFD0`, 116-120 `GPG[0..4]` NIM/tuner (unused on photo
frame), 57 `CVBS`, 59/61/63 `R/G/B`, 66-68 `VOUTB/G/R`. Full 128-pin table archived
in the extraction script output.

### 12.55 The slideshow is the IDLE screensaver — gated by __bPOWERONMENUInitial, NOT by a flipped key (ADC no-key verified correct)

Two user insights drove this pass: (a) the panel keys are a **SAR/ADC resistor
ladder** (confirmed §12.54, KEY_DET0/1 pins 25/24), and (b) *"buttons don't trigger
the slideshow — LACK of buttons does."* Both are exactly right, and they pin the
acceptance test precisely.

**The slideshow == the OSDSS idle screensaver.** `OSDSS_Monitor()` (osdss.c:298,
called from the CC main loop cc.c:1004) enters the JPEG slideshow (`OSDSS_Entry`)
when the box has been idle (`__dwOSDSSCheckNOData == __dwTimeNow`, i.e. playback
position unchanged) for longer than **`OSDSS_ENTER_TIME = COUNT_10_SEC*2 ≈ 20 s`**
(osdss.h:20, the DMP952A value) — **but only if `__bPOWERONMENUInitial == TRUE`**
(plus clock off, no alarm). That 20 s is the "~20 s in" the user has cited all along.

**The SAR/ADC key path, fully mapped:** `PANEL_KeyScan()` (panel.c:198) kicks a SAR
conversion via `ADCGLB = 0x8000407C` (write `0x00840000` ch0, `0x00C40000` ch1 for
DMP952A), spins a short delay, reads the 8-bit result from bits [31:24], and
threshold-decodes: **result ≥ 0xF0 ⇒ no key** (ladder pulled to rail); < 0xF0 ⇒ a
key. `aScanMap[0] = KEY_NO_KEY`.

**"Flipped key / keeping it alive" hypothesis — EMPIRICALLY REFUTED.** The emulator
models `ADCGLB` as `0xFF000000` (machine.c:403) ⇒ decode → `KEY_NO_KEY`, i.e.
*correct* idle, not `KEY_PICTURE` (which would force standby). A new write-watch
(`CT952_WWATCH`, machine.c) on the three deciding globals shows, across a crutch-free
boot:
- `__bPOWERONMENUInitial (0x40023a10)`: written 0 only by early bss-clear, **never
  set to 1** — the gate never opens.
- `__bISRKey (0x40039074)`: written 0 once at 4.8M, **never a phantom key** — nothing
  is injected that would reset the idle timer.
- `__dwOSDSSCheckTime (0x400239b8)`: stays `0xFFFFFFFF` — `OSDSS_Monitor`'s first-call
  init branch **never runs**, so the idle clock never even starts.

So the emulator is NOT erroneously holding a key down; the ADC/idle side is faithful.
The slideshow can't arm because the boot never reaches the power-on-menu state.

**Where it's actually stuck.** `__bPOWERONMENUInitial` is set at the *end* of
`POWERONMENU_Initial()` (950_Files/poweronmenu.c:434), called once at boot
(cc.c:1320). The logo JPEG *does* decode (that call is inside POWERONMENU_Initial,
line 417), so it enters the function but blocks before line 434 — in the display/
decoder bring-up (`DISP_DisplayCtrl(DISP_MAINVIDEO,TRUE)`). A periodic PC sampler
(`CT952_PCSAMPLE`, sparc.c) shows steady state from ~38M: the eCos scheduler cycling,
with a **generic bounded-wait primitive `0xa33d8`** (waits up to 0x18=24 ticks
polling the CC event mbox `0x5969c`→`0x40033830`, osdss/display region) timing out
and being re-called forever — the event never posts. One thread also periodically
runs the PROC2 vdec-playmode poll (`0x70890` on bram `0xB0000190`).

**VERDICT.** The user's model is correct end-to-end: no-key idle → 20 s → slideshow,
and the ADC no-key is modeled faithfully. The block is NOT a spurious keep-alive; it
is the *same* missing-event-POST frontier (§12.49-§12.53), now pinned to its
acceptance-test meaning: **POWERONMENU_Initial cannot finish its display bring-up, so
`__bPOWERONMENUInitial` never turns TRUE and the idle screensaver machinery never
starts.** New instrumentation: `CT952_WWATCH`, `CT952_PCSAMPLE`.

### 12.56 The gate is set EARLIER than thought: POWERONMENU_Initial is never CALLED — INITIAL_System hangs on a video-decoder state poll

Correcting the §12.55 working hypothesis ("POWERONMENU_Initial blocks inside
DISP_DisplayCtrl"): empirically, with the new configurable PC tracer
(`CT952_PCWATCH`, sparc.c), across a full crutch-free boot (to ~240M instr):
- `POWERONMENU_Initial` (flash **0x61be8**) — **0 hits**.
- `DISP_DisplayCtrl` (flash **0x4a754**, 33 callers) — **0 hits**.
- positive control (0x5969c, 0x70268, 0xa33d8) — hit normally.

So the photo-frame thread **never reaches the power-on-menu setup at all**; the
earlier logo JPEG decode comes from a *different* `UTL_ShowLogo` path, not
POWERONMENU_Initial. `cc.c:1320` (the POM call) sits in **`Thread_CTKDVD`**
(cc.c:1283), *after* `INITIAL_System` (1312) and `INITIAL_PowerONStatus` (1315).

**Where the thread actually is.** A register-window stack-unwinder (`CT952_STACKW`,
sparc.c) taken at the steady-state mbox poll shows the one running thread's chain:
thread body (`0xadc4`, which calls the init sequence `0x41614`/`0x416f4`/**`0x418f0`**
— the `INITIAL_ThreadInit(9),(3),(2)` subsystem-init dispatcher) → … → **`0x5b1e0`**,
a decoder/display state-machine that calls the **video-decoder playmode getter
`0x6ef38`** (the `0x6f054/0x6f098` family, §earlier) and then the **24-tick bounded
mbox wait `0xa33d8`** on the CC event mbox `0x40033830`. It loops here forever: read
vdec playmode → not the awaited state → wait ≤24 ticks on the mbox → time out →
retry. `POWERONMENU_Initial` (0x61be8) is *not* on this stack — it is downstream and
never reached.

**Root cause, tied to PROC2.** The awaited transition is a **video-decoder playmode
state** driven by PROC2 (`bram[0xB0000190]`), and PROC2 is held in reset (§12.51), so
the live decoder never reaches the commanded state. The emulator's `bram[0x190]`
stand-in supplies command *acks* (machine.c:945/1140, `CT952_FORCE_PLAYMODE`) but the
value it presents does not satisfy this particular init-time poll, so `INITIAL_System`
never returns.

**Consequence chain (now complete, acceptance-test end to root):**
`INITIAL_System` decoder-state poll never satisfied → `Thread_CTKDVD` never returns
from init → `POWERONMENU_Initial` never called → `__bPOWERONMENUInitial` stays FALSE →
`OSDSS_Monitor` idle machinery never starts → the 20 s no-key screensaver/slideshow
can never arm. The ADC/no-key idle side is faithful (§12.55); the wall is this
**early decoder-playmode poll in system init**, gated on PROC2 + the CC mbox POST —
the same missing-producer frontier, now located at its *earliest* point.

**Actionable next lever:** determine the exact playmode value `0x5b1e0`/`0x6ef38`
polls for at init time, and make the `bram[0x190]` stand-in present it faithfully
(what a released PROC2 vdec would report) — candidate to let `INITIAL_System`
complete. New instrumentation: `CT952_PCWATCH`, `CT952_STACKW`, `CT952_PCSAMPLE`,
`CT952_WWATCH`.

### 12.57 BREAKTHROUGH — modeling the released-vdec idle playmode (0x86) advances the boot past the INITIAL_System decoder poll

Acting on §12.56: decoded the awaited value. The `INITIAL_System` decoder-sync poll
(flash `0x6f7f0`-`0x6f858`) does:
```
read REG_SRAM_PLAYMODE(bram[0xB0000190]);
if (== 0x86 MODE_RELEASE_MODE) write 0x10 MODE_STOP;
re-read; if (== 0x10) proceed  else retry forever
```
`0x86` = **`MODE_RELEASE_MODE`** and `0x10` = **`MODE_STOP`** (comdec.h `EN_VDEC_CMD`;
bram[0x190] = `REG_SRAM_PLAYMODE`, ctkav_vdec.h:367). A running PROC2 vdec posts its
idle playmode here; PROC2 is held in reset (§12.51) so the register stays `0x00`
(MODE_NONE) and the poll spins (confirmed: `CT952_PMTRACE` shows `b0000190=00` read at
`0x6f824`/`0x6f84c` every ~200k instr forever).

**Fix + result.** Added `CT952_VDEC_IDLE` (machine.c): present `MODE_RELEASE_MODE`
(0x86) for reads of an *uninitialized* (raw==0) `bram[0x190]` — the idle state a
released decoder reports — while respecting firmware writes (so the immediate re-read
of the commanded `0x10` succeeds). With it on, the poll's **success path `0x6f858` is
reached at 5.0M** (was never reached), and the boot thread **advances** from the
`0x5b1e0`/`0x5b264` decoder wait to a *new, later* handshake: stack now
`0xadc4→0x41a44→0x5a38c→0x61248→0x37400→0x38f74→0xa33d8`. First real forward motion in
the boot.

**New frontier (one hop further).** The `0x38f74` wait is gated on CC-subsystem state:
`*0x40023567==1`, `*0x40033274==0`, `*0x4003996c` bit `0x20`, `*0x40033304` vs `0x64`
— fields another producer (PROC2 completion or a sibling thread) must update. So this
is a **chain of PROC2/decoder-dependent init handshakes**, each waiting on state a
live decoder would post. `__bPOWERONMENUInitial` still 0 (POWERONMENU_Initial still
not reached), but the wall moved.

**Strategic fork (for direction):** either (a) keep modeling each posted decoder/CC
state value at each handshake (peel the onion), or (b) actually **release and run
PROC2** (cpu2) with the JPEG/decoder microcode so it drives every handshake
faithfully in one shot — the firmware stages PROC2 but never releases it (§12.51),
so the release trigger itself may be gated behind this same init, i.e. a chicken/egg
the emulator can break by seeding PROC2. New knob: `CT952_VDEC_IDLE`.

### 12.58 PIVOTAL — "PROC2" decoder is a DSP (own ISA), NOT the second SPARC; behavioral state-modeling is the faithful path, "run cpu2" is a dead end

Investigating whether to break the §12.57 handshake chain by actually running PROC2:
traced the release path and dumped the staged region.

**The firmware stages then HOLDS the decoder, never releases it (even at 55M with
VDEC_IDLE):** `CT952_P2FULL` shows at 4.94M it writes `R_PROC2_SP(0x800007d4)=0x4001cf00`,
`R_PROC2_START(0x800007d8)=0x40002000`, then `RESET_ENABLE(0x80000324) bit0 = HOLD`.
The **core-release `0x80000304` bit0 (and DSU2 `0x98000000`) is written 0 times** the
entire boot — later 0x304/0x324 writes carry bits 0x200000/0x40000/0x800000/0x300
(other subsystems: VPU/JPU), never bit0.

**The staged entry `0x40002000` is NOT SPARC code.** DRAM dump at 6M, region
`0x40002000..0x40010000` (14336 words): **0 `save`, 0 `call`, 0 `jmpl/ret`** — vs the
known PROC1 TEXT (`0x4001d000`) at 118/197/148. The bytes (`02e4056b 02c20593
04000400 04000400…`, repeated `04000400` tables) are DSP microcode/coefficients. This
is the "JPEG" section (`_ChangeDSPCode(HAL_VIDEO_JPG)`, hdecoder.c:569) — **DSP
decoder microcode, a different ISA**, staged into `REG_SRAM_*` space and driven via
`REG_SRAM_PLAYMODE` (bram[0xB0000190]).

**Conclusion — the fork is resolved:** the video/JPEG decoder is a **DSP/VPU block**,
not the second SPARC. The emulator's SPARC `cpu2` cannot execute `0x40002000`
(releasing it would run garbage — the §12.51 warning, now explained). So:
- "Run PROC2 (cpu2)" to drive the handshakes is **infeasible** (wrong ISA).
- The **faithful** model of an un-emulated DSP is its **observable interface**: the
  playmode/handshake state it posts (bram[0x190] + the CC/OSD status fields). The
  pixel decode is already covered by the picojpeg JPU stand-in; what remained
  unmodeled is the DSP's **playmode state machine**, which `CT952_VDEC_IDLE` (§12.57)
  began modeling. Continuing that — modeling each decoder-posted state the init
  handshakes expect — IS the correct faithful path, not a crutch.

Next: model the DSP playmode state machine through the §12.57 handshake chain
(`0x38f74` etc.) far enough for `INITIAL_System` to return and `Thread_CTKDVD` to
reach `POWERONMENU_Initial`, opening the idle-screensaver gate.

### 12.59 After the DSP-playmode unblock, the next wall is firmware display-state coordination (not more DSP) — display is partially alive

Continuing the §12.57/§12.58 faithful path: characterized the next handshake past the
decoder-playmode poll.

**The next wait (`0x38f74`) is firmware inter-thread coordination, not DSP behavior.**
With `CT952_VDEC_IDLE` on, the INITIAL thread (`0xadc4→…→0x37400→0x38f74`) spins in a
24-tick bounded CC-mbox wait, gated on a display state machine reaching state `0xd`
(which sets `0x4003996c` bit `0x20` via `0x5afd8`). `CT952_PCWATCH` shows the
flag-setter `0x5afd8` **never runs** (0 hits) — so the flag never sets and the wait
never clears. The status source feeding that state machine (`0x85978`) is a firmware
data-structure setter (writes a table at `0x4003ce2c`), **not a DSP/hardware
register** — so this handshake cannot be advanced by DSP-behavior modeling. It is the
firmware producer/coordination layer (the §12.49-§12.53 CC-mbox wall) reached one
layer deeper.

**The display block is partially alive.** The main display timing generator IS
enabled: `R_DISP_TGEN_TOTAL(0x80001a38) = 0x120d035a` (bit28 `DISP_TGEN_EN` set;
Vtotal=0x20d=525, Htotal=0x35a=858 — NTSC timing). Interrupt state:
`P1_1ST mask@0b0=fffffffe` (VSYNC bit0 unmasked), `pend@0b4=00000001` (VSYNC pending),
`LEON_MASK@090=00002d00` (level 13 / VSYNC enabled). So per §12.50 the emulator is
generating VSYNC fields and they are enabled/pending — the raster side is running —
yet the display *state machine* that would drive the handshake flag to `0xd` does not
advance, and `disp_state` still reads bit1 (STOPPED, 0x07).

**Status of the push:** `CT952_VDEC_IDLE` advanced the boot exactly one real,
faithful step (past the decoder-DSP playmode handshake). The next wall is NOT more
DSP state — it is the firmware display/CC producer that must drive the OSD/display
state machine and post the `0x40033830` mbox. Forcing the firmware flags directly
would be a crutch (forbidden). The concrete new attack point this opens: the display
field/VSYNC state machine — why, with TGEN enabled and VSYNC pending, the display
state machine (`[0x40039949]`→0xd) never advances. That is the next lever, distinct
from (and downstream of) the now-solved decoder-DSP handshake.

### 12.60 SYNTHESIS — the whole chain funnels into the display-STOP-at-9.9M / VSYNC-never-re-armed deadlock (§12.49), now proven

Tracing the frozen display sequencer (§12.59, `[0x40039949]` stuck at state 7) to its
root closes the loop between the decoder handshake (§12.57) and the display-restart
wall (§12.49):

**The display sequencer is VSYNC-driven, single-source.** The P1_1ST ISR
(interrupt.c:147) advances the display state machine **only** on
`INT_PROC1_1ST_VSYNC` (bit0) → `ISR_DISPSaveClearStatus()`. The SCREEN_END(bit2)/
MAIN_END(bit3)/OSD_END(bit4)/HSYNC(bit1) handlers are **empty** in this build. So the
sequencer advances one step per delivered VSYNC and by nothing else. (The emulator
faithfully raises only VSYNC(bit0)+HSYNC(bit1); it correctly does NOT raise the
end-of-region IRQs, which the firmware ignores anyway — so that is not the gap.)

**VSYNC is enabled, then killed by the display-STOP, and never re-armed**
(`CT952_VSMTRACE`, with VDEC_IDLE):
- `4.94M` pc=`0x3fac8` (display-ENABLE block `0x3fac0`): `MASK<-0xffffffff` — VSYNC ENABLED.
- `9.90M` pc=`0xa4218` (display-STOP routine `0xa41f0`, §12.49): `MDIS 0x1` — VSYNC CLEARED (mask→`0xfffffffe`).
- never re-enabled through 50M.

So the sequencer advances `0→7` during the enabled window (4.94M–9.9M), then **freezes
at 7 the instant the display is stopped**, because VSYNC — its only clock — stops
being delivered. `INITIAL_System`'s handshake (waiting for state `0xd`) therefore
hangs, `POWERONMENU_Initial` is never reached, and the idle screensaver never arms.

**Unified picture (acceptance test → root):** modeling the decoder-DSP idle playmode
(§12.57 `CT952_VDEC_IDLE`) removed the FIRST gate; the boot then advances to the
display bring-up, which **stops the display at 9.9M and cannot restart it** — the
exact §12.49 deadlock (display-ENABLE `0x3fac0` never re-runs; the restart is gated
behind the CC event loop that needs the `0x40033830` mbox POST that is itself gated
behind the stalled display). Every thread we have chased — decoder poll, OSD region
wait, screensaver gate — funnels into this single display-STOP/no-restart knot.

**Sharpened next lever:** WHY does the firmware STOP the display at 9.9M (pc `0xa4218`,
routine `0xa41f0`), and what would make it re-run the ENABLE block `0x3fac0`? If the
stop is a spurious/mis-triggered transition (emulator side), suppressing it is a
faithful fix; if it is a deliberate reconfigure-stop, the restart trigger is the
target. That single transition now gates the entire boot.

### 12.61 VSYNC-KEEP diagnostic — forcing continuous VSYNC clears the display-stopped state and advances the boot to a NEW thread loop (but not yet POWERONMENU)

Tested the §12.60 hypothesis directly with `CT952_VSYNC_KEEP` (machine.c): after each
VSYNC tick, force the P1_1ST VSYNC mask bit back on, so the sequencer keeps being
clocked past the 9.9M display-STOP that disables it. With `CT952_VDEC_IDLE` +
`CT952_VSYNC_KEEP`:
- `disp_state (0x4002401c)`: **7 → 0** — the display-STOPPED state clears (was stuck
  at 0x07 for the entire boot). Continuous VSYNC unwinds the stop.
- New code runs: a **thread main-loop at `0x41a90`** (calls `0x59e90`/`0x59850`/
  `0x69578`/`0x69510`, near thread-init `0x41b60`) becomes a hot loop (`0xdde98`
  sampled 40x) — **never executed before**. The boot advanced to a new frontier.
- Still gated: `POWERONMENU_Initial (0x61be8)` and `DISP_DisplayCtrl (0x4a754)` 0 hits;
  the `0x38f74` handshake's producer `0x5af60` still never runs; `[0x40039949]` still 7.

**Reading:** VSYNC delivery *is* part of the gate (forcing it demonstrably unwinds
the display-stop and starts a new thread), confirming §12.60 — but it is not the
*whole* gate. The boot is a **multi-layer chain of gated states**: `VDEC_IDLE` peeled
the decoder-playmode layer, `VSYNC_KEEP` peels the display-stop layer, and a further
thread-coordination layer (`0x41a90` loop, `0x5af60` producer) remains. Each faithful
(or diagnostic) model advances the boot one measurable layer and reveals the next.

**Caveat on faithfulness:** `VSYNC_KEEP` *overrides* the firmware's explicit VSYNC
disable, so it is a diagnostic probe, not a final fix. The faithful resolution is to
make the display **restart** (re-run the ENABLE block `0x3fac0`, which re-enables
VSYNC on its own) after the 9.9M mode-set stop — i.e. find/model whatever the restart
waits on — rather than pin VSYNC on. But the probe proves the causal chain: display
stop → sequencer freeze → init hang, and that unwinding it moves the boot forward.

**Open question for direction:** the boot is converging toward POWERONMENU one gated
layer at a time, but the number of remaining layers is not yet bounded. Each is a
distinct producer/handshake. New knobs: `CT952_VDEC_IDLE` (faithful), `CT952_VSYNC_KEEP`
(diagnostic).

### 12.62 COMMON ROOT (option-3 pass) — the CC-event *producer* thread (0x6748) is CREATED but NEVER SCHEDULED

Per the user's "find the common root under all the layers" direction: enumerated the
eCos threads by watching the create wrapper `0x596f4` (`[desc+4]` = entry, → eCos
`cyg_thread_create` `0x4001dd5c`). Across the advanced boot (VDEC_IDLE + VSYNC_KEEP,
to 70M) **exactly 4 threads are created**, and watching each entry PC shows which
actually execute:

| entry | role | created | RUNS? |
|---|---|---|---|
| `0xadb0` | main / INITIAL (Thread_CTKDVD path) | 4.94M | yes (4.94M) |
| `0x83098` | (parser/decoder helper) | 30.0M | yes (30.0M) |
| `0x7007c` | decoder thread (id-5 branch) | 33.6M | yes (33.6M) |
| **`0x6748`** | **CC-event worker (F_REQ/CC-mbox producer, §12.43)** | 30.0M | **NEVER** |

So the producer thread is not *missing* — it is **created and then never runs** (its
entry `0x6748` is never executed through 70M, while the 3 siblings created alongside
it do run). The worker body (`0x6748`) would `cyg_flag_wait` on `__fThreadInit`
(`0x40038f80`) then service the CC event queue and post `F_REQ`/the `0x40033830`
mbox — the exact producer every stalled handshake (§12.49-§12.61) is waiting on.
Consistent with this, `CT952_FREQTRACE` shows `F_REQ` only ever written `0x00000000`
(bit `0x80` never set) — because its producer never executes.

**This is the single common root under the whole layer stack:** decoder poll, display
sequencer, OSD handshake, screensaver gate — all wait (directly or transitively) on
events the `0x6748` worker would post, and it is created-but-unscheduled. eCos threads
start suspended; something must `cyg_thread_resume` the worker, and that resume never
happens (or happens then the worker never gets the CPU). `0x671c` (called near the
worker create at `0x11b24`) only *clears* the F_REQ/F_DONE flags — it is not the
resume.

**Next (option-1 faithful peel from the root):** find who is supposed to
`cyg_thread_resume` worker `0x6748` and why it doesn't fire (a gated resume, a missed
priority, or a create that returns a handle nobody resumes). That single resume, once
faithful, would start the producer and — per §12.60/§12.61 — cascade the whole boot
forward. Verified producer identity via §12.43 (`0x6748` = the F_REQ worker); confirmed
non-execution empirically here (`CT952_PCWATCH`).

### 12.63 The worker's resume EXISTS in firmware — it is sequenced AFTER a display/CC readiness call (0x5abb4) that never returns

Chased the missing `cyg_thread_resume` for worker `0x6748` (§12.62):

**The create has no resume (by design of that branch):** `INITIAL_ThreadInit(9)`
builds the worker descriptor with entry `0x6400|0x348 = 0x6748` and calls create
`0x596f4` at `0x41be4`, then `b,a 0x41d50` (bare ret) — **no resume in the id-9
branch** (unlike id-5/id-0xb which pair create+`0x596e0`). So the worker is created
suspended and resumed separately.

**The separate resume is real and located:** function `0x66dc` loads the worker handle
(`ld [0x40038fb0]`) and calls resume `0x596e0` at `0x670c`, also setting
`__fThreadInit` bit `0x80000` (idempotent guard). `0x66dc` has exactly one caller:
`0x41b04`, inside the mode-0 boot-init function at `0x41a88`.

**But `0x41b04` is never reached.** Empirically (VDEC_IDLE+VSYNC_KEEP, `CT952_PCWATCH`):
`0x41a90` executes **once**, and neither `0x41b04` (resume) nor `0x41b58` (its skip
branch) ever execute. The call at `0x41a90` — **`0x5abb4(7)`** — never returns; the
thread parks inside it. `0x5abb4` is a large CC/display readiness dispatch (checks
`0x4003274a`/`0x40032782`/state `[0x149]`, branches into the `0x5afxx` display state
machine) whose deep path performs the 24-tick `0xa33d8` CC-mbox waits (§12.56/12.61)
and loops without returning. So:

```
boot-init (0x41a88):  r = 0x5abb4(7)      <- display/CC readiness; NEVER returns
                      if (r) skip                (0x41b58)
                      else   resume worker (0x41b04 -> 0x66dc -> 0x670c)
```

**This is the deadlock made exact.** The CC-event worker (the producer of the very
mbox/F_REQ posts the whole boot waits on) is resumed **only after** `0x5abb4(7)`
returns — and `0x5abb4(7)` is stuck in the display/CC readiness loop that is itself
(transitively) waiting on what the worker would post. A genuine ordering knot at the
boot-init level: resume-after-readiness, readiness-needs-producer, producer-needs-resume.

**Why it works on silicon (hypothesis):** `0x5abb4`'s readiness loop is a *bounded*
poll that, on real hardware, sees the display/decoder reach ready (VSYNC-driven
sequencer completes, DSP posts state) and RETURNS, then the worker is resumed. In the
emulator the loop's ready-condition is never satisfied because the display/decoder
state it polls is exactly the un-modeled DSP/display behavior we have been peeling
(§12.57/12.60/12.61). So the faithful fix converges on the same target: model the
display/decoder readiness `0x5abb4(7)` polls for, so it returns and the resume fires.

**Next:** dissect `0x5abb4(7)`'s loop exit condition — the specific display/decoder
ready state it waits for — and model that (the continuation of the VDEC_IDLE/VSYNC
line, now with a concrete return-or-hang test at `0x41b04`).

### 12.64 PAYOFF TEST — resuming the worker is necessary and WORKS, but the worker is a CONSUMER of F_REQ; the innermost root is the F_REQ-bit-0x80 producer (§12.43)

Ran the decisive experiment (`CT952_SKIP_READY`): jump the boot-init straight to the
worker-resume `0x41b04` (bypassing the hung `0x5abb4(7)` and its `i0!=0` skip), so the
CC-event worker `0x6748` starts.

**Result — the resume genuinely works and matters:**
- `0x66dc` runs → resume `0x596e0` fires → **worker `0x6748` executes** (o7=eCos
  thread-entry `0x4001ea60`) — first time ever.
- `__fThreadInit`: `0x00000102` → **`0x00080102`** (USB/worker bit `0x80000` now set) —
  exactly the "natural boot" value recorded in earlier sessions. So the un-resumed
  worker (§12.62/12.63) was the reason `__fThreadInit` was short a bit under
  `--skip-panelcfg`.

**But the boot still does NOT reach POWERONMENU**, and `F_REQ (0x40026e9c)` bit `0x80`
is STILL never set (`CT952_FREQTRACE`: only `0x0` written; the sole post-resume write
is the worker's own `cyg_flag` setup at eCos `0x4001dfdc`). So:

**The worker is the F_REQ *consumer/dispatcher*, not its producer.** Its body
`cyg_flag_wait`s on `F_REQ` bit `0x80`; on that bit it runs dispatcher `0x6eec` →
`PostEvent 0x12f10` → re-posts the CC/OSD mbox `0x40033830` → CC loop cycles. It
*forwards* events; it does not originate bit `0x80`. Something else must **set F_REQ
bit 0x80**, and that producer never runs — the exact §12.43 frontier, now confirmed as
the innermost root beneath the whole stack.

**What bit 0x80 is:** F_REQ's low byte `0x40026e98` mirrors `MediaPresentPost`'s last
source index (setter `0x6130`), so bit `0x80` is a **media/source-present event**. The
producer is the media/source-detect path (§12.15) — it must post "source present" to
wake the worker, which then drives the CC/OSD event loop that unblocks display init,
which lets `INITIAL_System` return, which reaches `POWERONMENU_Initial`.

**Chain, fully mapped slideshow→root:**
OSDSS idle screensaver ← `__bPOWERONMENUInitial` ← `POWERONMENU_Initial` ←
`INITIAL_System` returns ← display sequencer ready (VSYNC) ← CC/OSD mbox posts ←
worker `0x6748` dispatches ← **F_REQ bit 0x80 set by media/source-present producer** ←
??? (the one remaining unknown). Every intermediate link is now identified and, where
hardware, faithfully modelable; the final unknown is the media/source-present post.

New knob: `CT952_SKIP_READY` (diagnostic — force worker resume).

### 12.65 ROOT REACHED — the innermost trigger is an initial "source-present" event; the emulator presents no media source

Traced F_REQ bit `0x80` (source 7; F_REQ bits are `1<<source`, MediaPresentPost posts
`1<<1`/`1<<2`) to the producer. Findings:

- **`0x65dc(mask)` sets F_REQ bit `mask` only if `__fThreadInit` bit `0x80000` (worker
  resumed) is set** — so event posting is gated on the worker being up (now satisfied
  by `SKIP_READY`, §12.64).
- **The media-*present* call is `0x6120`**: `MediaPresentPost(source, present=1)` (all
  other MediaPresentPost sites pass `present=0` = removal). Its setter `0x6108` has one
  caller `0x12ba4`, inside the source-event handler `0x12b30`, which is invoked from
  `0xd834`/`0xd8a0`/`0xdb4c`/`0x5a4c8`/`0x5f718` (the CC/source event fabric).
- With every crutch applied (VDEC_IDLE+VSYNC_KEEP+SKIP_READY) **MediaPresentPost is
  still never called with present=1** — no code path ever declares a source present.

**The root:** the whole event fabric is edge-triggered by an **initial
"source-present" event**, and nothing in the emulated boot ever raises one. On real
hardware that first edge comes from a **media-detect**: a card-insert (schematic
`SDCD#` pin 124 `GPC[13]/SDCD_N`, `MSINS#` pin 122), a USB attach, or a power-on scan
that finds the **internal SPI-flash photo store** as a source. The emulator models no
inserted card and no present source, so `MediaPresentPost(_, 1)` never fires → F_REQ
never gets its source bit → worker never dispatches → CC/OSD mbox never posts →
display sequencer never completes → `INITIAL_System` never returns →
`POWERONMENU_Initial` never runs → screensaver never arms.

**COMPLETE CHAIN, slideshow → root (every link identified this session):**
```
OSDSS slideshow (idle screensaver, ~20s)
 ← __bPOWERONMENUInitial=TRUE
 ← POWERONMENU_Initial() runs
 ← INITIAL_System() returns
 ← display sequencer reaches ready (VSYNC-clocked; §12.60/12.61 — CT952_VSYNC_KEEP)
 ← CC/OSD mbox 0x40033830 posted
 ← worker 0x6748 dispatches events (§12.62/12.63 — resume gated; CT952_SKIP_READY)
 ← F_REQ source bit set (needs worker up AND a source event)
 ← MediaPresentPost(source, present=1)          ← ★ NEVER FIRES
 ← a media/source-present trigger (card-detect SDCD#, USB, or internal-flash scan)
                                                  ← ★ THE ROOT: emulator presents no source
```
Plus the parallel decoder link (§12.57 — CT952_VDEC_IDLE, the DSP idle playmode),
which is faithful and already modeled.

**Faithful fix (the keystone):** model a **present media source** — most faithfully
the internal SPI-flash photo store the frame ships with (so it needs no card), or a
modeled inserted SD card via `SDCD#`. That single initial source-present edge should
post `MediaPresentPost(_,1)` → set F_REQ → wake the worker → cascade every link above
to the slideshow. This is the one remaining hardware/state input the emulator does not
provide; everything upstream of it is now identified and (where hardware) modeled.

### 12.66 Crutch result + user correction — media-present is NOT the screensaver blocker; crutched boot advances far but does not converge to POWERONMENU in 250M

**User correction (important):** a real photo frame boots and runs its built-in
slideshow with NO card inserted. So `MediaPresentPost(_,1)` never firing (§12.65) is
*normal*, not the blocker — the OSDSS screensaver reads internal JPEGs directly and
does not use the media-*source* fabric. The §12.65 media chain was a mis-attribution
for the screensaver path (it remains the correct chain for external-media playback,
just not for the built-in slideshow).

**Crutch experiment:** with all three diagnostic knobs — `CT952_VDEC_IDLE` (faithful
DSP idle playmode), `CT952_VSYNC_KEEP` (force VSYNC delivery), `CT952_SKIP_READY`
(force worker resume past the hung `0x5abb4(7)`) — the boot advances substantially:
`disp_state` 7→0 (display running), `__fThreadInit` 0x102→0x80102 (worker up), and it
runs new display/decoder code (`0x6f8xx` result-ring consumer, `0xa8xxx`). But a long
run to **icount 250,000,000 never reaches `POWERONMENU_Initial` (0x61be8)** — final
pc `0x62528`, still cycling in CC/event loops. The crutches are NOT sufficient.

**Remaining gate:** the boot cycles in display/decoder *ring-consumer* loops
(`0x6f8a0`: compare write/read ptrs `0x40039d8c`/`0x40039efc`, return when empty) that
poll for producer data. The producer is the decoder DSP result path, which is
un-modeled beyond the picojpeg pixel stand-in — so the ring stays empty and the
consumer never advances the state machine that would let `INITIAL_System` return.

**Also reconsidered:** `0x5abb4(7)` (whose hang forced `SKIP_READY`) waits on **source
7** readiness — and F_REQ bit 0x80 == source 7. So source 7 is likely the internal
display/photo source, which on real hardware is posted *present* at power-on (always
there). The faithful unblock may be to post the internal source (source 7) present at
boot so `0x5abb4(7)` returns naturally and resumes the worker — but the initial
source-scan that would do this appears gated behind the same stalled init, and
`SKIP_READY` (which bypasses `0x5abb4` entirely) may skip setup needed downstream,
which is why the crutched boot does not converge.

**Honest status:** the full dependency chain is mapped and every link identified; three
faithful/diagnostic models advance the boot from "dead at INITIAL_System" to "display
up, worker up, cycling in decoder-ring loops." The finish line (rendered slideshow) is
NOT reached — the remaining work is modeling the **decoder DSP result-ring producer**
(the state the ring consumers poll for), the deepest layer of the DSP behavioral model
(§12.58). That, not media-present, is the last gate.

### 12.67 DSP-model groundwork — SRAM interface mapped; the crutched stall does NOT poll the DSP (model must be built with crutches OFF, at 0x5abb4)

Started the absolute DSP model. Mapped the PROC1↔decoder SRAM interface (ctkav_vdec.h,
`REG_SRAM_BASE = 0xB0000000`):
- `0x190` **PLAYMODE** (BYTE) — command/state (already stood-in by `CT952_VDEC_IDLE`).
- `0x194` **WATCHDOG** (DWORD) — **written by PROC1** (`REG_SRAM_WATCHDOG++` in cc.c:843/
  1270, mm_play.c:2739, monitor.c:674; monitor.c:1575 checks it changed). It is a
  *firmware* self-liveness counter, NOT a DSP-posted signal. Do not model it as DSP.
- `0x198` **DISPLINE** (DWORD) — display line.
- `0x00-0x18F` — decoder scratch (MP4/MPG state vars), PROC1-managed.

**Empirical finding (new `CT952_DSPTRACE` = log distinct bram reads past an icount):**
with `VDEC_IDLE`+`VSYNC_KEEP`+`SKIP_READY`, the boot performs **no bram/SRAM reads at
the stall** (50M+). So the current *livelock* is not a DSP-register poll — `VDEC_IDLE`
already covers the one decoder handshake (the `0x190` playmode poll at 0x6f820), and
what remains is firmware coordination that `SKIP_READY` disrupts by skipping the
`0x41a98`-`0x41b04` setup.

**Consequence for the model:** the place the DSP model actually matters is `0x5abb4(7)`
(§12.63) — the readiness dispatch whose hang forced `SKIP_READY`. But `SKIP_READY`
*bypasses* `0x5abb4` entirely, hiding what it polls. So the absolute DSP model must be
built with the crutch OFF: run `VDEC_IDLE`+`VSYNC_KEEP` only, let the thread park in
`0x5abb4`'s call tree (`0x5a38c→0x61248→0x37400→0x38f74→0xa33d8`, §12.56), and model
exactly the decoder/display state that path polls so `0x5abb4(7)` returns *naturally*
— which resumes the worker with its setup intact (no livelock). That, not a broad SRAM
model, is the bounded target.

**Status:** SRAM interface documented; DSP watchdog correctly excluded (PROC1-owned);
`CT952_DSPTRACE` added. The absolute DSP model is a substantial, well-scoped next
effort centered on the `0x5abb4(7)` readiness poll (crutches off), NOT the media
fabric and NOT the PROC1 watchdog. New knob: `CT952_DSPTRACE`.

### 12.71 MAJOR ADVANCE — clock speedup brings the event system ALIVE (deadlock was largely time-starvation); boot plateaus at DISPSTATE=7 / pre-POWERONMENU

Building on §12.70: with `CT952_TICK_FAST_AT=48000000,512` (eCos clock 512× after the
early gates) + VDEC_IDLE + VSYNC_KEEP, the late boot comes ALIVE:
- Boot-init completes its `OS_DelayTime`s and **reaches the worker-resume `0x41b04`
  naturally** (54.8M); **worker `0x6748` runs** (54.86M).
- **`F_REQ`/`F_DONE` cycle** (`CT952_FREQTRACE`): F_REQ bit `0x1000` (per-frame redraw)
  at 55.9M, bit `0x04000000` at 60.4M, with matching F_DONE — the CC-event worker is
  **dispatching events**. `MediaPresentPost` is called. The producer/consumer loop we
  spent §12.49-69 thinking was structurally deadlocked is **running** — it was
  time-starved (every firmware delay ate ~133K instr/ms, so test runs died mid-delay).

**But the boot does NOT reach `POWERONMENU_Initial`** through 400M instructions:
`__bPOWERONMENUInitial` (0x40023a10) never set; `POWERONMENU_Initial` (0x61be8) never
called; **`DISPSTATE` (0x40039949) frozen at 7** (`CT952_WWATCH`). So with timing and
the event loop no longer the issue, the remaining gate is the **display state machine
stuck at event 7** (§12.68: `0x5abb4` only ever receives event 7) and `Thread_CTKDVD`
not advancing from `INITIAL_System` to `POWERONMENU_Initial`.

**Reframed status (honest, measured):** the great majority of the "circular deadlock"
was **time-starvation of legitimate firmware delays** — a huge simplification. The
event fabric, worker, and F_REQ producer all work once given a runnable clock. What
REMAINS is a genuine gate: the display state machine never advances past event 7, so
`Thread_CTKDVD` never reaches the power-on menu. That — not the mbox producer, not the
media fabric, not the DSP registers — is the last wall, and it is bounded to
`0x5abb4`'s event-7 handling and who would send the next display event.

**Next:** with clock+events alive, trace why `0x5abb4` never receives event >7 (the
display-advance producer) — and try milder clock multipliers (512× may over-compress
and derail the display sequencer). New: confirmed time-starvation as the dominant
cause; event system verified alive via F_REQ cycling.

### 12.73 Close-the-gaps loop confirmed; boot advances to ~82M then parks at a new mbox wait (frontier)

With all faithful/diagnostic fixes (VDEC_IDLE + VSYNC_KEEP + TICK_FAST_AT=...,512 +
IIC 0x420c), the boot advances far past everything prior:
- config-read poll (§12.72) cleared (0x624f4 no longer hot);
- event system alive (§12.71): worker runs, F_REQ/F_DONE cycle;
- runs to ~82M then **hard-parks** (PC histogram: only eCos idle/scheduler past 82M,
  zero firmware PCs -- all threads suspended).

**The parking waits (new instrumentation `CT952_FLAGTRACE`, `CT952_MBOXTRACE` from
54M):** no `cyg_flag_wait` (4001dffc) past 54M; instead **3 threads poll mbox-gets**
around 55M then go idle: caller `0x841f4` obj `0x4003cc00`, caller `0x70240` obj `1`,
caller `0x59864` obj varies. They poll briefly then block -- the boot settles into a
suspended state waiting on one of these mbox objects (esp. `0x4003cc00`) that no
producer posts once the early activity drains.

**Reframe holds:** the boot is a **close-the-gaps sequence**, not a wall. Each fix
(decoder playmode, VSYNC, eCos clock, IIC status bit) closed a real, bounded gap and
advanced the boot; the next is another such gap -- a producer for the `0x4003cc00`
mbox (or the internal-source-present edge, still the leading candidate). The finish
line (POWERONMENU → slideshow) is not reached, but the path is now incremental and
measured, not a mystery.

**Turn summary (major):** overturned the "structural deadlock" model -- most of it was
**time-starvation** of legitimate firmware delays (eCos clock ~133K instr/ms). With a
sped clock the event fabric, worker, and F_REQ producer all run. Closed the IIC
config-read gap (§12.72). New instrumentation: `CT952_CALLTRAIL`, `CT952_FLAGTRACE`.
Remaining frontier: the `0x4003cc00` mbox producer / internal-source-present.

### 12.74 RAM dump (user suggestion) confirms NO source present; source-detect never runs and is NOT indirectly dispatched

Acting on the user's two suggestions — (1) dump RAM to catch ROM/DRAM changes, (2)
consider interprocedural + indirect dispatch as why the producer is unfindable.

**RAM dump at 55M (all fixes, event system alive):**
- `F_REQ 0x40026e9c = 0`, `F_DONE = 0` (transient bits cycle, 0 at snapshot).
- **CC mbox `0x40033830` message-count `[+0x3c] = 0`** — empty; the boot-init pollers
  (`0x841f4`/`0x70240`) wait on it.
- `__fThreadInit = 0x80102` (worker up), `__bPOWERONMENUInitial = 0`.
- **Per-source presence table `0x40039b08` (stride 0x64, present flag +0x30): ALL
  sources present=0**, including slot 7. Only slot 0 has state `0x30`. So NOTHING is
  marked present — the user's "no source present" reading is confirmed in memory.

**Source-detect never runs:** `CT952_PCWATCH` on the source-event handler `0x12b30`
and the present=1 path `0x6120` — **0 hits**. Only `0x60bc` (MediaPresentPost source-0
*removal*, present=0) fires (3×). So no source-present is ever posted.

**Indirect-dispatch check (user's point):** searched BOTH flash and the DRAM dump for
function-pointer words to `0x12b30`/`0x6108`/`0x6120` and the direct callers
(`0xd834`/`0xd8a0`/`0xdb4c`/`0x5a4c8`/`0x5f718`) — **none**. So the source-detect is
NOT reached via an indirect/table dispatch; it is called directly from sites that
simply never execute. The producer isn't hidden by indirection — the **source-scan
that would call it never runs**.

**ROM/DRAM code check:** flash is XIP/read-only in the model (writes ignored), so ROM
cannot be self-modified; eCos + relocated sections live in DRAM but the poller/producer
functions run XIP from flash (addresses < 0x200000), matching the disassembly — no
patched-code surprise.

**Frontier (confirmed):** the boot reaches "event system alive, worker dispatching"
but no source-present is ever declared, so the CC-mbox event that would carry
`Thread_CTKDVD` to `POWERONMENU_Initial` is never posted. The next step is the
**source-scan**: find the boot-time media scan that should call `0x12b30` to declare
the internal source (and/or the "no source found" path a card-less frame takes to the
menu), and why it never executes. New: RAM-dump analysis method; indirect-dispatch
ruled out for the source-detect.

### 12.75 USB HID keyboard — bare-metal EHCI driver, enumerated + polled from Python

Having faithfully modeled the hardware, the branch's next milestone is a **USB HID
keyboard driver** written against the modeled EHCI host controller and driven entirely
from MicroPython on the device.

**Emulator side (`jupiter/emu/machine.c`, `machine.h`).** The EHCI model (base
`0xA0000100`) previously exposed only its register file (capability + operational regs,
an empty root hub) so the retail boot's USB enumeration completes with zero devices.
This adds the missing execution engine and a device:

- **Async-schedule executor** (`ehci_run_async`): walks Queue Heads from
  `ASYNCLISTADDR`, and for each QH advances its qTD overlay chain (`QH+0x10` Next-qTD
  pointer), running each active qTD by PID — SETUP (parse the 8-byte USB control
  request), IN (return staged descriptor / control data, or an interrupt-IN HID
  report), OUT (accept the status/data stage). Status is written back to the qTD token
  (Active cleared, residual byte-count in bits[30:16]); IOC raises `USBSTS.USBINT`. A
  NAK (interrupt IN with no queued report) leaves the qTD Active so the driver's next
  poll retries — exactly the real device behaviour. The executor is invoked on a coarse
  cadence from `machine_cycle` (every 1024 cycles) while `USBCMD.RS` and `USBCMD.ASE`
  are set, modelling the controller running the ring in the background.
- **HID boot keyboard device**: standard descriptors (device VID `0xCEEB` / PID
  `0x0952`, one configuration, one HID boot-protocol interface, one interrupt-IN
  endpoint `0x81`), the 63-byte boot-keyboard HID report descriptor, and string
  descriptors. Control requests handled: `GET_DESCRIPTOR` (device/config/HID-report/
  string), `SET_ADDRESS` (adopted at the status-stage IN, per spec), `SET_CONFIGURATION`,
  `GET_CONFIGURATION`, `GET_STATUS`, and the HID class `SET_IDLE` / `SET_PROTOCOL` /
  `GET_REPORT`. The interrupt endpoint streams 8-byte boot reports
  (`[modifiers, 0, key1..key6]`) from a queue.
- **PORTSC handshake**: when a device is attached (opt-in), port 0 reports connect
  (CCS+CSC), and the port-reset→high-speed-enable sequence sets PED when the driver
  clears Port Reset.
- **Host key feed** (`machine_usb_kbd_feed` / `CT952_USB_KEYS="..."`): converts an
  ASCII string to HID usage codes (with the shift modifier for uppercase/symbols) and
  queues a key-down + key-up report per character. **Opt-in only** — with no key source
  the root hub stays empty, so the retail boot's enumeration is unchanged.

Modeling choices, documented in-source: the schedule structures (QH/qTD) are read/written
**CPU-native big-endian** on this SoC (so the driver needs no byte-swaps), while USB
*descriptors* keep their spec little-endian byte order. The interrupt endpoint is serviced
via the async schedule rather than a separate periodic-schedule frame list; since both the
controller and the device are modeled here, this is internally consistent and keeps the
driver small. Buffer pointers are followed linearly (no 4 KB page-split), which is
transparent for the small control/interrupt transfers used here.

**Device side (`jupiter/mpy/modusb_kbd.c`).** A real, if minimal, bare-metal EHCI driver
exposed as the MicroPython `usb_kbd` module:

- `usb_kbd.init()` — HCRESET, `CONFIGFLAG=1`, run; reset the root-hub port and confirm
  PED; then enumerate over the async schedule with standard control transfers
  (GET device descriptor → SET_ADDRESS 1 → GET config descriptor → SET_CONFIGURATION →
  HID SET_PROTOCOL boot / SET_IDLE). Builds the QH + SETUP/DATA/STATUS qTDs in DRAM
  (`.bss`) with ordinary 32-bit stores and polls the qTD Active bit for completion.
- `usb_kbd.poll()` — one interrupt-IN transaction; returns the 8-byte report as `bytes`,
  or `None` on NAK.
- `usb_kbd.getchar()` — polls until a key-down report with a printable usage, mapping the
  HID usage + shift modifier to an ASCII `str` (US layout).

**Result (verified).** `CT952_USB_KEYS="hello world"` →

```
usb_kbd: device VID=ceeb PID=0952 class=0 MPS0=64
usb_kbd: configured, polling ep 0x81
usb_kbd typed: hello world
```

`CT952_USB_KEYS="CT952-DVD!"` round-trips uppercase (shift modifier) and `!` (shift+1)
correctly; with no `CT952_USB_KEYS`, `init()` returns False and the demo prints
`usb_kbd: no keyboard on port 0` — the retail boot path is untouched. A full USB stack —
controller reset, port reset, control-transfer enumeration, and HID interrupt polling —
now runs on the emulated CT952, written in Python on the device.

### 12.76 Boot-to-REPL — a live Python prompt driven by the USB keyboard

The branch's goal reached: the MicroPython port now **boots to an interactive
REPL** whose stdin is the USB HID keyboard (and UART1).

- `mp_hal_stdin_rx_chr` (`uart_core.c`) now polls both sources: a non-blocking
  `usb_kbd_c_getchar()` (one short interrupt-IN poll on the HID endpoint,
  returning the decoded ASCII of a key-down or -1) and UART1 RX, returning
  whichever produces a character first.
- `mpy_main` (`main.c`) brings up the keyboard (`usb_kbd_bringup()`, the C entry
  the Python `usb_kbd.init()` also uses), runs a one-line startup
  (`import ct952; ct952.init()`), then loops on MicroPython's
  `pyexec_friendly_repl()` — the real friendly REPL (readline line editing,
  multi-line continuation, expression auto-print).
- Build: added `shared/runtime/pyexec.c` + `shared/readline/readline.c`;
  `readline.c` goes in `SRC_QSTR` so its `MP_REGISTER_ROOT_POINTER(readline_hist)`
  is collected into `genhdr/root_pointers.h`. readline pulls `snprintf`
  (`shared/libc/printf.c`), and `-U_FORTIFY_SOURCE` stops the sparc64 headers
  redirecting it to the unavailable `__snprintf_chk`.

**Result (verified), feeding `CT952_USB_KEYS="print(2+2)\nimport ct952\nct952.fill(11)\n2**16\n"`:**

```
usb_kbd: device VID=ceeb PID=0952 class=0 MPS0=64
usb_kbd: configured, polling ep 0x81
[ct952] USB keyboard ready -- type Python below
MicroPython v1.29.0-preview on 2026-07-28; ct952 with sparc-v8-be
>>> print(2+2)
4
>>> import ct952
>>> ct952.fill(11)
>>> 2**16
65536
```

Every keystroke traversed the full stack — HID boot report → EHCI async schedule
→ bare-metal driver → REPL stdin — and `ct952.fill(11)` repainted the OSD plane
live. A DVD-player SoC, emulated from its own firmware, now takes typed Python at
a `>>>` prompt over a USB keyboard.

### 12.77 Bind the rest of the player into `ct952` — GPU, JPU, IR, raw registers

With the REPL live, the `ct952` module grew from a framebuffer poker into a
binding for the emulator's actual peripheral models, so Python drives the real
blocks:

- **GPU 2-D engine** (`gpu_fill`, `text`). The shared JPU/GPU block at
  `0x80002880` is programmed and kicked; `gpu_exec()` in the emulator writes the
  pixels. `gpu_fill(x,y,w,h,color)` issues fill-rectangle ops (opmode 6),
  computing `AG_OFF` so the plane pitch stays 480 bytes (`ag_offset = 61 -
  ag_width`, valid to ~244 px wide; wider fills are tiled). `text(x,y,s,fg,bg)`
  drives the 1-bit font path: a built-in 8x8 font (public-domain font8x8_basic)
  is expanded once into a DRAM glyph table (MSB-first, one DW/row), then each
  glyph index is pushed to `FONT_IDX` and a font op is kicked.
- **JPU decoder** (`decode_jpeg(addr)`). Writes the MCU-BIU bitstream source
  (`0x80002a20`) and runs a JPU op (`CTL0[28]=0`), so the functional picojpeg
  decode reconstructs the frame onto the video plane the display composites.
- **IR remote** (`ir_poll()`). Reads `IR_DATA` (`0x80000390`), returning the
  scancode of a pending key (and consuming it) — the input the emulator's
  `CT952_IRKEY`/`CT952_IRKEYS` injector feeds through the HW-NEC decoder path.
- **Raw registers** (`peek32`, `poke32`, `poke_bytes`). Full 32-bit access to
  any modeled register or memory, plus a bytes-to-DRAM copy for staging data
  (e.g. a JPEG before `decode_jpeg`).

**Verified (REPL session over the USB keyboard):**

```
>>> ct952.gpu_fill(20,20,220,60,1)
>>> ct952.text(30,100,'HELLO CT952',9,10)
>>> print(ct952.peek32(0x80001a54))
284426208            # 0x10f001e0: OSD enable | 240<<16 | 480
```

`--fb-out` (with `CT952_OSD_FULL=1`) shows the exact result: a 220x60 red
rectangle (13200 px) laid down by the 2-D engine and "HELLO CT952" rendered
white-on-navy by the GPU font engine. Two findings worth noting: (1) the USB
key feed was refactored from a fixed 256-entry report ring to a lazily-generated
report stream over an ASCII buffer, after the ring overflowed at ~127 characters
and stalled input mid-session; (2) the OSD `--fb-out` composite clamps at row
~51 (`0x40065000`) unless `CT952_OSD_FULL=1`, which had hidden the lower rect
rows and the text until the flag was set.

The player is now scriptable end to end from a `>>>` prompt: display plane,
palette, 2-D blitter, font engine, JPEG decoder, IR remote, and arbitrary
registers — all from Python typed on a USB keyboard.

### 12.78 The last peripherals — SD host controller + panel keys in `ct952`

Completing the "all hardware" pass: the two remaining modeled peripherals are
now bound, so every block ct952emu models is reachable from Python.

- **SD host controller** (`sd_present`, `sd_init`, `sd_read`). A minimal but real
  bare-metal SD driver against the SDHC model (base `0xA0001100`): it runs the
  standard init handshake (CMD0 / CMD8 / ACMD41 until ready / CMD2 / CMD3 /
  CMD7) then issues CMD18 multi-block reads, with the controller DMAing 512-byte
  blocks from the inserted FAT card image into a DRAM buffer. `sd_read(lba,n)`
  returns the blocks as `bytes`.
- **Panel keys** (`panel_adc`). Reads the analog key-ladder ADC at `0x8000407C`:
  it selects a ladder line (writes bits [23:16]) then returns the voltage byte
  [31:24] -- the input the emulator's CT952_PANELKEY / CT952_ADC drives.

Also enabled `MICROPY_PY_BUILTINS_SLICE` so byte buffers slice at the REPL
(`b[0:11]`); indexing worked at the minimum ROM level but slice syntax was
compiled out, which had shown up as a `SyntaxError` on the `:`.

**Verified (REPL over the USB keyboard, `CT952_SDCARD=<fat.img>`):**

```
>>> ct952.sd_init()
True
>>> b = ct952.sd_read(0, 1)
>>> print(b[0:11])
b'\xeb<\x90MSDOS5.0'          # the card's real FAT boot sector, read by DMA
>>> print(999, ct952.panel_adc(0x84))
999 119                       # key-ladder ADC voltage (0x77)
```

The whole modeled machine is now scriptable from a `>>>` prompt over a USB
keyboard: display/OSD, palette, GPU 2-D blitter, GPU font engine, JPU JPEG
decoder, SD host controller, IR remote, panel key ladder, USB, UART, and
arbitrary registers/memory. Every peripheral ct952emu models has a Python
binding.

### 12.79 Python AS a firmware app — an on-device live debugger

The payoff of the MicroPython work: package the interpreter as an app and run it
**inside the booted firmware**, in place of a firmware app, so Python becomes an
interactive on-device console over the *live* firmware state.

**How it works.**
- **Free DRAM window.** After a firmware boot the region `0x40500000..0x40800000`
  (3 MB) is 100% zero -- only transient decompress scratch used it during boot.
  A DRAM-resident MicroPython payload lives there without touching eCos or the
  firmware working set.
- **Embedded build** (`make APP=1 BUILD=build-app`, `ct952_app.ld` +
  `start_app.S`): everything (code/rodata/data/bss/heap/stack) links into that
  window. `--build-id=none -N` pins `.text` to the window base so the raw `.bin`
  loads 1:1. The app installs its OWN trap table (`%tbr`) with the register-
  window overflow/underflow + `ST_FLUSH_WINDOWS` handlers -- riding eCos's
  handlers faulted (`trap 0x05 with ET=0`), because they are coupled to eCos
  thread context. Safe to own the traps: the app runs with interrupts masked
  (PIL=15) and never returns, so eCos's traps are dormant while Python holds
  PROC1.
- **Injection + seize** (`CT952_PYAPP=<payload>` [`CT952_PYAPP_AT=<icount>` |
  `CT952_PYAPP_HOOK=<pc>`]): the emulator DMA-loads the payload at `0x40500000`,
  then at the chosen point seizes PROC1 -- sets PC to the window base, a fresh
  `%sp`, and PIL=15 -- exactly the mechanism by which the firmware would hand off
  to an app's entry (`CT952_PYAPP_HOOK` = the replaced app's entry PC).
- **Live-firmware bindings.** `ct952.peek32/poke32/poke_bytes` read/write any
  live address; `ct952.call(addr, *args)` calls a firmware function directly
  (args in `%o0..%o3`, result from `%o0`), since Python shares the firmware's
  address space and ABI.

**Verified (REPL over the USB keyboard, real `dp700wd.bin` booted then seized):**

```
[PYAPP] launched Python app (seized PROC1 -> 0x40500000) at icount=40000xxx
[pyapp] MicroPython launched inside the firmware
[pyapp] the firmware is live: ct952.peek32/poke32/call
>>> print(hex(ct952.peek32(0x40000000)))
0xa7580000                         # live: the firmware's loaded ROMV code
>>> ct952.call(0xd3900, 0x40780000, 0x40000000, 8)   # the firmware's own memcpy
1081606144                         # returns dst (0x40780000)
>>> print(hex(ct952.peek32(0x40780000)), hex(ct952.peek32(0x40000000)))
0xa7580000 0xa7580000              # memcpy ran: scratch now equals the source
```

Two gotchas fixed: `gc_collect` now issues `ta 3` to flush register windows
before the stack scan (else a collection deep in the VM frees in-register roots
-> `VERIFY_PTR` heap corruption); and `pyapp_main` imports `ct952` up front so
the REPL name is bound.

**Does this help knock out the remaining unknowns? Yes -- decisively.** The open
questions (why the boot parks before `POWERONMENU`, why the source-detect never
runs, which gate the CC-mbox event waits on) have been chased with static
analysis + env-gated emulator probes. On-device Python replaces that with
*interactive* experiments against the running firmware:
- `peek32` any suspect variable live (`__bISRKey` 0x40039074, `__bPOWERONMENU
  Initial` 0x40023a10, DISPSTATE, the CC event flag 0x40026ea4, thread/mbox
  state) instead of inferring it.
- `poke32` a gate and watch the firmware's own threads react (the probes we
  hard-coded in C become one-liners typed at a prompt).
- `ct952.call` the firmware's OWN functions -- call the source-detect
  (`0x12b30`), `POWERONMENU_Initial` (`0x4b808`), an event post, etc. -- and read
  the resulting state, turning "why doesn't X run / what would X do" from a
  disassembly question into a direct call. `call`-ing `memcpy` already proves
  arbitrary firmware routines execute and return correctly from the prompt.

The emulator becomes an interactive firmware lab: seize -> poke/peek/call ->
observe, with no rebuild between experiments.

### 12.80 On-device debugger applied: park confirmed live; source-detect ruled out

Using the embedded Python app (§12.79) as a live probe, seized into the running
`dp700wd.bin` at 100M (well past the ~82M park) and inspected/called the
firmware directly.

**Live park state (confirmed on the running firmware, not a RAM dump):**
```
>>> print(ct952.peek32(0x4003cc3c), ct952.peek32(0x4003386c), ct952.peek32(0x40023a10))
0 0 0        # CC mbox 0x4003cc00 count=0, CC mbox 0x40033830 count=0, __bPOWERONMENUInitial=0
```
Both CC mailboxes are empty and the power-on-menu flag is unset -- exactly the
§12.74 RAM-dump picture, now verified on the live system: the waiting CC threads
never receive a message, so `Thread_CTKDVD` never advances to `POWERONMENU`.

**Source-detect 0x12b30 ruled out as the producer:**
```
>>> print(ct952.call(0x12b30,0,0,0), ct952.peek32(0x40023a10), ct952.peek32(0x4003cc3c))
0 0 0        # returns 0; neither the flag nor the mbox count changes
```
Disassembly confirms why: `0x12b30(idx)` is a per-source *descriptor-setup*
helper -- it indexes a 52-byte-stride table at `0x40039b08` by its argument and
writes descriptor fields (size/addr/flags 0x202). It is NOT the media/source-
*present* event producer. Calling it changes no CC state, so "just call source-
detect" is not the fix.

**Frontier refined.** `0x4003cc00` is not a lone mailbox but the **CC-application
state block**, referenced base+offset across the entire CC code region
(0x83000..0xa2000, 273 sites). The missing step is the higher-level media/source-
*present* event (the §12.65 "initial source-present" root), which would enqueue
into the CC mailbox and wake the parked threads. The debugger has now (a) proven
the park state live, (b) eliminated the low-level source-detect helper, and (c)
localized the producer to the CC media-present path rather than the descriptor
layer.

**Method note.** Each probe is a one-liner typed at the on-device REPL against
the live firmware -- `peek` a variable, `call` a firmware routine, read the
result. The seize freezes PROC1 (so we observe a frozen snapshot + call effects,
not thread resumption); a non-freezing variant (run the app on PROC2 alongside
the firmware) is the next tool for watching threads react to a poke in real time.

### 12.81 Reflash without decapping -- the firmware self-programs its flash

Scan of `dp700wd.bin` for a software reflash path (no chip access needed):

- **Serial-flash program/erase driver** at flash ~`0x3ce34..0x3d1cc` (XIP),
  printing `serial Flash Cycle`, `Erase sector(64K)`, `Erase sector(4K)`,
  `Flash ad[%lx,%lx,%lx]`. So the running firmware can erase + program its own
  SPI/PROM NOR flash (this is what settings-persistence uses).
- **Two-stage boot + auto-upgrade** in the boot region `~0x3000..0x4400`:
  `auto-upgrade code: %lx,now: %lx`, `AP_Loader() fail. So, re-booting`,
  `Unable to get valid boot block!`, `Boot fail flash, this Boot is %hd` -- a
  boot loader validates a boot block and can auto-upgrade the application image.
- **Media sector I/O**: `Card ReadSector`/`Card WriteSector`, `SectorCount`.

So a non-destructive reflash is feasible two ways: (1) the firmware's own
upgrade path -- present an update image on media and let the AP loader reprogram
flash (safest; the `auto-upgrade` check validates chip/version); (2) in-
application programming via the on-device debugger -- `ct952.call` the erase
(`~0x3ce34/0x3ce60`) then the program routine to write a new image to flash,
straight from the REPL. Both avoid decapping the part. (Reflashing is
brick-risky: verify the erase/program signatures and keep the boot loader's
recovery/boot-block path intact before writing.)

### 12.82 Concurrent (PROC2) debugger: watch the firmware run live

Built a non-freezing variant of the on-device debugger: the Python payload runs
on **PROC2** while the firmware keeps running on **PROC1**, so we observe and
poke the live system without pausing it.

- Emulator: `CT952_PYAPP_PROC2=1` starts the payload on cpu2 (own trap table,
  supervisor PSR) and leaves cpu1 running; `CT952_PYAPP_SCRIPT=<file>` stages an
  experiment script at 0x40740000 ("PYSC" header) that the payload runs instead
  of the REPL (no keyboard needed for the second core). A diagnostic register
  exposes PROC1's live PC/nPC to PROC2 at 0x98080020 / 0x98080024.
- Fix that unblocked it: `ct952.peek32/poke32/call` extracted the address with
  `mp_obj_get_int` (signed), so any address >= 2^31 -- i.e. the whole I/O map
  (0x80000000 / 0xa0000000 / 0x98000000) -- overflowed and returned nil. Switched
  to `mp_obj_get_int_truncated` (raw 32-bit pattern). Every low-address peek had
  worked by luck; high-address peeks now work too.

**Result (script on PROC2, firmware live on PROC1, seized at 100M):**
```
PROC2 watching PROC1 execute (pc, npc):
0 0x40001044 0x4001e6d8 0
1 0x4001d090 0x400010b0 0
2 0x4001d80c 0x00070268 0
...                          # PROC1's PC moves every sample -> genuinely concurrent
9 0x00059848 0x40001058 0    # __bPOWERONMENUInitial stays 0 throughout
```
PROC1's PC cycles through `0x40001040..0x400010b0` (eCos scheduler/dispatch) and
`0x4001d000..0x4001e708` (the poll-thread bodies) -- the §12.73 park watched
live: the threads spin on their mbox-gets and the scheduler round-robins, with
nothing ever posting the source-present message.

This is the tool the park needs: poke a candidate source-present gate on PROC2
and watch PROC1's PC in real time -- if it leaves the `0x4001dxxx` poll region
for the POWERONMENU path, that gate is the producer. (Caveat: calling firmware
*functions* from PROC2 while PROC1 also runs them races on unlocked state; peek/
poke of data is the safe concurrent primitive.)

### 12.83 Park-crack via live experiments: it's a blocked kernel-wait, not a poll

Drove the park investigation with the on-device debugger (concurrent sweep on
PROC2 + a seize-mode function call). Two decisive results.

**1. Poke sweep is a clean negative.** On PROC2, while PROC1 ran, poked the three
"polled-guard" candidates and sampled PROC1's live PC (0x98080020) after each:
- page-8 advance guard `byte[0x40022f81]=1`
- CC event flag `0x40026ea4 |= 0x1000`
- F_REQ source-present bits `0x40026e9c |= 0x86`

In every case PROC1 stayed in the eCos idle/scheduler + blocked-thread region
(`0x40000054..0x400010b0` and `0x4001d000..0x4001e6d0`); `__bPOWERONMENUInitial`
and both CC mailbox counts stayed 0. **No memory poke wakes the parked threads**
-- so the park is a *blocked eCos kernel wait* (mbox_get / flag_wait), not a
spin-poll of a data flag. The transition cannot be forced by poking a value; the
source-present PRODUCER has to actually run and reschedule the waiter.

**2. MediaPresentPost is the producer; it accepts source 7 but doesn't announce
at power-on.** In seize mode, `ct952.call(0x6130, 7, 1)`:
```
before  F_REQlo(0x40026e98)=0x0
ret     0x7
after   F_REQlo(0x40026e98)=0x7          # recorded "last source index = 7"
        F_REQ(0x40026e9c)=0, mboxes=0, POM=0
```
So `0x6130` is `MediaPresentPost(sourceIdx, present)` and source 7 is a valid
index -- calling it executed and recorded the source, but the flag-set / mailbox
post did not land (consistent with §12.64: the built-in/SPI source is never
actually announced at power-on -- the announce is gated, or the recorded index
never gets the follow-through `OS_SetFlag` under these no-media conditions).

**Conclusion.** The boot parks because, with no media, nothing completes a
source-present announcement, and the CC threads are *blocked* on the resulting
kernel objects -- a state a data-poke provably cannot escape (result 1). The
producer function exists and takes the built-in source index (result 2) but its
announce doesn't complete at power-on. Closing this needs a **call-and-resume**
primitive (seize PROC1, call the full announce path, restore PROC1's saved
context, and let it run) -- the current seize freezes PROC1 with no resume, and
concurrent PROC2 calls into eCos would race the non-SMP kernel. That primitive
is the next tool; the diagnosis (blocked-wait, producer-gated) is now firm.

### 12.84 Call-and-resume tool; park gate localized (but debugger is emulator-only)

Built call-and-resume for the seize debugger: the seize saves PROC1's full
context; `ct952.resume()` (writes 0x0C0FFEE0 to 0x80007FE8) restores it so the
firmware continues from the seize point after Python mutates the frozen state.
Verified: `[PYAPP] resumed PROC1 from saved context (pc=0x4001e6c0)`.

Used it to chase the source-present announce:
- `MediaPresentPost` (0x6130) sets `MediaInfo[i].present` (byte at
  0x40039b08+i*52+48; present7 verified = 1) and records the source index at
  0x40026e98, then calls the flag-setter 0x65dc.
- **The setter 0x65dc is gated**: it reads flag 0x40038f80, tests bit 0x80000,
  and if clear returns WITHOUT setting F_REQ (0x40026e9c). That is why the
  announce never posts. Lifting that gate + announcing + resuming still didn't
  reach POWERONMENU (0x40038f80 bit 0x80000 is itself gated upstream) -- the
  no-media park is gates-behind-gates in the media-present subsystem.

**Scope caveat (user's point, recorded):** this entire on-device debugger runs
in the *emulator*, where a payload can be injected freely. On real silicon,
running it would require the very flash/code-injection access we're trying to
obtain -- it's circular. The debugger is an analysis tool for the emulated
firmware; it does not help reflash a physical unit.

### 12.85 Reflash without decapping -- the stock updater: UPG952A.AP on FAT USB/SD

The non-circular reflash path (needs no pre-existing access; the stock boot/app
does it): the firmware has a **field updater keyed on the filename `UPG952A.AP`**
(string at flash 0x0e7658, referenced from the update routine at ~0x1bdc8 and
the update UI at 0x23xxx-0x25xxx). Context strings pin the mechanism:
`fatfs`, `usb`, `File Manager: Mount device %s OK`, `USB`, `SD`,
`KH_COMMON_QueryIfExistPlayableFile`, plus the AP-loader log
(`Find the desired AP, location:%lx`, `Switch to AP mode, ID:%lx`,
`Err: Not desired chip version auto-upgrade code:%lx,now:%lx`,
`Err: AP Size is %lx, larger than reserved space`, `AP_Loader() fail`).

Recipe (no chip access, non-disruptive to a running unit):
1. Obtain the new firmware as an **`UPG952A.AP`** file -- the CheerTek AP-image
   container (the application section + its header). The header carries the
   chip/version ("auto-upgrade code") the loader compares against the running
   part; a wrong code is refused ("Not desired chip version").
2. Put `UPG952A.AP` in the **root of a FAT-formatted USB stick or SD card**.
3. Insert into the running player -- the File Manager mounts the volume and the
   updater finds `UPG952A.AP`, validates it, erases + programs the serial flash
   (driver ~0x3ce34..0x3d1cc, §12.81), and reboots.

Only the **AP (application) section** is reprogrammed; the boot loader stays
intact, so a bad AP flash is recoverable (boot loader + recovery survive) --
lower brick risk than a full-chip write. Caveat: renaming a raw flash dump to
`UPG952A.AP` will fail the header/chip-version check; a valid `.AP` container
(from the OEM updater, or rebuilt with the correct header) is required.

### 12.86 UPG952A.AP + flash-image format fully reversed (byte-exact, verified)

Reversed the complete CT952A flash-image and `UPG952A.AP` update-container format
for a safe software reflash. Full spec: `jupiter/CT952A_FLASH_FORMAT.md`; tool:
`jupiter/tools/ctkap.py` (sections/apinfo/apfix/apwrap).

- **Section table** (24-byte entries @ flash 0x10): name/lma/rma/lsz/rsz then
  `(cksum16<<16)|flags16`. **cksum16 = sum of the UNPACKED section bytes & 0xFFFF**
  -- verified exact on every raw section and on the decompressed TEXT/ENGL.
- **UPG952A.AP** = a `CT909-AP` header (0x200 bytes) + a RAM-boot AP body. The
  loader (validator @0x4170, checksum @0x40320) enforces: magic0=0x43543930
  "CT90", magic1=0x392d4150 "9-AP", chip/auto-upgrade code @0x18 = 0x41(952A) or
  0x01, size @0x0c <= 0x166000 (4-aligned), APPacker ver @0x14 >= 5, and a
  **16-bit additive byte-sum of body[0x200:size] stored BE @0x2e** (NOT a CRC).
  Two independent disassembly passes agreed; magic/checksum/chip-code re-verified
  by hand instruction-for-instruction.
- Only the AP area (<=0x166000) is erased+programmed (64K-aligned sectors,
  driver 0x3d0fc/0x3d1cc); the boot loader is preserved => a bad AP is
  recoverable. The `.AP` body must be a valid self-flashing AP (the container
  tool builds/validates the wrapper + both checksum kinds).

`ctkap.py sections dp700wd.bin` verifies all content-section checksums; a
wrap->validate->corrupt roundtrip confirms the AP body-checksum detects a
single flipped byte.

### 12.87 UZIP reversed = LZMA1; ctkap.py now packs/unpacks + verifies every section

UZIP (the section compression, `rsz<lsz`) is **plain LZMA1** with props 0x63
(lc=0, lp=1, pb=2) inside a 13-byte XOR-obfuscated (key 0x5A5A5A5A) container:
word0 deobf=0x63000080 (props in high byte), word1/word2 carry the byte-shuffled
uncompressed size, byte12=0x5A, LZMA1 stream at +0x0D. Decompressor wrapper
@0x2C50, parser @0x2028, range-decoder @0x20b4. The decoder uses the output as
its window (no explicit dict_size / end marker; stops at the header size).

Independently corroborated: Python `lzma` FORMAT_RAW (and system liblzma
FORMAT_ALONE) decode the firmware stream byte-exactly (TEXT->0xc759,
ENGL->0xe14e), and re-encoding reproduces near-OEM size (TEXT 5972->5970,
round-trips). `jupiter/tools/ctkap.py` now:
- `sections` decompresses + checksum-verifies EVERY section (ROMV/TEXT/DATA/
  ENGL/SFAT all OK, not just raw ones);
- `unpack <flash> NAME out` extracts+decompresses a section;
- `repack body out` UZIP-compresses (real LZMA1 matching) with the correct
  obfuscated header, verified to round-trip.

So a modified section can be re-packed to firmware-format UZIP (or shipped raw).
CT952A_FLASH_FORMAT.md updated with the UZIP container + LZMA params.

### 12.88 Self-flashing AP body: emulator flash-write model + verified update loop

Reversed the serial-flash write path and built a testable self-flashing AP.

**Flash driver (all verified by disassembly):**
- `WriteSPF` @ `0x3d0fc` (XIP, always callable): `WriteSPF(i0=flashAddr,
  i1=srcBuf, i2=size)` — asserts `flashAddr` 64 KB-aligned + `size<=0x10000`,
  erases the 64 KB sector (one 64 KB erase, or 16×4 KB via `0x4001fea8` when
  `ldub[0x40033723]==4`), then programs via `0x4002003c(src, src+size, flashAddr)`.
- erase `0x4001fea8` / program `0x4002003c` live in decompressed TEXT (lma
  `0x4001d000`): mask interrupts (PSR PIL=15), cache-flush `0x40020578`, gate on
  the halfword `[0x40033720]==0x6655`.
- program → SPI loop `0x400200a0` → **SPI/PROM controller @ `0x80002800`**
  (base ptr read from `0x8000006c`; per-channel data `+0x22c`, status/trigger
  `+0x224`, cmd `+0x220`, result `+0x230`; channel index byte `[0x400238d3]`;
  opcode/config bytes at `0x40033720..0x40033728`), command issue+wait
  `0x4001fe20`, trigger/poll `0x40020324` (status bit 0x4 ready, timeout 0x32).

**Emulator model (`CT952_FLASHWRITE`, machine.c):** a *contract-level* model of
`WriteSPF` — intercept at its entry (`brk_pc=0x3d0fc` so `sparc_run` stops there),
apply the reversed contract to `m->flash` (64 KB erase to 0xFF + program `size`
bytes from the DRAM source), return `%o0=0` to `%o7+8`. Chosen over a gate-level
`0x80002800` model: the boundary is reversed byte-exact and simple, and gate-level
fidelity would only exercise driver internals we already trust while the real
brick-risk unknowns (start offset, entry convention) are unaffected by it.

**Self-flashing AP body (`jupiter/tools/apstub.S`, embedded in ctkap.py):** a tiny
SPARC V8 stub linked at the loader's body-copy target `0x4009a000`. It reads a
descriptor at `body+0x100` (`u32 nchunks; {u32 dstFlash,u32 srcBodyOff,u32 size}*`)
and loops `WriteSPF(0x3d0fc)` over a carried image 64 KB per call, then signals
`0xC0DED00D` and `ta 0`. `ctkap.py mkflasher <out.AP> --write DST:img` builds
body+descriptor+image and wraps a valid CT909-AP (magic, chip 0x41, pkver 5,
entry 0x30 = 0x4009a000, size + body checksum).

**Emulated update loop (`--aprun <AP> --flash-out <bin>`):** copies the AP body
`[0x200:size]` to DRAM `0x4009a000` and jumps to the header entry — as the loader
`0x3e48` does after header validation — then runs the body under the flash-write
model. Verified end-to-end:
- *Targeted sector:* a 1×64 KB sentinel write to `0x1a0000` changed **only** that
  sector; boot region `[0..0x3000)` and all other flash byte-identical.
- *Multi-sector + boot:* a 4×64 KB reflash of sectors 0–3 (with a boot-safe marker
  in the no-checksum SETD sector) programmed all four, kept the image header +
  section table intact, and the reflashed dump **boots to the exact same state as
  the original** (pc/npc/psr/tbr/cwp/wim identical at 20M instrs).

**Two honest caveats (documented, not worked around):**
1. *Raw-image size.* A full `0..0x166000` image can't be carried **raw** inside an
   AP that must itself be `<=0x166000` — this is precisely why the OEM body is
   UZIP-compressed and decompresses sections on the fly. The raw stub is right for
   targeted/sector updates; a full reflash needs the compressed-section approach
   (which `ctkap.py repack` supports for the payload).
2. *Real-hardware driver location.* On hardware, calling the XIP `WriteSPF` to
   rewrite the sector that *holds* `WriteSPF` would erase the running code. The
   OEM body avoids this by running a **DRAM-resident** copy of the flash driver
   (the AP's own TEXT/DATA). The emulator's entry-intercept hides this (the XIP
   sector is never actually executed during erase), so the stub verifies flashing
   *logic*, not hardware-safe driver placement. A hardware-ready body must embed
   the DRAM driver + the correct AP start offset — safest lifted from an OEM AP.

### 12.89 Gate-level 0x80002800 SPI/PROM controller model -- real DRAM driver reflashes

Modeled the serial-flash controller at the register level so the firmware's OWN
DRAM-resident flash driver reprograms the emulated flash -- no WriteSPF shortcut.

**Controller register banks** (per-channel, stride 4, channel byte [0x400238d3]=1):
- CMD    `0x80002a20 + c*4` -- write issues an SPI transaction; word = the 24-bit
  address stored LSB-first in the upper 3 bytes | opcode in the low byte.
- STATUS `0x80002a24 + c*4` -- fw writes a control/clear value, then polls a "done"
  bit (WREN/SE:0x1000, PP:0x800, RDSR:0x1000; model sets 0x4|0x800|0x1000).
- DATA   `0x80002a2c + c*4` -- page-program byte stream.
- RESULT `0x80002a30 + c*4` -- RDSR status byte read back.

**Reversed command flow** (all from the decompressed TEXT driver):
- dispatch `0x4001fe20`: st ctrl->STATUS, st cmdWord->CMD, poll STATUS & mask.
- RDSR poll `0x40020324`: st 0->STATUS, st RDSR->CMD, poll STATUS&0x1000, RESULT&m==exp.
- sector erase `0x4001ff80`: WREN -> RDSR(WEL) -> SE(0x20)+addr -> RDSR(WIP).
- page program `0x400200a0`: per 256-byte page: WREN -> first byte to DATA -> PP(0x02)
  at the page addr -> stream the remaining 255 bytes to DATA (RDSR-WIP between each).
- post-erase unlock `0x400204c0` -> `0x4002046c`: WRSR(0x01) to clear protection.

**Model** (`spi_ctrl_*` in machine.c, armed by m->spi_ctrl_on): standard serial
NOR -- WREN(0x06)->WEL, WRDI(0x04)/WRSR(0x01)->clear WEL, RDSR(0x05)->WIP(=0)|WEL,
SE(0x20)/BE(0xD8)->erase the sector to 0xFF, PP(0x02)->OPEN a streaming page-program
at the addressed byte. The key subtlety: PP is a *stream* -- the command programs the
bytes buffered before it and arms a running address; each later DATA write programs
one more byte at that address (RDSR polls in between must NOT disturb it). Ops
complete instantly (WIP reads 0) so every fw poll passes first try.

**Two bugs the test caught (kept for the record):**
1. WRSR (0x01, from 0x4002046c) wasn't clearing WEL, so 0x400204c0's `(RDSR&3)==0`
   wait spun forever -> the whole call burned its budget. Fix: handle WRSR.
2. Accumulate-then-flush was wrong (and mid-stream RDSR cleared the buffer): only the
   first byte of each 256-byte page landed, the rest stayed 0xFF. Fix: stream each
   DATA byte into the open page-program.

**Verified:** boot normally (populates the driver config + decompresses TEXT/DATA
into DRAM), arm the model, then CALL the real `WriteSPF(0x3d0fc)` via
`--spitest ADDR:SIZE`. The real SE/PP helpers issue real SPI commands the model
services against m->flash: SIZE bytes at ADDR match the staged sentinel exactly,
16 erase + 16 program ops (the 64 KB sector + 0x1000-byte payload), and the surrounding flash is untouched. This is the
faithful counterpart to the WriteSPF-contract model (12.88) -- same net effect,
but driven end-to-end by the firmware's own driver code.

### 12.90 Full self-flashing AP through the gate-level controller (--apflash)

Ran the ENTIRE update path on firmware code, with only the SPI controller modeled:
boot -> AP body in DRAM -> real WriteSPF (XIP) -> real erase/program (DRAM) ->
gate-level 0x80002800 controller (12.89) -> m->flash.

`--apflash <AP>`: boot normally (driver resident + config populated), arm the
controller model, copy the AP body [0x200:size] to DRAM 0x4009a000 and jump to the
header entry (0x30) -- as the loader (0x3e48) does after validation -- then let the
body run. The body (apstub.S) calls the real `WriteSPF(0x3d0fc)`, whose real SE/PP
helpers issue real SPI commands the controller services against m->flash. The body's
window environment is set trap-free (S=1, PIL=15, ET=0, WIM=0, as machine_call uses)
so nested save/restore just rotate and the terminating `ta 0` halts cleanly.

**Verified** (`--apflash apflash.AP`, body writes a 4 KB sentinel to sector 0x1a0000):
- boot -> body jump -> body ran 217859 instrs, halted at its `ta 0` (pc 0x4009a060);
- **16 erase + 16 program** SPI ops (the real driver, not a hook);
- target sector == the sentinel, full 64 KB sector erased (tail 0xFF);
- boot region [0..0x3000), the XIP WriteSPF sector [0x30000:0x40000), and all
  surrounding flash byte-identical.

So the complete chain -- loader-style body load, the firmware's own flash driver,
and the SPI controller -- executes end-to-end in the emulator, every layer real
firmware code except the modeled controller. Note the caveat from 12.88 still holds:
the body targets a sector that does NOT hold the XIP WriteSPF trampoline; a hardware
body that rewrites the low image must run a DRAM copy of the flash driver (as the OEM
AP does), so it never executes flash it is erasing.

### 12.91 Firmware SOURCE found in-repo; AP format + DRAM-driver reloc verified src↔binary

The repo carries the firmware's own C source -- `aploader.c` (AP loader), `spflash.c`
(SPI flash driver), `hsystem.c` (`HAL_CheckSum`), `Winav.h` (IC versions). No OEM
`UPG952A.AP` file is downloadable, but the source + our binary together pin the whole
format down. Everything below is cross-checked against `dp700wd.bin`.

**Controller model confirmed exact.** `spflash.c` `HLCHANG(I) = (I<<24) | ((I&0xff00)<<8)
| ((I&0xff0000)>>8)` is byte-identical to the gate-level model's address encode; opcodes
PP=0x02, SE=0x20/0xD8, WREN=0x06, RDSR=0x05, WRSR=0x01 and the STATUS control values
`Format_Page_Program`=0x0c / `Format_Sector_Erase`=0x0b all match what the model services.

**Checksum confirmed.** `HAL_CheckSum` (`hsystem.c`) = additive byte-sum into a 16-bit
WORD over `[start,end)` stepping by DWORD -- exactly `ctkap` `sum16` and the binary's
`0x40320`. AP-body range = `body[0x200 : dwAP_Size]`.

**AP_INFO header corrected to source** (`aploader.h`): `0x08`=`dwAP_Type` (auto-upgrade=1),
`0x10`=`dwExternalFlag`, `0x18`=`dwChipVersion`, `0x2C`=`dwCheckSum`, `0x30`=`dwAP_SP`
(a stack pointer, NOT an entry -- my earlier "entry@0x30" was wrong), `0x34`=`dwAP_UNZIP_BUF`.
`ctkap.py apinfo` now names/validates these faithfully.

**Key constants -- source has variants, binary is authoritative:**
- `IC_VERSION_952A = 0x41` (`Winav.h`) == the binary's chip-check constant. VERIFIED.
- `DS_AP_CODE_AREA` (AP body dest): the repo's `dvd_dram_16m.h` says `0x4008b000`, but a
  scan of `dp700wd.bin` shows **`0x4009a000` used 13x and `0x4008b000` zero times** -- our
  binary is a different DRAM-layout build. `0x4009a000` (my original reversal) is correct.
- `AP_TABLE_ADDRESS = 0x40000800` (source) == 6 sethi hits in the binary. VERIFIED.

**The DRAM-driver relocation -- answered by the format itself.** `AP_Loader` STEP6 runs
`ROMLD_BOOT_LoadSectionAndRun((PSECTION_ENTRY)AP_TABLE_ADDRESS, dwAP_UNZIP_BUF, dwAP_SP)`:
a loader-accepted AP body is a **section-table image** (AP_INFO + [image hdr 0x10][section
table] + compressed sections), and the loader DECOMPRESSES the AP's sections into DRAM and
runs it. So the AP's flash driver runs from DRAM by construction -- it never executes flash
it is erasing. That is exactly the reloc; it's inherent to the AP format, not something a
body has to hand-roll.

**Consequence for our tooling.** `mkflasher`'s body is a RAW code stub (entry at body base)
for the emulator's direct-jump harness (`--aprun`/`--apflash`) -- it proves the flash
mechanism but the on-device loader, which *section-loads*, would not run it. A loader-
compatible AP needs the section-table wrapper: a small flasher app linked to a DRAM LMA,
UZIP-packed as a section-table image behind the AP_INFO header (a future `ctkap mksectionap`).

### 12.92 Section-table flasher AP run through the REAL loader (--apload)

Built a loader-compatible self-flashing AP -- a *section-table image*, the format the
on-device loader actually accepts (12.91) -- and ran it through the firmware's real
ROM loader. Every layer is firmware code except the modeled SPI controller.

**The AP** (`ctkap.py mksectionap`): AP_INFO(0x200) + [AP image hdr 0x10] + [32
SECTION_ENTRY table] + the flasher app as ONE section (name `FLSH`, Load|ProgEntry,
LMA 0x40500000). The flasher (`apstub_sec.S`) reads a descriptor + image and reflashes
by calling the resident DRAM driver (erase 0x4001fea8 / program 0x4002003c). dwRMA is
stored IMAGE-RELATIVE (offset within the AP image); the loader resolves the source as
`dwRMA + pSecTbl - 0x10` after ROMLD_MoveSectionTable rebases it -- the two cancel.

**The run** (`--apload`): boot, arm the gate-level controller, stage the whole AP at
DS_AP_CODE_AREA=0x4009a000, replicate ROMLD_MoveSectionTable (0x4009a210 -> 0x40000800,
dwRMA += src-dest), then CALL the binary's ROMLD_BOOT_LoadSectionAndRun (@0x4bc ->
iterator @0x528). The loader raw-copied the FLSH section to LMA 0x40500000 (verified:
LMA reads back `21101400 e2042100` = the flasher `_start`), checksum-passed it, and
jumped to it. The flasher then reflashed: **16 erase + 16 program** SPI ops.

**Verified byte-exact** (flasher writes a 4 KB sentinel to sector 0x1a0000): target ==
sentinel, sector tail 0xFF, and the boot region [0..0x3000), the XIP WriteSPF sector
[0x30000:0x40000), and all surrounding flash untouched.

**Two things reversed to get here** (each cost a debug pass against the binary):
1. `dwRMA` is image-relative, not absolute: the loader adds `pSecTbl-0x10` (binary
   0x5cc-0x5d0). My first build made it absolute (via MoveSectionTable), the loader
   added the base again, and it read garbage -> section didn't load. Fix: store the
   image-relative offset.
2. The section loader (0x528) skips a section whose dwLMA <= 0x3fffffff and picks the
   Load|ProgEntry section to jump to (dwCheckSumFlag bit1); its checksum is the byte-sum
   of the UNPACKED bytes at the LMA (romld.c ROMLD_LoadSectionTo).

**Clean-halt fix (was a trap, now resolved -- §12.94):** the flasher first ended on a
stray `trap 0x02 @ 0x41400804`. A pc-ring trace showed the reflash always completed and
the flasher reached its own `ta 0`, but the firmware driver/loader leaves traps enabled
(ET=1), so `ta 0` took a REAL trap to the vector (tbr 0x40000000 | tt 0x80<<4 = 0x40000800)
and executed the section-table bytes as code -> illegal instruction. Fix: the flasher
clears ET (`rd %psr; andn 0x20; wr %psr`) before `ta 0`, so the halt is error-mode
regardless of ET. It now stops cleanly at its own `ta 0` (trap 0x80 @ 0x405000e4) --
confirmed on the FULL 22-sector compressed-image reflash too (352 erase + 5632 program,
flash[0:0x160000] == the new image, and the reflashed dump boots identically). On
hardware the AP reboots here instead; the `ta 0` halt is only the emulator's end marker.

So the COMPLETE on-device update path -- AP_INFO validation, MoveSectionTable, the real
ROMLD_BOOT_LoadSectionAndRun section loader, the flasher app running from its DRAM LMA,
the firmware's own flash driver, and the SPI controller -- executes end-to-end for a
self-built, loader-compatible AP, with only the controller modeled.

### 12.93 Full compressed-image reflash: --image / mksectionap section-load path

Extended `mksectionap` for whole-image reflash. `--image DST:img` ships the payload as
its own UZIP-compressed section (`IMGn`, Load|ZIP) instead of raw bytes in FLSH, so a
large image fits under the AP cap: the loader (0x528 -> UZIP codec 0x2c50) decompresses
it to a DRAM LMA (0x40600000), and the flasher writes the decompressed bytes. dwAP_UNZIP_BUF
(header 0x34) supplies the decoder work buffer (0x40780000); descriptor chunk src offsets
into IMGn are `(IMGn_LMA - FLSH_LMA) + off`.

Verified through the real loader (`--apload`): a 256 KB image (UZIP 0x40000->0x17d1d)
decompressed and reflashed byte-exact -- **64 erase + 1024 program** SPI ops (4 sectors x
16 erase; 4 x 256 pages), flash[0:0x40000] == the new image incl. a boot-safe SETD marker,
flash above untouched. The full [0:0x160000] firmware image UZIPs to 0x7f33c (well under the
0x166000 cap), packs into a 0x7fa58 AP (22 sectors, photo+COPY above 0x160000 preserved).

So the update pipeline is complete for a self-built AP: `mksectionap --image` builds a
loader-compatible, compressed, section-table AP, and `--apload` runs it through the real
ROMLD loader (decompress -> flasher -> resident driver -> gate-level controller -> flash).

**Full-image result (completed):** the whole [0:0x160000] firmware image (22 sectors)
reflashed byte-exact through the real loader -- **352 erase + 5632 program** SPI ops
(22x16 erase; 22x256 pages), flash[0:0x160000] == the new image, differing from the
original by exactly the 18-byte SETD marker, with the photo + COPY above 0x160000 intact.
And the reflashed dump **boots identically to the original** (pc/npc/psr/tbr/cwp/wim
match at 20M instrs). So the complete update path -- compressed section-table AP -> real
ROMLD decompress -> flasher app from DRAM -> firmware flash driver -> gate-level controller
-> flash -> bootable image -- is verified end-to-end for a full firmware image built from
our own binary. (Needed a 600M single-step budget for the 1.4 MB decompress + reflash.)

### 12.95 MicroPython on the frame as a NON-DESTRUCTIVE run-AP (the on-device debugger)

The AP loader loads a UPG952A.AP's sections into DRAM and jumps to the ProgEntry
section -- exactly how the OEM apps launch. So MicroPython can BE the AP: no reflash,
no brick risk, and a reboot restores stock firmware. `ctkap.py mkrunap out.AP
<payload.bin> [--lma 0x40500000]` wraps the embedded MicroPython app (jupiter/mpy
APP build, entry _app_start at offset 0 -- installs its own trap table/stack/PSR)
as one Load|ProgEntry|ZIP section 'MPY '. The 0x99b48 (629 KB) payload UZIPs to
0x1a54d -> a 0x1aa60 (109 KB) VALID AP.

Verified end-to-end through the REAL loader (`--apload`, feeding the REPL over UART):
the loader decompressed the MPY section to 0x40500000 and jumped in; MicroPython
booted ("[pyapp] MicroPython launched ... the firmware is live: ct952.peek32/poke32/
call"), and the fed line executed:

    >>> print('PY-ON-FRAME', 6*7)
    PY-ON-FRAME 42
    >>>

So a live, interactive MicroPython REPL with ct952.peek32/poke32/call runs on the
device from a USB/SD UPG952A.AP -- the on-device debugger, with nothing written to
flash. On hardware, MicroPython reads/writes UART1 (and the modeled USB HID keyboard);
the firmware's DRAM state stays intact and inspectable from Python.

**How to use it on the real frame:**
1. `python3 jupiter/tools/ctkap.py mkrunap UPG952A.AP jupiter/mpy/build-app/firmware.bin`
2. `python3 jupiter/tools/ctkap.py apinfo UPG952A.AP`   (every field OK)
3. copy UPG952A.AP to the ROOT of a FAT USB stick / SD card
4. trigger the player's update mode -> MicroPython REPL over UART1 (no flash touched)

---

### 10.17 Bare-metal AP OSD bring-up — how a run-from-DRAM AP must (not) touch the display

Context: getting a bare-metal AP (banner / MicroPython console) to render legibly
on the REAL frame, loaded via the SD/USB AP-loader path (`--apload` / `UPG952A.AP`).
Verified against `dp700wd.bin` by direct disassembly (symbol addresses from the
DVD909-toolchain map are valid XIP offsets for this binary — the *chip* is 952A,
the *build lineage* is DVD909; the addresses check out against the actual bytes).

- **The AP loader configures + activates the OSD BEFORE jumping to the AP.**
  `aploader.c` STEP2: `GDI_ConfigRegionInfo(0,&RegionInfo)` (wWidth=616, wHeight=78,
  bColorMode=`GDI_OSD_4B_MODE`=1, dwTAddr=`DS_OSDFRAME_ST_AP`=0x40084000) →
  `GDI_InitialRegion(0)` → `GDI_ClearRegion(0)` → `GDI_ActivateRegion(0)`. So when
  the AP starts, region 0 is already live, pointed at 0x40084000, correct stride.
- **Stride = `wWidth >> bColorMode` = 616>>1 = 308 bytes/row** (`gdi.c _gdi_SetPixel`,
  `dwRegionWidthInByte`). GDI writes at 308 and the scanout reads at 308, so the
  loader's own text is coherent — 308 is authoritative.
- **The OSD scanout is driven by the DISP block at `0x80002xxx`, NOT `VCR20`.**
  `DISP_OSDSet` (`0x7e594` XIP) computes `base+byteoffset` from the region struct
  and stores to `0x80002450`; it pokes `0x800021c0`; it never writes `REG_MCU_VCR20`
  (0x80000D80). `VCR20` ("VOU OSD Read Channel base") is a *different* path. The
  ct952emu scanout honors `VCR20` as a **crutch** because `--apload` does NOT run
  the loader's `DISP_OSDSet` (it only replays `MoveSectionTable` + the low-level
  `ROMLD_BOOT_LoadSectionAndRun` @0x4bc). Consequence: **on real hardware a bare
  AP must touch NO display registers** — writing `VCR20`/`REG_DISP_OSD_SIZE` only
  clobbers the loader's working DISP config. The AP should ONLY write pixels into
  0x40084000 at stride 308 and let the loader's scanout show them.
- **Palette RAM writes need the DISP-blob handshake, not a bare store.** `GDI_LoadPalette`
  → `DISP_SetPalette` wraps every `REG_DISP_GAM_OSD(i)` access in `REG_VLD_SHO32 =
  0xFFFFFFFF` + double-read + `GDI_WaitPaletteComplete` (`gdi.c:3911-3925`). A plain
  `*(0x80001C00+i*4)=yuv` is accepted by the emulator (models palette RAM as memory)
  but IGNORED on silicon. So a bare AP inherits the LOADER's live palette:
  empirically index 1 = yellow, 2/3 = black/white.
- **A solid fill hides stride errors** (all bytes equal → uniform at any pitch). Only
  text / thin lines expose the true stride. Do not conclude "stride correct" from a
  clean solid background.
- **The faithful path if raw-pixel bring-up keeps fighting the hardware:** call the
  firmware's own renderer from the AP (verified XIP addresses, region globals left
  intact by the loader): `GDI_FillRect` @0x7cdc, `GDI_DrawString` @0x9664,
  `GDI_ClearRegion` @0x78b4 — they write at the real stride by construction.
- **Emulator honesty:** ct952emu's `--apload` never runs `DISP_OSDSet`, so it cannot
  validate "no-register-poke" AP display behavior. That is a genuine gap, not a
  detail — historically papered over with `--fb-addr` + forced 4bpp `--fb-wh`, which
  makes the emu agree with whatever stride/base you pass it. Real-display bring-up
  must be judged on the frame, or by first teaching the emu to run the loader's DISP
  setup.

---

### 10.18 Faithful DISP model -- RE notes (in progress)

Building an emulator OSD scanout that reads geometry from the real registers
instead of hand-fed --fb-addr/--fb-wh. Findings, all verified by disassembling
dp700wd.bin directly (NOT via DVD909.sym -- see the warning below):

- **DVD909.sym is MISALIGNED with dp700wd.bin. Do not trust its addresses.**
  Proof: the sym puts DISP_OSDSet@0x7e594 / DISP_DisplayCtrl@0x7ea18 /
  DISP_SetPalette@0x7ebf0, but the code THERE writes the VLD/MC video-decode
  block (0x80002xxx: 0x2450=REG_MC_BASE+0x340, 0x21c0=REG_VLD_MBINT_CTL,
  0x2200), not the DISP block. The **real** DISP OSD code is at file offset
  ~0xa3000-0xa5c00 (writes OSD_CR 0x80001a58 @0xa4ca4/0xa5b1c; VCR23 @0xa57bc/
  0xa5888). Earlier "GDI region code at 0x76ec" was a false-positive match.
  CONSEQUENCE: the whole "call firmware GDI at sym addresses" detour was calling
  the WRONG functions -- that is why GDI_FillRect drew nothing and DISP_OSDSet
  never programmed an OSD register. Any future firmware-call work must locate
  functions by register footprint, not by the symbol map.

- **The OSD read channel is VCR20-23** (REG_MCU @ 0x80000d80..d8c):
  VCR20=0xd80 base, VCR21=0xd84 base(2nd), VCR22=0xd88 (width<<16 | height),
  VCR23=0xd8c increment/**stride**. The real setup code (~0xa5760 and ~0xa5814,
  two near-identical blocks -> two fields/planes) computes W/H and increment
  from DRAM display-config globals at 0x40040e78 and 0x40040eb4, then
  `st -> VCR22` and `st (val<<shift)+4 -> VCR23`. Stride is NOT wWidth>>colormode
  (that is only the GDI software write pitch); the SCANOUT stride is VCR23,
  computed from the display config -- still being decoded.

- **Display timing is INTERLACED NTSC at boot**: TGEN(0x1A38)=0x120d035a
  (Vtotal=525, Htotal=858), SYNC_WH(0x1A3C) PSCAN_EN(bit28) CLEAR = interlaced.
  Two fields shown as two spatial bands is the leading explanation for the
  on-hardware "2 bands". GDI_REGION_INFO.dwTAddr is the "top field" buffer;
  a bottom-field buffer/stride is the second VCR block.

- Emulator instrumentation: CT952_DUMP_OSD now dumps the OSD channel
  (0x2440-0x2460), OSD window/CR, and scan/scale/interlace regs, in both the
  normal and --apload paths. CT952_OSD_FORCE ungates the debug scanout readback.

**STRIDE FORMULA -- SOLVED and VERIFIED.** The real DISP setup code (file offset
0xa5758-0xa580c) computes, from W=0x40040eb4, H=0x40040e78, and a mode byte whose
low 2 bits go to CM=0x40040d64 and whose high nibble goes to SEL=0x40040ec0
(writer at 0xa45e4-0xa4604: `CM = mode & 3`, `SEL = mode & 0xF0`):

```
    shift  = (SEL > 0x1F) ? CM+1 : CM+2
    VCR22  = (H << 16) | (W >> shift)                      /* io 0xD88 */
    VCR23  = ((W << (16 - CM + (SEL > 0x1F))) & 0xFFFF0000) + 4   /* io 0xD8C */
```

so the registers decompose as

```
    VCR22 = (height_lines << 16) | (width / X_increment)
    VCR23 = (Y_increment  << 16) | X_increment      /* X_increment = 4 */
    SCANOUT ROW STRIDE (bytes) = VCR23 >> 16
                               = W >> CM        if (mode & 0xF0) <= 0x1F
                               = W >> (CM-1)    if (mode & 0xF0) >= 0x20  (doubled)
```

Verified end-to-end: an AP programming the channel for W=616,H=78,CM=1,SEL=0
produces VCR22=0x004e004d ((78<<16)|77) and VCR23=0x01340004 ((308<<16)|4),
i.e. **stride 308** -- and a 32-bit block readout drawn at that stride decodes
back byte-exact from the rendered framebuffer.

**Consequence -- no more stride guessing anywhere.** The stride is READABLE at
runtime: `stride = REG_MCU_VCR23 >> 16`, `height = REG_MCU_VCR22 >> 16`. Any AP
should read it rather than assume 308/360/616, because the loader's DISP setup
has already programmed it.

**Emulator (now faithful on this axis):** machine_disp_scanout derives stride and
height from VCR22/VCR23 when the channel is programmed, and when it is NOT it
prints a loud WARNING that the geometry is the caller's and the scanout is not
authoritative (CT952_OSD_NOREGS forces the old behavior). It also fixes a real
unfaithfulness: the 4bpp decode used `w>>1` as the row pitch instead of the
hardware stride, which silently made the scanout agree with whatever --fb-wh the
caller passed -- the reason earlier "the emulator confirms stride 308" checks were
circular and worthless.

STILL OPEN: what the SEL>=0x20 stride doubling means physically (interlaced field
scan vs OSD horizontal upscale, cf. REG_MCU_VCR25 "OSD upscalling"), and the
two-field layout (two near-identical VCR setup blocks at 0xa5760/0xa5814).

---

### 10.19 CONFIRMED ON HARDWARE: OSD stride = 308, buffer is linear, banding is vertical

First hard measurement off the real DP700WD (photo of the panel), using a
byte-space bucket readout (the AP fills contiguous BYTE ranges of the region, so
the pattern cannot be garbled by the very row-mapping under test, and reports
`REG_MCU_VCR23 >> 16` as one of five unmistakable bar lengths):

- **The bar landed in the QUARTER bucket => stride == 308 bytes.** This is the
  hardware's own Y-increment read back from VCR23, not an inference. It confirms
  the RE'd formula of 10.18 (`stride = W >> CM` = 616 >> 1 = 308) and confirms
  `DS_OSDFRAME_ST_AP = 0x40084000` (dvd_dram_16m.h:246) is the right buffer.
- **The OSD buffer is LINEAR.** Contiguous byte runs rendered as clean, solid,
  full-width bands. A tiled/swizzled/bank-interleaved layout could not produce
  that, so plain `off = y*stride + (x>>1)` addressing is correct.
- **The residual "2 bands" is a VERTICAL mapping artifact, not a pitch error.**
  The region's content appears ~twice with black filling the remainder. Cause:
  `REG_DISP_OSD_SIZE = 0x00f002d0` = a **720x240** OSD window, while the AP region
  is only 616x78 -- the scanout keeps fetching past the 24024-byte region for the
  remaining ~162 lines. Interlace (PSCAN_EN clear, 10.18) plus the two VCR base
  blocks are the likely duplication mechanism.
- Corollary: earlier pixel-space probes at stride 308 looked "garbled" mostly
  because 8px-tall text cannot survive the vertical duplication/squash, and the
  early builds also clobbered the loader's DISP config by writing VCR20/OSD_SIZE.
  Byte-space or large-feature drawing at stride 308 renders cleanly.

**Practical rule for any AP drawing to the OSD:** read `stride = VCR23 >> 16`
(don't assume), write pixels as `off = y*stride + (x>>1)` capped to the 24024-byte
region, write NO display registers, and keep features tall (>= ~6 rows) until the
vertical mapping is pinned down.

NEXT: pin the vertical mapping -- probe with 6-row alternating bands (count of
bands on screen gives the vertical scale/duplication factor) plus a hard
left/right colour split, whose boundary is a straight vertical edge iff the
stride is right (pixel-space confirmation of 308).

**CORRECTION (from the device owner) -- "all black + one white line" is the AP-mode
DEFAULT SCREEN, not a failure.** The frame always shows a black screen with a
single solid white line ~2/3 down when it enters SD/USB AP mode; everything an AP
draws appears ON TOP of that. Consequences for earlier notes in this document:

- Runs previously recorded as "all black => our content did not render / the AP
  crashed / wrote past the region and faulted" were, in at least some cases, simply
  **nothing visible drawn** (fully transparent, or drawn outside the visible
  window). Do NOT treat a black screen as evidence of a crash or a write fault.
- The white line is part of that default screen. It is NOT an artifact of our
  content, a stride symptom, or a leftover from a previous AP. Ignore it entirely
  as a diagnostic signal.
- Black areas inside our region are index 0 = the OSD transparent colour key
  (DISP_OSD_T_EN), letting the default screen show through -- so "black" means
  "transparent/undrawn here", not "broken".
- Re-reading the 10.19 photo with this in mind: the pattern is
  [content][transparent][content][transparent], i.e. our 78-row region is painted
  TWICE inside the taller (240-line) OSD window, each copy followed by undrawn
  lines showing the black default. 240/2 = 120 lines per copy ~= 78 drawn + 42
  undrawn, which matches the observed content:black ratio.

**RETRACTION of 10.19's "stride = 308 CONFIRMED".** That conclusion was WRONG and
must not be relied on. The byte-space bucket readout did show `VCR23>>16 == 308`,
but a follow-up probe that drew in (x,y) at pitch 308 -- with a hard left/right
colour split whose boundary must be a straight vertical edge -- came back
**DIAGONAL** on hardware ("black lines cutting up the two yellow bands
diagonally, nothing was straight"). A diagonal edge means each successive display
line starts at a different x, i.e. the hardware's line pitch is NOT the 308 I drew
with.

What this proves methodologically: **a uniform/solid byte-range fill cannot
validate a pitch** (every row looks identical at any pitch), so the clean bands in
10.19 were never evidence for 308. Only pixel-space features can test pitch. The
same trap invalidated the emulator checks earlier (§10.18).

So `REG_MCU_VCR23 >> 16` is NOT the scanout line pitch, or not it alone. Leading
hypothesis: the pitch follows the OSD *window* width, `REG_DISP_OSD_SIZE =
0x00f002d0` -> 720 px, which at 4bpp is **360 bytes per display line**, not the
region's 308. This also fits the owner's early observation that 360 looked "more
aligned" than 308.

What DOES still stand from 10.19: the buffer is linear and memory order maps
monotonically to raster order (byte ranges render as contiguous screen areas), and
the region's content is painted ~twice inside the taller 240-line OSD window.

Resolution in flight: a one-shot 8-candidate pitch finder. The region is split
into 8 byte zones; zone k gets a column of white marks spaced every P[k] BYTES,
phase-aligned so that if P[k] is the true pitch the column sits at x=120 (well
inside the visible 480 px). Marks spaced by exactly the true pitch land on
consecutive lines at the same column -> one zone shows a STRAIGHT VERTICAL line
and identifies the pitch in a single flash. Candidates: 308, 320, 336, 344, 352,
360, 368, 384. Verified in the emulator against synthetic true pitches 308/344/
360/384 -- in each case exactly the matching zone renders a straight column at
x=120. (Note: zone boundaries do not align to row boundaries, so a single
boundary row can carry a neighbouring zone's mark; ignore one stray.)

**Pitch sweep pass 1 result: NEGATIVE, and it bounds the answer.** No zone in the
308..384 sweep produced a vertical column on hardware, so the true line pitch is
**below 308**. The same photo also yields an independent estimate: the 24024-byte
region renders over ~83 panel lines (panel interior ~960 photo px for 234 lines =>
4.10 px/line; the top copy's content spans ~340 px), so

    pitch ~= 24024 / 83 ~= 290 bytes

Note 290 is nowhere near 360 (the 720px-window hypothesis) and not 308 either, so
BOTH earlier hypotheses are dead. 480 px * 4bpp = 240 B (the panel width) is also
excluded by this estimate.

Pass 2 sweeps 276..304 in 4-byte steps, and improves the readout: marks are 24 B
(48 px) wide, so at the true pitch consecutive marks ABUT into a solid vertical
bar, while any other pitch breaks into a visible staircase -- much easier to judge
than a column of thin dashes. Verified in the emulator against synthetic pitches
288 and 292: only the matching zone renders a solid bar (1 distinct left edge; all
others 8-10).

**Pitch sweep pass 2 result: the solid bar landed in zone 5 => LINE PITCH = 292
BYTES.** The fat-mark probe rendered a compact solid white block (not a staircase)
at ~60% down the region in both copies, and zone 5's center is at 56% -- the only
candidate that fits. Cross-checks: the independent height estimate was ~290, and
292 is a multiple of 4 as expected, since VCR23's low half (the X increment) is 4.

So the OSD scanout line pitch is **292 bytes = 584 px at 4bpp** -- NOT the region
width (308 B / 616 px) and NOT the OSD window width (360 B / 720 px). Neither
register value is the pitch; treat `VCR23 >> 16` as unreliable for this purpose
until the discrepancy is explained (it reads 308 while the panel scans 292).

Confirmation method now in flight, worth reusing: a **self-identifying** readout.
Three stacked zones each draw their own candidate pitch as large (3x, 24 px)
digits, addressed USING that candidate as the row pitch. Digits are only legible
when the pitch matches the hardware, so the screen literally displays the right
answer and needs no measuring, counting or ordinal reporting. Verified unbiased in
the emulator: rendered at 288/292/296 the readable number is 288/292/296
respectively, with the other two zones sheared into diagonal hash.

### 10.20 MILESTONE: legible text on the real panel (pitch 292 CONFIRMED)

The self-identifying probe came back **"292 perfect"** from the device owner: the
zone that drew its own number using 292 as the row pitch was legible, its 288/296
neighbours sheared into hash. So:

**OSD scanout line pitch = 292 bytes = 584 px at 4bpp. CONFIRMED ON HARDWARE.**

Authoritative recipe for drawing to the OSD from a bare-metal AP loaded via the
SD/USB AP path (all of it verified on silicon):

```
    base   = 0x40084000        /* DS_OSDFRAME_ST_AP, dvd_dram_16m.h -- 2MB part */
    pitch  = 292               /* MEASURED; do NOT derive it from a register    */
    rows   = 24024 / 292 = 82  /* region is 24024 B; clamp every write to it    */
    4bpp, big-endian nibbles: even x = high nibble, odd x = low
    off    = y*292 + (x>>1)
    palette (loader's live one): 0 = transparent key, 1 = yellow, 2 = white
    write NO display registers -- doing so clobbers the loader's working config
```

Why the pitch must be hardcoded from measurement: it matches NO register found so
far. It is not the GDI region width (308 B / 616 px), not the OSD window width
(360 B / 720 px); `REG_MCU_VCR23 >> 16` reads 308 while the panel scans 292. That
discrepancy is still unexplained and is the main open item on the display axis --
until it is resolved, 292 is an empirical constant, and the emulator's
register-derived scanout (10.18) will disagree with hardware for AP-drawn content.

Cosmetic, not yet addressed: the region is painted ~twice inside the taller
240-line OSD window, and the panel's AP-mode default screen (black + one white
line ~2/3 down) shows through wherever we leave index 0.

What finally worked, after ~10 failed hardware rounds, was fixing the MEASUREMENT
method rather than guessing harder:
 1. never trust a uniform fill to validate a pitch (it looks right at any pitch);
 2. draw probes in BYTE space when the pixel mapping is the unknown;
 3. make the readout SELF-IDENTIFYING (render each candidate using itself, so the
    screen names the answer) instead of asking a human to count zones or estimate
    fractions -- ambiguous ordinal reporting cost several rounds;
 4. get one photo and measure it, rather than iterating on prose descriptions.

### 10.21 The vertical "repeat": constraint found, and the panel as a register readout

Confirmed on hardware: the banner renders legibly at pitch 292, but the region's
content is painted TWICE down the panel with a black band between the copies.
Photo geometry puts the repeat period at ~160-168 lines, i.e. roughly 2x the
region height, with the second copy truncated at the bottom.

**Hard constraint -- the repeat CANNOT be fixed by drawing more lines.** The AP OSD
region is bounded by `DS_OSDFRAME_ST_AP = 0x40084000` and
`DS_OSDFRAME_END_AP = 0x4008A000` (dvd_dram_16m.h:246-247), i.e. exactly
**0x6000 = 24576 bytes = 84 lines at pitch 292**. Filling a 240-line window would
need 240*292 = 70080 B, running to 0x40095140 -- straight through
`DS_USERDATA_BUF_ST_AP`/`DS_AP_CODE_AREA` at 0x4008B000, i.e. over the AP's own
staged code. So the fix must be in the display configuration, not in the drawing.
(This also corrects the earlier note that the region is 24024 B; 24024 = 308*78 was
the GDI region's size, not the AP OSD allocation. 84 usable lines, not 78/82.)

Two candidate mechanisms, not yet distinguished:
  a) **Interlace / dual field.** PSCAN_EN (REG_DISP_SYNC_WH 0x1A3C bit28) is CLEAR
     = interlaced, and there are two OSD base registers (VCR20 = field 0,
     VCR21 = field 1) plus two near-identical setup blocks in the DISP code
     (0xa5760 / 0xa5814). If a progressive panel stacks the two fields instead of
     interleaving them, the buffer appears twice. Fix would be PSCAN_EN, or
     VCR21 = VCR20.
  b) **REG_DISP_N_LINE_REPEAT (0x1A64, "back to 1st line after access n lines")**,
     dumped as 0x02000000, wrapping the OSD fetch and re-showing the buffer.

**Method note worth keeping: the panel is now the diagnostic channel.** With legible
text at pitch 292 there is no need to infer the display config from photo
proportions or guess register semantics -- an AP can simply READ the registers and
PRINT them in hex on the screen. That closes the loop that made this whole display
bring-up so slow (no UART, no JTAG, emulator blind on the display axis). The
current banner does exactly that for VCR20/21/22/23, OSD_SIZE, OSD_POS, SYNC_WH,
N_LINE_REPEAT, TGEN and OSD_CR.

### 10.22 REAL display register values, read off the frame (both repeat theories dead)

The AP printed the live registers on the panel. Actual DP700WD values in AP mode:

| reg | value | decode |
|---|---|---|
| VCR20 (0xD80) | `40084000` | OSD base, field 0 |
| VCR21 (0xD84) | `40084000` | OSD base, field 1 -- **identical to VCR20** |
| VCR22 (0xD88) | `004E004D` | height 78 lines, width 77 units (77*4 = 308 B/line) |
| VCR23 (0xD8C) | `01340004` | Y increment 308, X increment 4 |
| OSD_SIZE (0x1A54) | `104E0268` | enable=1, height **78**, width **616 px** |
| OSD_POS (0x1A50) | `001C0066` | x=102, y=28 |
| SYNC_WH (0x1A3C) | `1014000A` | **PSCAN_EN SET => PROGRESSIVE** |
| N_LINE_REPEAT (0x1A64) | `02000000` | |
| TGEN (0x1A38) | `120D035A` | Vtotal 525, Htotal 858 |
| OSD_CR (0x1A58) | `0005001A` | |

**Both earlier repeat hypotheses are FALSIFIED:**
- Not interlace: PSCAN_EN (bit 28) is **set**, so the output is progressive. (Note
  the emulator showed `0014000A` -- bit clear -- another emulator/hardware
  divergence.)
- Not a dual-field split: VCR21 == VCR20, one base for both.
- The window is **616x78**, not the 720x240 the emulator reported, so "window much
  taller than the region" is also wrong.

**The pitch conflict is now confirmed against real silicon**, not inferred: every
register says the line stride is 308 bytes over 78 lines, yet text is only legible
when drawn at pitch **292**. So the DMA's real line advance is NOT `VCR23 >> 16`.
Something between the OSD read channel and the panel changes the effective advance
(candidates not yet checked: REG_DISP_H_REQ 0x1A08 "times to access DRAM per
line", REG_DISP_REDUNDANT 0x1A18 "redundant for 1st DRAM access", the H/V scaling
registers 0x1A1C/0x1A20/0x1A24, and VCR25 "OSD upscaling"). 292 stays an empirical
constant until this is explained.

Next probe: a ROW-INDEX RULER. Every 8th buffer row prints its own row number with
an alternating-colour tick. A photo then states the vertical mapping directly --
which rows appear where, how many panel lines per buffer row, and the repeat period
in BUFFER ROWS (do the numbers restart at 0, or continue past 84?) -- with no
proportional inference. This is the same "make the panel report facts, not
impressions" method that finally settled the pitch.

### 10.23 The repeat SOLVED: the 78-line window is painted twice, 157 lines apart

The row-index ruler settled it with one photo. Observed on the frame: the top band
showed row labels 0..72 and the bottom band **restarted at 0**, reaching ~48 before
the panel's last line.

Decoding that:
- Top band shows rows 0..77 -- **exactly the 78-line OSD window** (OSD_SIZE height
  = 78). The label at row 80 is outside the window, which is why it never appears.
  So the usable height is **78 rows, not the region's 84** (24576/292).
- The bottom band restarting at 0 means the SAME window is painted a second time.
  With the loader's `OSD_POS y = 28`: copy 1 = panel lines 28..105, copy 2 starts at
  233 - 48 = 185, so the **repeat period is 157 panel lines**. Copy 2's visible part
  (185..233) is exactly the observed "bottom band 0..48".

**Fix: raise OSD_POS y so copy 2 falls past the panel's last line (233).** Both
copies derive from the same window and move together, so `y = 95` puts copy 1 at
95..172 (fully visible) and copy 2 at 252 (off-screen), with ~18 lines of margin
against error in the measured period. Write:

```
    REG_DISP_OSD_POS (0x80001A50) = (95 << 16) | 102      /* keep loader's x=102 */
```

This is the ONLY display register worth writing from an AP, and it is the narrowest
possible change: position only -- no base, stride, size or timing -- and trivially
reversible (the loader's value is 0x001C0066). Everything else must still be left
alone.

Mechanism of the duplication itself remains unexplained (not interlace, not
dual-field, not an oversized window -- all ruled out in 10.22); 157 lines is
suspiciously close to Vtotal/2 - 105 and may be a panel-driver artifact of the
525-line NTSC timing feeding a 234-line progressive panel. Not needed for a usable
display, so recorded and left open.

**Consolidated, hardware-verified recipe for drawing from a bare-metal AP:**
```
    base   = 0x40084000     /* DS_OSDFRAME_ST_AP (2MB part)                    */
    pitch  = 292            /* MEASURED; registers claim 308 -- see 10.22      */
    rows   = 78             /* the OSD window height, not the region capacity  */
    4bpp, big-endian nibbles; off = y*292 + (x>>1); clamp to 24576 bytes
    palette (loader's): 0 = transparent key, 1 = yellow, 2 = white
    visible width 480 px
    write OSD_POS = (95<<16)|102 to drop the duplicate off-screen; touch no other
    display register
```

**Caveat on the OSD_POS fix -- do not move the window too far down.** With
`OSD_POS y = 95` the duplicate did vanish (single copy confirmed on the panel), but
the window's LOWER ROWS then sheared: rows 0..~43 rendered perfectly while rows 44+
smeared progressively. The identical drawing at the loader's `y = 28` was clean over
all four text lines, and nothing else changed -- same pitch, same buffer rows, same
code -- so pushing the window that far down evidently exceeds some OSD/line-buffer
limit in the display path (candidates: REG_DISP_LB_CR1/CR2 line-buffer control at
0x1A28/0x1A2C, or DRAM-bandwidth/H_REQ budget for a window that late in the frame).

Use the **minimum** y that still hides the duplicate: copy 2 sits at `y + 157`, so
`y >= 77` clears the last line (233). **`y = 80`** gives window 80..157 and copy 2
at 237 (off-screen by 4 lines) while sitting only 52 lines below the loader's
original position instead of 67.

Also worth building into any probe: a **full-height vertical bar** as a shear
detector. It is straight only if the pitch holds across every row of the window, so
it reveals both the presence and the starting row of any shear without needing text
to be legible.

### 10.24 The OSD's ABSOLUTE vertical limit: panel line ~139

Measured, and it explains everything seen so far. Two runs of the identical drawing
at different window positions sheared at the same **absolute panel line**:

| OSD_POS y | shear began at buffer row | = panel line |
|---|---|---|
| 95 | ~44 | 139 |
| 80 | ~60 | 140 |
| 28 (loader) | never (all 78 rows clean) | window ends at 105, above the limit |

So the OSD plane renders correctly only **above panel line ~139**; below it the
fetch shears progressively. This is an absolute limit in the display path, not a
window-relative effect, and it retro-explains why the loader's own y=28 window was
always clean and why "just move the window down" broke the lower rows.

**The two constraints conflict at the loader's window height:**
```
    hide the duplicate  ->  y + 157 > 233   ->  y >= 77
    stay above the limit ->  y + height - 1 < ~139
    with height = 78:   77 + 77 = 154  >  139   -> IMPOSSIBLE
```
Both hold only if the window is also made SHORTER. **y = 78 with height = 56**
gives window 78..133 (clear of the limit) and the duplicate at 235 (off-screen).
Cost: 56 usable rows instead of 78 -- which at 1x is still 7 lines x 60 columns,
and 1x is known legible here (the row-ruler labels were read off a photo at 1x).

Final AP display recipe, superseding 10.23's position advice:
```
    base 0x40084000, pitch 292, 4bpp, off = y*292 + (x>>1), clamp to 24576 B
    palette: 0 = transparent, 1 = yellow, 2 = white; visible width 480 px
    OSD_SIZE (0x1A54) = (old & ~0x0FFF0000) | (56 << 16)   /* keep enable+width */
    OSD_POS  (0x1A50) = (78 << 16) | 102
    rows 0..55 usable; touch no other display register
    (loader values to restore: OSD_POS 0x001C0066, OSD_SIZE 0x104E0268)
```
Cause of the ~139-line limit is not established (OSD line-buffer depth at
REG_DISP_LB_CR1/CR2 0x1A28/0x1A2C, or a DRAM-bandwidth/H_REQ budget, are the
candidates). Recorded as an empirical limit; not needed for a working console.

### 10.25 MicroPython on-screen, no keyboard, with an embedded investigative script

Milestone 2. The interpreter now runs inside the live firmware and routes every
`print()` to the frame's panel through the console retargeted to the confirmed
geometry (pitch 292, 4bpp @0x40084000, 56 rows -> **60x7 text cells**). Verified in
the emulator end to end: boots through the REAL AP loader, `mp_init()` +
`ct952.init()` succeed, the console reports "OSD console 480 x 56", and the embedded
script's six output lines render legibly on the OSD scanout.

Console specifics that matter:
- Glyphs are blitted as WHOLE BYTES: at 4bpp a glyph's x origin is always a
  multiple of 8, hence byte-aligned, so each 8-px row is exactly 4 bytes.
- Scroll is `memmove` by `8 * 292`, clamped inside the 24576-byte region.
- Text is drawn on index 0 (transparent) so the panel shows through.
- `console_setup()` programs ONLY the two window fields (OSD_SIZE height := 56
  preserving the enable bit and width, OSD_POS := (78<<16)|102). It no longer
  writes VCR20/21 or palette RAM -- both clobbered the loader's config on silicon.

**No keyboard required.** `pyapp_main()` runs an embedded script, so the frame is
useful standalone. Two bugs fixed getting there, both invisible in earlier work:
1. The previous staged-script hook probed a "PYSC" header at **0x40740000**, which
   is outside this part's 2 MB DRAM (0x40000000..0x40200000) -- an out-of-bounds
   read that only ever "worked" because the emulator mapped it.
2. This port builds at `MICROPY_CONFIG_ROM_LEVEL_MINIMUM`, where
   `MICROPY_PY_BUILTINS_STR_OP_MODULO` is **0**, so `"'%08X' % v"` raises
   TypeError. The first script version therefore printed NOTHING while still
   reporting that it had started. Hex is now hand-rolled from a digit string.
   Lesson: at MINIMUM ROM level, assume %-formatting, f-strings and most of the
   stdlib are absent.

The script dumps the registers that could explain the two open display puzzles
(the 292-vs-308 pitch, and the ~139-line shear limit): H_REQ, REDUNDANT, VSCALE,
HU/HD_SCALE, LB_CR1/CR2, VCR25 and MEM_LINE. Emulator values are mostly 0 (it does
not model them), which is precisely why this has to be read on the frame.

Also confirms the window writes take effect: the emulator run reported back
`SZ=003802D0` (height 0x038 = 56) and `POS=004E0066` (y=78, x=102).

### 10.26 The 292-vs-308 pitch EXPLAINED (line-buffer offset), and vertical scaling found

MicroPython ran on the frame with no keyboard and printed these live values (this
is the payoff of the on-screen console: registers the emulator does not model can
now be read off real silicon):

| reg | value | decode |
|---|---|---|
| IC (0x28C8) | `00000003` | |
| H_REQ (0x1A08) | `000400B4` | 180 DRAM accesses per line |
| REDUNDANT (0x1A18) | `00040008` | 8 |
| **VSCALE_CR (0x1A1C)** | **`08003BBB`** | **DISP_VSCALE_EN SET**, factor 0x3BBB |
| HU_SCALE (0x1A20) | `00008000` | unity, NEAREST_EN clear |
| HD_SCALE (0x1A24) | `00008000` | unity, **HD_EN clear** |
| **LB_CR1 (0x1A28)** | **`00300010`** | **low = 16**, high = 48 |
| LB_CR2 (0x1A2C) | `000002D0` | 720 (line-buffer width in px) |
| VCR25 (0xD94) | `00000000` | OSD upscaling off |
| MEM_LINE (0x1A68) | `00000000` | |
| OSD_SIZE | `10380268` | enable, height **56**, width 616 -- our write |
| OSD_POS | `004E0066` | y **78**, x 102 -- our write |

**1. The pitch discrepancy is accounted for exactly: `308 - 16 = 292`.** `LB_CR1`'s
low field is **16**, and the OSD's effective line advance is the VCR23 stride (308)
minus that 16. Horizontal scaling is ruled out as the cause -- both HU and HD read
unity with their enable bits clear, and VCR25 (OSD upscaling) is 0. This is a
numerically exact match rather than a proof; the decisive test is to alter LB_CR1's
low field and confirm the required drawing pitch moves with it. Until then, treat
292 as `VCR23_stride - LB_CR1_low` rather than a magic constant -- and note that any
firmware that programs LB_CR1 differently will need a different pitch.

**2. Vertical scaling is ON and was invisible until now.** `VSCALE_EN` (0x08000000)
is set with factor 0x3BBB = 15291. Reading 0x4000 as unity gives **0.9333**, so the
78-row window renders as ~72.8 panel lines -- which matches the ~74 lines measured
from the row-ruler photo. So OSD rows are NOT 1:1 with panel lines, and any future
vertical arithmetic (the 157-line repeat period, the ~139-line shear limit) must be
done in PANEL lines and converted, not assumed equal.

**3. Our two window writes are confirmed live on hardware:** OSD_SIZE height = 56
and OSD_POS = (78,102), exactly as programmed, with the enable bit and width
preserved.

Still open: the mechanism of the 157-line duplication, and the ~139-line shear
limit. `LB_CR1` high = 48 and `LB_CR2` = 720 are the remaining line-buffer knobs and
the natural place to look next, since a line-buffer limit is the most plausible
cause of a hard vertical cutoff.

**RETRACTION: the LB_CR1 explanation of the 292 pitch is FALSIFIED.** Clearing
LB_CR1's low field (16 -> 0) predicted the effective pitch would become 308. On
hardware the legible number was still **"292"**, so the pitch did NOT follow
LB_CR1. `308 - 16 = 292` was a coincidence. The pitch stays an EMPIRICAL constant;
do not present it as derived.

One caveat this test does not separate, and it should be closed cheaply rather than
assumed: the run proves "pitch unchanged after writing LB_CR1", which is consistent
with either (a) LB_CR1 not affecting the pitch, or (b) the write to LB_CR1 not
sticking at all (write-protected, shadowed, or re-driven by the DISP block). The
disambiguation is a one-line READBACK of LB_CR1 after writing it -- cheap enough to
ride along with any future on-frame script rather than costing its own flash.

So the 292-vs-308 gap remains unexplained. What IS excluded so far: horizontal
scaling (HU/HD unity, enables clear), OSD upscaling (VCR25 = 0), and now LB_CR1's
low field. Remaining candidates: H_REQ (0x1A08, = 180 DRAM accesses per line),
REDUNDANT (0x1A18, = 8), LB_CR1's HIGH field (48), LB_CR2 (720), or a fixed
hardware offset between the OSD read channel's fetch width (VCR22 low = 77 units =
308 B) and its line advance (measured 292 B = 73 units -- exactly 4 units less,
which may itself be the clue).

### 10.27 REPL on screen; USB keyboard blocked by the loader gating the USB clocks

The interpreter now reaches an interactive prompt on the panel: the emulator run
shows the MicroPython banner and `>>>` rendered on the OSD console with scrolling
working, in the 60x7 cell area. Whatever drives stdin, the REPL itself is on-screen.

**Found: the AP loader powers USB down before jumping to the AP.** `aploader.c:134-139`
runs, for the USB/servo source:
```
    HAL_PowerControl(HAL_POWER_USB, HAL_POWER_SAVE);
    USB_HCExit();
```
and HAL_POWER_SAVE sets `PLAT_UCLK48M_USB_DISABLE | PLAT_HCLK_USB_DISABLE`
(0x01000000 | 0x00800000) in `REG_PLAT_CLK_GENERATOR_CONTROL` at **0x80000300**
(hsystem.c:562-584, ctkav_platform.h:246/249/295-296). With those clocks gated the
EHCI register block is dead, so `usb_kbd_bringup()` sees no port connection and
returns 0 -- which is exactly the observed failure. The AP now clears both bits (the
documented inverse of HAL_POWER_NORMAL) and waits for the PHY before bringup.

**Status: still UNVERIFIED.** With the clocks re-enabled the emulator STILL reports
"no USB kbd", so either there is a second blocker (USB_HCExit() also tears down
controller state that needs re-initialising, and the port may need power via
HAL_WriteGPIO(USB_POWER_GRP, PIN_USB_POWER, 1) under SUPPORT_USB_POWER_BY_IO), or
the emulator's EHCI model is simply not reachable on the --apload path. The
emulator cannot settle this: it attaches a HID device ("[USBKBD] attached HID
keyboard on port 0") yet the driver never sees the port, and USB is one of the
axes it does not model faithfully. Do not claim keyboard support until a real
keyboard is seen working on the frame.

Degradation is safe: when bringup fails the REPL falls back to UART1 RX rather
than hanging, so the on-screen prompt still appears.

### 10.28 Trap 0x07 on the PYAPP seize SOLVED: the payload window is inside the framebuffer

The `trap 0x07 with ET=0` that killed every CT952_PYAPP seize is fully explained, and
the USB-keyboard REPL is working again in the emulator.

Diagnosis from the CPU state at the fault:
```
    pc=0x400c0004  npc=0x400c0008  tbr=0x40000060  (the FIRMWARE's trap base)
```
`tbr` still being the firmware's proves our entry code never ran -- `_tt` entry 0 is
`b _app_init`, so had it executed, npc would be `_app_init` and tbr ours. The CPU was
executing something that is not our payload.

**Root cause: the payload window collides with the MM framebuffer.** The payload
occupies 0x400c0000..~0x4015A378 (~620 KB), and
`DS_FRAMEBUF_ST_MM = 0x400A2000..0x401A2000` (dvd_dram_16m.h:99-100) covers all of
it. Once the firmware decodes a photo into that framebuffer, the payload is
overwritten, so a seize at the old default of icount 120M lands in image data.
Seizing at **22M** -- after boot and section load, before decode -- works.

Two further 8 MB-era leftovers were fixed on the way (the same class of bug as the
0x40500000 base):
- `npc` was hardcoded `0x40500004`, i.e. out of bounds on a 2 MB part, so control
  left the payload after a single instruction.
- the seize `%sp` was hardcoded `0x407F0000`, also out of bounds, which would fault
  the first window-overflow `std`.
Both now derive from the payload base / real DRAM (`mach_pyapp_base()`,
`mach_pyapp_sp()`, overridable via CT952_PYAPP_BASE / CT952_PYAPP_SP), and the
default seize icount is 22M with the reason recorded at the assignment.

**VERIFIED end to end in the emulator** -- the full stack, on the OSD console:
```
    usb_kbd: device VID=ceeb PID=0952 class=0 MPS0=64
    usb_kbd: configured, polling ep 0x81
    [pyapp] USB keyboard ready
    MicroPython v1.29.0-preview... on ct952 with sparc-v8-be
    >>> print(6*7)
    42
```
i.e. USB HID enumeration -> interrupt-endpoint polling -> keystrokes into the REPL
-> evaluated -> result rendered on the frame's own display at pitch 292.

Caveat for hardware: this validates the driver and the REPL path, NOT that a real
keyboard enumerates on the AP-loader path, where the loader has powered USB down
(10.27). The AP build clears the USB clock gates, but that is still untested on
silicon.

**Recurring lesson, third instance:** three separate 8 MB-era hardcoded addresses
(PYAPP base, npc, %sp) silently disabled or broke a working feature after the DRAM
size was corrected. When a capability regresses, grep the harness for absolute
addresses before doubting the model.

### 10.29 USB keyboard on the AP path: it was a HARNESS bug (machine_call froze devices)

Question that prompted this: "we have to run the usb init... why should it work?" --
correctly refusing a hardware test that had no positive evidence behind it.

Instead of guessing, the embedded script was made to READ the controller. Both paths
reported IDENTICAL EHCI state before any bringup attempt:
```
    CAP =01000010   (controller responds; HCIVERSION 1.0, CAPLENGTH 0x10)
    CMD =00000000   (not running yet -- expected, we reset it ourselves)
    STS =00001000   (HCHalted)
    PORT=00000003   (bit0 CCS = device CONNECTED, bit1 CSC = connect change)
    CLK =00000000   (USB clock gates clear)
```
So the controller was alive and a device was connected on the failing path too --
meaning the difference was never hardware state, USB power, or the firmware's USB
init.

**Root cause: `machine_call()` never called `machine_cycle()`.** Its loop was
`sparc_run(c, 1)` only, so for the entire duration of a machine_call every device
model was FROZEN -- the EHCI async schedule, VSYNC, the watchdog. The `--apload`
path runs the AP inside machine_call, so the emulated host controller never
processed a single transfer descriptor and `usb_kbd_bringup()` failed at its first
control transfer. Real silicon obviously does not stop its peripherals while the CPU
is inside a subroutine, so this was plain unfaithfulness.

Fixed: machine_call now steps `machine_cycle(m)` per instruction
(`CT952_CALL_NOCYCLE` restores the old behaviour as an escape hatch). Verified no
regression: the flash-write path produces byte-identical results with and without
the change.

**Result -- the keyboard now works on the SAME path that gets flashed:**
```
    usb_kbd: device VID=ceeb PID=0952 class=0 MPS0=64
    usb_kbd: configured, polling ep 0x81
    [pyapp] USB keyboard ready
    >>> print(6*7)
    42
```
So `USB_HCInit` does NOT need to be called: our driver's own EHCI init (HCRESET,
CONFIGFLAG, port reset, enumerate, SET_PROTOCOL boot) is sufficient, which is the
answer to "don't we have to run the USB init?" -- no, provided the clocks are
ungated (10.27), and that part is real and still required.

**Fourth harness artifact in a row** (8 MB base, npc, %sp, and now frozen devices in
machine_call). Every one of them looked like a hardware or firmware mystery. The
standing rule is now: before theorising about silicon, check whether the emulator is
even modelling the thing under test -- and prefer reading state off the device to
reasoning about it.

### 10.30 Keyboard fails on real hardware: almost certainly a LOW-SPEED device on an EHCI-only driver

On the frame the keyboard (Gearhead KB1500U, 89-key mini) is not detected, even
though the emulator now enumerates its modelled keyboard on the same --apload path.

**Leading explanation, and the emulator could never have caught it: USB keyboards are
LOW-SPEED (1.5 Mbps) devices, and our driver is EHCI-only.** EHCI addresses
high-speed (480 Mbps) devices only. A low- or full-speed device requires either
  (a) a companion host controller (OHCI or UHCI) sharing the same port, or
  (b) a high-speed HUB, whose transaction translator does the split transactions.
The emulator's HID model does not represent device SPEED at all -- it answers EHCI
transfers directly -- so an EHCI-only driver "works" there and cannot work on
silicon. This is a fidelity gap that predicts real failure, unlike the previous four
harness artifacts.

Two consequences worth testing in one flash, both now instrumented:
1. **PORTSC line status (bits 11:10)** after the port reset. `LS=1` (01b) means a
   LOW-SPEED device is attached -- confirmation. The AP now prints
   `usb1 PORTSC=`, `usb2 PORTSC=... LS=n`, and a specific `usb FAIL:` line naming the
   step that failed (no CCS / port not enabled / GET_DESCRIPTOR), instead of failing
   silently.
2. **Is there an OHCI companion?** The no-crutch boot inventory showed the firmware
   touching 0xa0001000, 0xa00010a4 and 0xa000112c, which fits an OHCI register block
   at 0xA0001000 (HcRevision +0x00 reads 0x00000010, HcControl +0x04,
   HcRhDescriptorA +0x48, HcRhPortStatus1 +0x54). The AP prints those four. If
   HcRevision reads ...10, the keyboard route is **OHCI, not EHCI**, and the driver
   needs an OHCI front end -- a real but well-understood piece of work.

Cheap workaround if OHCI is absent: plug the keyboard in through a **USB 2.0 hub**.
The hub enumerates at high speed and its transaction translator lets the existing
EHCI driver reach the low-speed keyboard behind it.

### 10.31 ROOT CAUSE of the keyboard failure: wrong USB register map (ChipIdea, not bare EHCI)

Hardware readout that cracked it, with a keyboard plugged in:
```
    usb FAIL: no CCS (nothing connected)
    PORTSC=00000000 LS=0
    oREV=0e6facda oPRT=000122ff        <- garbage, so no OHCI at 0xA0001000
```
`PORTSC` reading 0 while the firmware demonstrably enumerates the same keyboard (the
splash screen takes visibly longer whenever USB is connected) meant we were reading
the wrong register, not that the port was empty.

**The USB core is a ChipIdea/Freescale-style USB 2.0 OTG controller, NOT a bare EHCI
with a 0x10-byte capability block.** Proven from the firmware's own accesses -- the
only three the disassembler could resolve, and all three fit exactly one layout:

| firmware access | ChipIdea meaning | offset from OP base 0xA0000140 |
|---|---|---|
| `0xA0000184` read + `btst 1` | **PORTSC1**, bit 0 = CCS | +0x44 |
| `0xA00001A4` read + `and 0x200` | **OTGSC**, VBUS valid | +0x64 |
| `0xA0000164` `\|= 0x7f0000` | **TXFILLTUNING** | +0x24 |

So HCCAPBASE is at 0xA0000100 with **CAPLENGTH = 0x40**, putting the operational
registers at **0xA0000140**, not 0xA0000110. Our driver assumed CAPLENGTH = 0x10 and
had every operational register **0x30 too low** -- it polled 0xA0000154 as PORTSC,
which is really PERIODICLISTBASE and reads 0. Hence "nothing connected" with a
keyboard attached.

Corrected map (verified: the AP now prints `HCCAP=01000040 caplen=40 op=a0000140`):
```
    0xA0000100 HCCAPBASE (CAPLENGTH=0x40)   0xA0000180 CONFIGFLAG
    0xA0000140 USBCMD                       0xA0000184 PORTSC1
    0xA0000144 USBSTS                       0xA00001A4 OTGSC
    0xA0000154 PERIODICLISTBASE             0xA00001A8 USBMODE
    0xA0000158 ASYNCLISTADDR
```
Two fixes in the driver: derive the operational base from CAPLENGTH instead of
assuming it, and set **USBMODE.CM = 3 (host)** -- a ChipIdea core comes out of reset
in DEVICE mode and will never report a connection until told to be a host, which
`USB_HCInit` normally does and we were replacing.

**The low-speed theory (10.30) was wrong and is retracted.** A ChipIdea core drives
full- and low-speed devices itself, with no companion controller and no hub
transaction translator. `oREV=0e6facda` also confirms there is no OHCI at
0xA0001000. So no hub is needed -- that advice was a red herring.

**The emulator was validating the bug.** Its EHCI model also used CAPLENGTH = 0x10,
so it agreed with the wrong driver and reported success. Both are now on the real
layout (CAPLENGTH 0x40, operational registers at +0x40..+0x84), so the emulator can
catch this class of error instead of confirming it. Verified after the change:
HID enumerated, `>>> print(6*7)` -> `42`, `PORTSC=00000005`, `CAP=01000040`.

Method note: the winning move was reading three register addresses out of the
firmware's own instruction stream and asking which single documented layout explains
all three -- not probing candidate bases.
