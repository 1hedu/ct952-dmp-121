# Build a CT952 Emulator - Quick Start Guide

## Why Emulation is Brilliant for This Project

✅ **Test firmware safely** - no risk of bricking hardware
✅ **Faster iteration** - rebuild & test in seconds
✅ **Perfect debugging** - GDB, tracepoints, register dumps
✅ **No hardware needed** - start NOW without soldering IR receiver
✅ **Test Python first** - boot Linux & run Python before flashing real device
✅ **We have SOURCE CODE** - massive advantage for emulator development

## What We Have (Goldmine!)

From analyzing your firmware source, we have **everything needed** for an emulator:

### 1. Complete Memory Map
- **DRAM**: 0x40000000 (16/32/64MB configurable)
- **I/O Registers**: 0x80000000-0x80002FFF (every peripheral mapped!)
- **SRAM**: 0xB0000000 (2KB internal)
- **ROM**: 0x00002000 (firmware code)
- **Flash**: 0xBFC00000 (boot)

### 2. Every Hardware Register Documented
We have complete register definitions for:
- UART (0x80000070, 0x80000080)
- Timers (0x80000040-0x8000006C)
- Interrupts (0x80000090-0x800000DC)
- Display (0x80001A00)
- USB (0x800028xx)
- SPI Flash (0x80002A00)
- ...and 20+ more peripherals!

### 3. Firmware Binaries
- `DVD909.rom` - main firmware (1.3MB)
- `boot.bin` - bootloader (4KB)
- Binary files for JPEG, MPEG, audio codecs

## Quick Build with QEMU

### Step 1: Clone QEMU
```bash
git clone https://github.com/qemu/qemu.git
cd qemu
```

### Step 2: Create CT952 Machine Definition

Create `hw/mips/ct952.c`:

```c
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/mips.h"
#include "hw/mips/cpudevs.h"
#include "hw/char/serial.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"

static void ct952_init(MachineState *machine)
{
    MIPSCPU *cpu;
    MemoryRegion *ram, *rom, *io, *sram;
    qemu_irq *irq;

    /* CPU: MIPS32 little-endian */
    cpu = MIPS_CPU(cpu_create(machine->cpu_type));
    irq = mips_cpu_create_clock(cpu);

    /* RAM @ 0x40000000 */
    ram = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram, NULL, "ct952.ram",
                          16 * 1024 * 1024, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                               0x40000000, ram);

    /* ROM @ 0x00002000 for firmware */
    rom = g_new(MemoryRegion, 1);
    memory_region_init_rom(rom, NULL, "ct952.rom",
                          32 * 1024 * 1024, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                               0x00002000, rom);

    /* SRAM @ 0xB0000000 */
    sram = g_new(MemoryRegion, 1);
    memory_region_init_ram(sram, NULL, "ct952.sram",
                          2048, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                               0xB0000000, sram);

    /* I/O region @ 0x80000000 */
    io = g_new(MemoryRegion, 1);
    memory_region_init_io(io, NULL, NULL, NULL,
                         "ct952.io", 0x3000);
    memory_region_add_subregion(get_system_memory(),
                               0x80000000, io);

    /* UART1 @ 0x80000070 */
    serial_mm_init(get_system_memory(), 0x80000070, 0,
                  irq[2], 115200, serial_hd(0),
                  DEVICE_NATIVE_ENDIAN);

    /* Load firmware */
    if (machine->kernel_filename) {
        load_image_targphys(machine->kernel_filename,
                           0x40000000, 16 * 1024 * 1024);
    }
}

static void ct952_machine_init(MachineClass *mc)
{
    mc->desc = "CT952/CT909P Digital Picture Frame";
    mc->init = ct952_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("mips32r2-generic");
}

DEFINE_MACHINE("ct952", ct952_machine_init)
```

### Step 3: Add to QEMU Build

Edit `hw/mips/meson.build`, add:
```python
mips_ss.add(when: 'CONFIG_CT952', if_true: files('ct952.c'))
```

Edit `configs/devices/mipsel-softmmu/default.mak`, add:
```
CONFIG_CT952=y
```

### Step 4: Build QEMU
```bash
mkdir build && cd build
../configure --target-list=mipsel-softmmu
make -j$(nproc)
```

