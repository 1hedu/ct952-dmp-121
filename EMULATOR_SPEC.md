# CT952/CT909P Emulator Specification

## Overview
This document specifies how to build a QEMU-based emulator for the Actions Semiconductor CT952 (CT909P) digital picture frame chip.

**WHY THIS IS FEASIBLE:** We have the complete firmware source code, which makes emulator development significantly easier than reverse engineering binaries alone.

## Architecture

### CPU
- **Architecture**: MIPS32 (likely little-endian based on Actions Semiconductor heritage)
- **Cores**: Dual-core
  - PROC1 (Primary): Main application processor
  - PROC2 (Secondary): Audio/media co-processor
- **Clock**: 133 MHz (configurable)
- **ISA**: MIPS32 Release 1 or 2

### Memory Map

```
Address Range         | Size    | Description
----------------------|---------|------------------------------------------
0x00000000-0x???????? | ???     | Boot ROM (internal)
0x40000000-0x40FFFFFF | 16MB    | DRAM (CT909_DRAM_START, configurable 16/32/64MB)
0x80000000-0x80002FFF | 12KB    | Memory-mapped I/O (peripherals)
0xB0000000-0xB00007FF | 2KB     | Internal SRAM
0xBFC00000-0xBFFFFFFF | 4MB     | SPI Flash (bootable, mapped)
```

**Note**: CT909_DRAM_START likely = 0x40000000 (standard MIPS KSEG0 uncached)

### DRAM Layout (16MB Configuration)

Based on `dvd_dram_16m.h`:

```
Offset from DRAM_START | Size    | Purpose
-----------------------|---------|---------------------------
0x000000 - 0x001800    | 6KB     | Reserved
0x001800 - 0x002000    | 2KB     | NV Buffer / USB Buffer
0x002000 - 0x01CF00    | 114KB   | PROC2 code space
0x04C000 - 0x04F000    | 12KB    | UNZIP buffer
0x04F000 - 0x05C000    | 52KB    | PCM audio buffer
0x05C000 - 0x065000    | 36KB    | OSD font table + OSD frame
0x065000 - 0x183800    | 1.11MB  | Video frame buffers (3x)
0x184800 - 0x1D2800    | 312KB   | Audio/Video stream buffers
0x1D2800 - 0x1E9000    | 89KB    | FW buffer, SP buffers
0x1E9000 - 0x1F0000    | 28KB    | AVI index, misc
0x1F0000 - 0x200000    | 64KB    | USB memory pool
```

### Peripheral Register Map

All registers are 32-bit DWORD aligned at base `IO_START = 0x80000000`

#### Platform Registers (0x80000000 - 0x800001FF)

```c
0x8000000C  AHB Failing Address
0x80000010  AHB Status
0x80000014  Cache Control
0x80000018  Power Down
0x8000001C  Write Protection 1
0x80000020  Write Protection 2
0x80000024  Configuration

// Timers
0x80000040  Timer1 Counter
0x80000044  Timer1 Reload
0x80000048  Timer1 Control
0x8000004C  Watchdog
0x80000050  Timer2 Counter
0x80000054  Timer2 Reload
0x80000058  Timer2 Control
0x80000060  Prescaler Counter
0x80000064  Prescaler Reload
0x80000068  Timer3 Control
0x8000006C  Timer3 Value

// UARTs
0x80000070  UART1 Data
0x80000074  UART1 Status
0x80000078  UART1 Control
0x8000007C  UART1 Scaler
0x80000080  UART2 Data
0x80000084  UART2 Status
0x80000088  UART2 Control
0x8000008C  UART2 Scaler

// Interrupts
0x80000090  INT Mask/Priority
0x80000094  INT Pending
0x80000098  INT Force
0x8000009C  INT Clear
0x800000B0  PROC1 1ST INT Mask Enable
0x800000B4  PROC1 1ST INT Pending
0x800000B8  PROC1 1ST INT Status/Clear
0x800000BC  PROC1 1ST INT Mask Disable
0x800000D0  PROC1 2ND INT Mask Enable
0x800000D4  PROC1 2ND INT Pending
0x800000D8  PROC1 2ND INT Status/Clear
0x800000DC  PROC1 2ND INT Mask Disable

// DSU Debug UART
0x800000C4  DSU UART Status
0x800000C8  DSU UART Control
0x800000CC  DSU UART Scaler
```

