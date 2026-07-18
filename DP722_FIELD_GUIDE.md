# Coby DP722 / Cheertek CT952 — SDK Teardown & Hacking Field Guide

> **Device correction:** the frame in hand is a **Coby DP700WD** (7", 480×234,
> JPEG-only), not a DP722. Same Cheertek CT9xx platform and `DMP_121` firmware —
> only the panel resolution and the (absent) video path differ. Everything below
> still applies. For the **source-verified deep reference** — exact register
> addresses, the full UART monitor command/opcode tables, the serial-reflash
> protocol, and the `.AP` image format — see **`DP700WD_HW_REFERENCE.md`**, which
> supersedes and adds hard `file:line` evidence to this orientation.

A reverse-engineering orientation to the firmware source in this repo, written
toward one goal: **running your own code (ideally Python/MicroPython) on the
frame, and using the USB port for something other than photo storage.**

Everything below is derived from the source and binaries in this tree.

---

## 1. What this thing actually is

| Property | Value | Evidence |
|---|---|---|
| Product | Coby DP722 digital photo frame | your ID; config `DMP_121` |
| Firmware | Cheertek **"WinAV"** firmware | `README.TXT`, `Winav.h` |
| Build tag | `DMP_SW_VERSION = 121`, base `SW_VERSION = 278` | `dmpcustm.h`, `customer.h` |
| SoC | Cheertek **CT952** (CT909P / CT950/952/955/956 family) | `vd/Regtable_CT952.H` et al. |
| CPU | **SPARC V8, 32-bit, big-endian** | ELF headers of `*.o` (`Machine: Sparc`, `MSB`) |
| Clock | ~133 MHz default (100–162 MHz configs) | `internal.h` `CPU_SPEED CPU_133M` |
| RTOS | **eCos 2.0.1** | `libtarget.a` path `/ecos-c/ct909/ecos_build/ct909_release_install_2.0.1` |
| DRAM | 16 / 32 / 64 Mbit configs | `dvd_dram_*.h`, `romcfg_*` |
| Storage | Serial **SPI flash** 4/8/16 Mbit | `spflash.c`, `romcfg_*_flash.txt` |
| Toolchain | GCC SPARC + `makerom.exe`, `APPacker.exe`, `extend909.exe` | build `.bat`/`Makefile` |

The CT9xx family is a "DVD/media-player-on-a-chip": MPEG1/2 + JPEG decode,
display/scaler, TV encoder, optical-disc servo, USB, card reader — all on one
SPARC SoC. The DP722 is the **DMP** (Digital Media Player) cut: the disc/servo
path is dropped, and it keeps hardware **JPEG decode → TFT panel** plus
**USB + SD** as photo sources.

`CPU_SPARC` in `platform.h` is the real ISA (the `IO_SPARC` "8051 bus" comment
is legacy naming for the on-chip peripheral bus, not an 8051 core).

---

## 2. Boot & firmware image layout

Runtime memory map (from `DVD909.ld`):

```
rom  : 0x00002000  (SPI-flash mapped, .text/.rodata run in place)
ram  : 0x40000000  (DRAM; vectors, data/bss, and copied-in hot code)
       0x4001D000  .text_dram  — perf-critical code copied to writable DRAM
                                  (eCos libtarget_ram, usbdi, interrupt,
                                   spflash, pardram, dec_dram)
```

Flash image pieces (see `upgcfg.txt`, `makerom.exe`, `rominfo.c`):

- `boot.bin` / `dsu_boot*.rom` — first-stage boot (4072 B).
- `flash.bin` — **compressed main CODE**, loaded to `0x40001000`, program entry.
- `dvd909.rom` — decoder/firmware ROM payload.
- `rominfo.bin` — 45-byte INFO header @ `0x40000b00`.

**Field-update image = `.AP` file** (`UPG952A.AP`, ~1.3 MB), built by
`APPacker.exe`. `aploader.c` parses `AP_INFO`, runs an **IC-version check**
(`_AP_IC_Version_Check`) and **checksum** (`_AP_CheckSum`), then programs SPI
flash. Delivered via USB stick or SD card. `flash_dsu_main.upgrade` is a packed
example.

