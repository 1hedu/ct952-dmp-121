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