#### Audio Interface Unit - AIU (0x80000400)
```c
0x80000400  AIU Enable
0x80000404+ UPK registers (unpacker for audio streams)
```

#### Bus Interface Unit - BIU (0x80000800)
```c
0x80000800  BIU Control
```

#### MCU (0x80000880)
```c
0x80000880  MCU Configuration Register (MR0)
... (MCU memory controller registers)
```

#### Display Controller (0x80001A00)
```c
0x80001A00  Main Frame 1 Control
0x80001A04  Main Frame 2 Control
0x80001A08  OSD Control 1
0x80001A0C  OSD Control 2
... (display output, sync, framebuffer addresses)
0x80001C00  Gamma/OSD Palette RAM (1KB window)
```

#### TV Encoder - TVE (0x80001880)
```c
0x80001880+ TVE registers (PAL/NTSC video output)
```

#### Sub-Picture Unit - SPU (0x80001900)
```c
0x80001900+ SPU registers (DVD subtitles, OSD)
```

#### Video Decoder - VLD (0x80002080)
```c
0x80002080  VLD registers (MPEG1/2/4, H.264 decoder)
```

#### JPEG Decoder (0x80002268)
```c
0x80002268  JPEG Data 1
0x8000226C  JPEG Data 2
0x80002270  JPEG Data 3
0x80002274  JPEG Data 4
0x80002278  JPEG Huffman RAM
0x80002290  JPEG Quantization Memory
```

#### GPU/JPU (0x80002880)
```c
0x80002880  GPU/JPU Control
... (2D graphics acceleration, JPEG encoding)
```

#### ATAPI/Transport Stream (0x80002980)
```c
0x80002980  ATAPI Command Control / TS NIM Config
... (DVD drive interface OR DVB tuner)
```

#### Flash Controller (0x80002A00)
```c
0x80002A00  PROM Config
0x80002A24  SPI Command (CT909P)
0x80002A28  SPI Operation
0x80002A2C  SPI Read Type
0x80002A30  SPI Write
0x80002A34  SPI Read
0x80002A38  SPI SCLK Control
```

### Interrupt System

#### Primary Interrupt Vector (INT_MASK_PRIORITY @ 0x80000090)
```c
Bit 1   INT_AHB_ERROR       AHB bus error
Bit 2   INT_UART2           UART2 interrupt
Bit 3   INT_UART1           UART1 interrupt
Bit 6   INT_TIMER_SPORT     Timer/SPORT (CT909P)
Bit 7   INT_PROC2_1ST       PROC2 first-level (CT909P)
Bit 8   INT_TIMER1          Timer1
Bit 9   INT_TIMER2          Timer2
Bit 10  INT_PROC1_2ND       Second-level interrupt
Bit 11  INT_SOFTWARE        Software interrupt
Bit 12  INT_IR              IR receiver (CT909P)
Bit 13  INT_PROC1_1ST       First-level interrupt
```

#### Second-Level Interrupts (INT_PROC1_2ND)
```c
Bit 0   USB_OHCI            USB host controller
Bit 1   SERVO               DVD servo
Bit 2   IR                  IR receiver (older chips)
Bit 3   STBBUF_UNDERFLOW    STB buffer underflow
Bit 4   BIU                 BIU interrupt
Bit 5   MCU_BSRD            MCU bitstream read
Bit 6   MCU_ECCRD           MCU ECC read
... (video/audio buffer over/underflows)
Bit 21  VPU                 Video processing unit
Bit 24  FCR                 Flash Card Reader (CT909P)
Bit 25  NFC                 NAND Flash Controller (CT909P)
```

### Boot Sequence