---

## 3. The USB port — what it does today, and why "not storage" is hard

Controller: on-chip **USB OTG** (host **and** device). Stack is a licensed
binary blob — `usb/usb.a`, `usbdi.o`, `jos_mem.o` (the `jos_` = "Jungo OS"
layer strongly suggests **Jungo USBware**). No stack source ships here.

The firmware uses the port in exactly **two roles, both storage**:

1. **Host mode** (normal): enumerates USB **mass-storage** thumb drives, reads
   FAT via `info.a`, displays photos. `USB_HCInit`, `USB_ReadData`,
   `USBD_SBC_Read10`.
2. **Device mode (OTG)**: `USBSRC_SetOTGMode`, `USBSRC_CMD_SWITCH_USB_STACK`,
   `USBSRC_CMD_DEVICE_MODE_CHK_CARD` — the frame becomes a **USB Mass Storage
   gadget** (card-reader) so a PC can read the inserted SD card.

**Only the MSC class is implemented.** There is no CDC-serial, HID, or custom
class anywhere. To make the OTG port do "something else" you must author a new
device-class gadget on top of `usbdi` — but that's a binary blob against an
undocumented UDC register set. This is the single hardest path in the whole
project. **If you want a non-storage data link, the UART (below) is the sane
door, not the OTG port.**

---

## 4. Ways to run your own code (ranked by effort)

### 4.1 UART serial debug monitor — THE way in
`debug.c` compiles a full RS-232 monitor (`SERIAL_DEBUG` + `DBG_MINI_PRINTF`)
on `HAL_UART1`. It emits boot/debug logs **and** accepts an interactive command
set (parsed in `_DBG_ProcessCommand`, dispatch at `debug.c:1340`):

| Command | Effect |
|---|---|
| `DDR addr [n]` | **Dump DRAM** |
| `MDR / MDRA / MDRO addr val [n]` | **Write / AND / OR DRAM** — arbitrary memory poke |
| `RD addr n` / `RM[A/O] addr val` | Read / modify **MMIO registers** — full hardware control |
| `K xx` | **Inject an IR remote key** — drive the UI programmatically |
| `C…` | I²C read/write |
| `P[M/S/R/A] …` | Toggle subsystem debug flags |
| binary `WRITE_DATA_CMD` | Stream a blob → `SPF_WriteProgram` → **reflash SPI over serial** |
| binary `READ_DRAM_CMD` / `WRITE_DRAM_CMD` | Bulk DRAM up/download |

Because hot code lives in **writable DRAM** (`.text_dram @ 0x4001D000`) and you
have DRAM-write + register-write + a data-download path, this is effectively
**arbitrary code execution**: poke a routine/callback/vector and it runs (there
is no explicit `GO`, but `MDR` into a live function pointer achieves it). There
is a matching host-side "Serial Debug Tool," and an `_bAutoUpgrade` loop.

**Caveat:** retail `RELEASE_SETTING` builds *may* strip the RX command half.
The `DBG_MINI_PRINTF` boot log is usually still on. First real-world step:
**find the UART pads on the PCB**, attach a 3.3 V USB-TTL adapter, and watch for
a boot log. If you get the prompt, you own the box.

### 4.2 External SPI-flash reprogram (durable)
`spflash.c` exposes `SPF_ReadID / ReadData / EraseSector / EraseChip /
WriteProgram`. Clip the SOIC-8 flash, dump it as ground truth, map it against
`makerom`/`upgcfg`/`rominfo`, modify `flash.bin`, reflash. Bricking is
recoverable because the programmer is external. This is the safety net for
every other path.

### 4.3 Custom `.AP` update file (OTA-style)
Rebuild the container (INFO + zipped CODE + ROM, checksum, IC-version gate) with
`APPacker.exe` or a reimplementation, drop it on USB/SD, let `aploader.c` flash
it. Cleanest no-solder path *if* you satisfy the version/checksum checks.