### Step 5: Run!
```bash
./qemu-system-mipsel \
    -M ct952 \
    -kernel /path/to/DVD909.rom \
    -serial stdio \
    -nographic
```

## What You'll See

If the firmware boots, you should see UART output at 115200 baud on stdio!

## Next Steps for Full Emulation

### Phase 1: Boot Messages ✅
- Minimal UART working
- See boot sequence
- Validate memory map

### Phase 2: Add Peripherals
```bash
# Timer for delays
hw/timer/ct952_timer.c

# Interrupt controller
hw/intc/ct952_intc.c

# Display output
hw/display/ct952_disp.c
```

### Phase 3: Boot Linux
Once we can boot the firmware, modify bootloader to load Linux from SD:
```bash
./qemu-system-mipsel \
    -M ct952 \
    -kernel vmlinux \
    -drive file=rootfs.ext4,if=sd \
    -append "root=/dev/mmcblk0 console=ttyS0,115200"
```

### Phase 4: Python!
Build minimal buildroot with Python:
```bash
# In buildroot
make menuconfig
# Select: Target packages → Interpreter → Python3
make

# Test in emulator
./qemu-system-mipsel \
    -M ct952 \
    -kernel output/images/vmlinux \
    -drive file=output/images/rootfs.ext4 \
    -append "root=/dev/mmcblk0 console=ttyS0" \
    -serial stdio

# Once booted:
# python3 -c "print('Hello from virtual DP700WD!')"
```

## Parallel Hardware Path

While building emulator, you can **also** work on hardware:

1. ✅ Solder IR receiver
2. ✅ Test backdoor (Vol Up/Down/Up/Down)
3. ✅ Enable UART debug (mode 2 or 11)
4. ✅ Connect serial adapter
5. ✅ Compare real hardware vs emulator output

**The emulator helps validate what you should see on real hardware!**

## Advantages Over Hardware-First Approach

| Aspect | Hardware Only | Emulator First |
|--------|--------------|----------------|
| Risk | Brick device | Zero risk |
| Speed | Flash → boot → test (minutes) | Rebuild → run (seconds) |
| Debug | Serial only | GDB + tracepoints |
| Iteration | Slow | Blazing fast |
| Python Testing | After hardware mods | Before anything |
| Cost | Need tools, parts | Free |

## Expected Timeline

- **Week 1**: Basic QEMU boot (UART output)
- **Week 2**: Add display, timers, interrupts
- **Week 3**: Linux boot in emulator
- **Week 4**: Python running in emulator
- **Week 5**: Flash to real hardware (validated!)

## Resources

**QEMU Development:**
- QEMU Docs: https://qemu.readthedocs.io/
- MIPS Machine Examples: `hw/mips/malta.c`, `hw/mips/jazz.c`
- Device Model Tutorial: https://qemu.readthedocs.io/en/latest/devel/qom.html

**Linux for MIPS:**
- OpenWrt: https://openwrt.org/ (great for embedded MIPS)
- Buildroot: https://buildroot.org/ (minimal builds)

**Community:**
- QEMU Mailing List: qemu-devel@nongnu.org
- OpenWrt Forum: https://forum.openwrt.org/
- EEVblog: https://www.eevblog.com/forum/

## Why This Will Work

1. ✅ **We have source code** - can instrument, understand behavior
2. ✅ **We have binaries** - can test actual firmware
3. ✅ **We have register map** - every peripheral documented
4. ✅ **We have linker scripts** - exact memory layout
5. ✅ **MIPS is well-supported** - QEMU has excellent MIPS support
6. ✅ **Similar projects exist** - OpenWrt on MIPS, Malta board emulation

## The Big Picture

```
                    ┌─────────────────┐
                    │  CT952 Emulator │
                    │   (QEMU MIPS)   │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
        Test Firmware   Boot Linux    Run Python
              │              │              │
              ▼              ▼              ▼
       Validate boot   Test drivers   Develop apps
              │              │              │
              └──────────────┴──────────────┘
                             │
                    Once everything works:
                             │
                    ┌────────▼────────┐
                    │  Flash to real  │
                    │  DP700WD device │
                    └─────────────────┘
                        🎉 Success!
```

---

**Start with emulation, perfect the code, then flash with confidence!**

This is the fastest, safest path to getting Python running on your picture frame. 🚀