1. **Reset Vector**: 0xBFC00000 (typical MIPS boot ROM)
2. **Boot ROM**: Internal ROM initializes hardware
3. **SPI Flash**: Bootloader loads from SPI flash @ 0xBFC00000
4. **DRAM Init**: Memory controller configured
5. **Code Load**: Main firmware loaded to DRAM @ 0x40002000 (PROC2) and higher
6. **Jump**: Execute from DRAM

### SPI Flash Layout (Typical)

```
Offset      | Size  | Content
------------|-------|----------------------------------
0x00000000  | 4KB   | Boot loader
0x00001000  | 16KB  | Logo bitmap
0x00005000  | ~1MB  | Main firmware (PROC1 + PROC2)
0x00100000  | ~4MB  | Resources (fonts, strings, UI)
0x00500000  | Rest  | User data / settings
```

## Emulation Strategy

### Phase 1: Minimal Boot
**Goal**: Boot to serial console

Required components:
- MIPS CPU core (QEMU has mips32el)
- 16MB RAM @ 0x40000000
- UART @ 0x80000070 (output only initially)
- Timers @ 0x80000040-0x8000006C
- Basic interrupt controller
- SPI flash @ 0xBFC00000

**Test**: Load `boot.bin` + `DVD909.rom`, see if we get UART output

### Phase 2: Display Output
**Goal**: Show splash screen

Add:
- Display controller @ 0x80001A00
- Framebuffer mapping
- TV encoder (basic)
- JPEG decoder (or stub)

**Test**: Load logo, display on virtual screen

### Phase 3: User Input
**Goal**: Respond to IR remote

Add:
- IR receiver @ INT_IR
- GPIO for panel buttons
- Timer interrupts

**Test**: Backdoor sequence (Vol Up/Down/Up/Down)

### Phase 4: Storage
**Goal**: Read from SD card / USB

Add:
- SD/MMC controller
- USB OHCI controller
- File system access from host

**Test**: Browse photos, play media

### Phase 5: Full System
**Goal**: Run Python!

Add:
- Network interface (emulated)
- Custom bootloader to load Linux
- Framebuffer console

**Test**: Boot buildroot Linux, run Python scripts

## QEMU Implementation

### File Structure
```
hw/mips/ct952_dmp.c           - Machine definition
hw/char/ct952_uart.c          - UART implementation
hw/timer/ct952_timer.c        - Timer implementation
hw/intc/ct952_intc.c          - Interrupt controller
hw/display/ct952_disp.c       - Display controller
hw/block/ct952_spi.c          - SPI flash controller
include/hw/mips/ct952.h       - Register definitions
```

### Minimal Machine Init (Pseudocode)

```c
static void ct952_init(MachineState *machine)
{
    MIPSCPU *cpu;
    MemoryRegion *ram, *io, *sram, *flash;

    // Create CPU
    cpu = MIPS_CPU(cpu_create(machine->cpu_type));

    // RAM 16MB @ 0x40000000
    ram = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram, NULL, "ct952.ram", 16 * 1024 * 1024);
    memory_region_add_subregion(get_system_memory(), 0x40000000, ram);

    // SRAM 2KB @ 0xB0000000
    sram = g_new(MemoryRegion, 1);
    memory_region_init_ram(sram, NULL, "ct952.sram", 2048);
    memory_region_add_subregion(get_system_memory(), 0xB0000000, sram);

    // I/O region @ 0x80000000
    io = g_new(MemoryRegion, 1);
    memory_region_init_io(io, NULL, &ct952_io_ops, NULL, "ct952.io", 0x3000);
    memory_region_add_subregion(get_system_memory(), 0x80000000, io);

    // SPI Flash @ 0xBFC00000
    flash = g_new(MemoryRegion, 1);
    memory_region_init_rom(flash, NULL, "ct952.flash", 16 * 1024 * 1024);
    memory_region_add_subregion(get_system_memory(), 0xBFC00000, flash);

    // Load firmware
    load_image_targphys("DVD909.rom", 0xBFC00000, 16 * 1024 * 1024);

    // Create peripherals
    ct952_uart_create(0x80000070);
    ct952_timer_create(0x80000040);
    ct952_intc_create(0x80000090);
}
```