### 4.4 Backdoor / engineering menu (recon only)
`backdoor.c` is a hidden service overlay reached by a remote-key sequence; shows
SW versions, region control, and on-screen debug modes. Not code-exec, but great
for recon and toggling debug output.

---

## 5. The MicroPython dream — honest assessment

The frame is **easily powerful enough** (SPARC V8 @ ~133 MHz, 16–64 Mbit DRAM).
MicroPython is C99 and runs fine on 32-bit big-endian. The blockers are
**architecture + toolchain + drivers**, not horsepower:

- **No upstream SPARC port of MicroPython exists.** You'd add a bare-metal
  SPARC V8 (big-endian) port dir: startup/stack, `mp_hal` stubs, and a UART for
  the REPL. This is a genuine port, but exactly the kind MicroPython is built
  for — days-to-weeks, not research-grade.
- **You need `sparc-elf-gcc`.** The SDK implies one (the eCos build); reconstruct
  or obtain it to build anything.
- **Drivers come as register pokes, not APIs.** `decoder.a`, `display.a`,
  `usb.a` are blobs, but `vd/Regtable_CT952.H`, `tft.c`, `TFTSetup.c`, `gdi.c`,
  `hal.c`, `rtcdrv.c`, `spflash.c` document panel init/backlight, OSD
  framebuffer, GPIO/keys, RTC, SPI-flash, and I²C — enough to build a
  MicroPython HAL that lights the panel and reads buttons.

**Two feasible shapes:**

- **(a) Co-resident REPL blob — recommended first.** Don't replace the firmware.
  Use the §4.1 serial monitor to upload a small MicroPython image into DRAM and
  jump to it; expose the REPL over the same UART; drive the panel by poking the
  display registers learned from `tft.c`/`vd/`. Fast, reversible, no flashing.
- **(b) Replacement firmware.** Reflash `flash.bin` with a MicroPython image for
  full control — bricking risk, requires the §4.2 external-programmer recovery.

**"USB port for not-storage" from Python:** same wall as §3 — no gadget-class
source. The practical "other use" of a data port here is the **UART**, not the
OTG connector.

---

## 6. Concrete roadmap

1. **Hardware recon.** Find UART TX/RX/GND pads; attach 3.3 V USB-TTL; capture
   the boot log → confirm `SERIAL_DEBUG` is live and read the baud.
2. **Exercise the monitor.** If it responds: `DDR`/`MDR` to dump flash-mapped ROM
   (`0x2000+`) and DRAM; `RD` to read the display/USB register blocks; `K` to
   drive the UI. This alone lets you "know all about it."
3. **Dump SPI flash externally** as ground truth; map against
   `makerom`/`upgcfg`/`rominfo`.
4. **Rebuild the toolchain** (`sparc-elf-gcc`) and reproduce a build from this
   SDK — prove you can make images the flash/AP loader accepts.
5. **MicroPython:** start with the co-resident UART REPL (§5a), HAL from the
   register tables; grow to panel + buttons; later consider a replacement image.

---

## 7. Notable supporting files

- `python/` — **host-side** analysis tools (Python 2.7 + PIL): decode the OSD
  BMP resources and the OSD **YUV palette** format. Not on-device code.
- `allstr.h`, `OSDString/` — all OSD strings + fonts (localization).
- `LOGO.TXT`, `logo*.bin` — **boot logo** (an easy, low-risk first mod).
- `vd/Regtable_CT952.H`, `ViporFunc.c`, `tft.c`, `TFTSetup.c` — panel/scaler/TV
  encoder register programming (your future display HAL).
- `hal.c`/`hal.h`, `spflash.c`, `rtcdrv.c`, `interrupt.c` — the closest thing to
  a driver layer you'll reuse for a port.

---

*Facts here come from static reading of this SDK; on-hardware behavior
(especially whether `SERIAL_DEBUG` survived in the retail build) must be
confirmed on the bench.*
