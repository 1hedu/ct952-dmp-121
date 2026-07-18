# Coby DP700WD / Cheertek CT952 — Verified Hardware & Hacking Reference

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