### UART Implementation (Pseudocode)

```c
static uint64_t ct952_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    CT952UartState *s = opaque;

    switch (addr) {
    case 0x00: // UART_DATA
        return serial_getchar(s->serial);
    case 0x04: // UART_STATUS
        return s->status;
    case 0x08: // UART_CONTROL
        return s->control;
    case 0x0C: // UART_SCALER
        return s->scaler;
    }
    return 0;
}

static void ct952_uart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CT952UartState *s = opaque;

    switch (addr) {
    case 0x00: // UART_DATA
        serial_putchar(s->serial, val);
        break;
    case 0x04: // UART_STATUS
        s->status = val;
        break;
    case 0x08: // UART_CONTROL
        s->control = val;
        break;
    case 0x0C: // UART_SCALER
        s->scaler = val;
        break;
    }
}
```

## Building & Running

### Build QEMU with CT952 Support

```bash
git clone https://github.com/qemu/qemu.git
cd qemu
mkdir build && cd build

# Add CT952 machine files to source tree first
../configure --target-list=mipsel-softmmu
make -j$(nproc)
```

### Run Emulator

```bash
./qemu-system-mipsel \
    -M ct952 \
    -cpu mips32r2 \
    -m 16M \
    -kernel DVD909.rom \
    -serial stdio \
    -nographic
```

Or with display:

```bash
./qemu-system-mipsel \
    -M ct952 \
    -cpu mips32r2 \
    -m 16M \
    -kernel DVD909.rom \
    -serial stdio \
    -device ct952-display \
    -sdl
```

## Testing Approach

### 1. Trace Execution
```bash
# Enable QEMU logging
./qemu-system-mipsel -M ct952 -d in_asm,int,cpu_reset -D qemu.log
```

### 2. GDB Debugging
```bash
# In one terminal
./qemu-system-mipsel -M ct952 -s -S

# In another terminal
mipsel-linux-gnu-gdb DVD909.rom
(gdb) target remote :1234
(gdb) break *0xBFC00000
(gdb) continue
```

### 3. Monitor Registers
Since we have the source code, we can:
- Add debug prints to firmware
- Create register trace tools
- Compare emulator vs. real hardware

## Advantages of Emulation

✅ **Safe Testing**: No risk of bricking hardware
✅ **Fast Iteration**: Rebuild & test in seconds
✅ **Perfect Debugging**: GDB, tracing, breakpoints
✅ **Snapshot/Restore**: Save states, time-travel debug
✅ **Network Boot**: Easy to test Linux/Python
✅ **Source Code**: We have the firmware - can add instrumentation
✅ **No Hardware Needed**: Develop before IR receiver soldering
✅ **CI/CD**: Automated testing possible

## Next Steps

1. **Extract more register info** from all `ctkav_*.h` files
2. **Determine MIPS variant** (check `*.ld` linker scripts)
3. **Find boot ROM** behavior (may need to reverse `boot.bin`)
4. **Implement minimal UART** for "Hello World"
5. **Test with actual firmware** images
6. **Add peripherals incrementally**
7. **Boot Linux in emulator**
8. **Test Python execution**
9. **Validate against real hardware** (once IR soldered)

## Resources for QEMU Development

- QEMU MIPS Documentation: https://qemu.readthedocs.io/en/latest/system/target-mips.html
- Adding New Machines: https://qemu.readthedocs.io/en/latest/devel/qom.html
- MIPS Architecture: See MIPS32 spec
- Similar Projects:
  - OpenWrt QEMU targets
  - MIPS Malta machine (reference)
  - PIC32 emulator (similar embedded MIPS)

---

**This emulator approach could be MUCH faster than hardware hacking!**

We can develop everything in emulation, then flash to real hardware once we know it works. The source code is a goldmine - we can instrument it, understand boot flow, and build a perfect emulator.

Let's do this! 🚀
