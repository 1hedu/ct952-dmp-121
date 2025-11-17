# DP700WD Hardware Hacking Guide

## Quick Reference: Backdoor Access
**IR Sequence**: Vol Up → Vol Down → Vol Up → Vol Down
**Result**: Shows "60XX" in top right corner

## Phase 1: IR Remote Access (START HERE)

### Required Materials
- IR receiver module (TSOP38238, TSOP4838, or similar 38kHz)
- Soldering iron & solder
- Multimeter
- Universal IR remote (or NEC-protocol remote)

### Step 1: Locate IR Receiver Pads
Look on the PCB for:
- Unpopulated 3-pin footprint labeled "IR" or "U?"
- Usually near the front panel
- Standard pinout: GND - VOUT - VCC

### Step 2: Solder IR Receiver
```
TSOP38238 Pinout (facing front):
  1: OUT → to MCU GPIO
  2: GND → Ground
  3: VCC → +3.3V or +5V (check with multimeter!)
```

### Step 3: Test with Universal Remote
1. Power on the frame
2. Navigate to setup menu (if possible)
3. Try backdoor sequence with various remote codes:
   - NEC protocol (most common)
   - Try customer codes: 0x00, 0xFF

### Step 4: Access Backdoor
Press: **Vol Up, Vol Down, Vol Up, Vol Down**
- Should see "6000" or "6001" etc. in top right
- Use Up/Down to navigate menu
- Use Left/Right to change values

---

## Phase 2: Serial Console Access

### Required Materials
- USB-to-TTL serial adapter (3.3V logic level!)
  - FTDI FT232RL, CP2102, or CH340
  - **MUST BE 3.3V** - 5V will damage the chip!
- Jumper wires
- Soldering equipment (may need to solder to vias/pads)

### Step 1: Enable Serial Debug in Backdoor
1. Access backdoor menu
2. Navigate to "Debug Mode" (shows 60XX number)
3. Set to Mode 2, 6, 8, or 11 (routes UART to card reader)
4. Mode 11 recommended: CARD_TX + GPIO_RX

### Step 2: Locate UART Pins

#### Option A: SD Card Slot
The debug mode routes UART to card reader pins:
```
SD Card Pinout (looking at slot):
  1: CD/DAT3
  2: CMD        ← May be UART TX
  3: GND
  4: VCC (+3.3V)
  5: CLK
  6: GND
  7: DAT0       ← May be UART RX
  8: DAT1
  9: DAT2
```

#### Option B: GPIO Test Points
Look for:
- Test points labeled TX, RX, UART, or DEBUG
- Grouped set of 4-6 pads (VCC, GND, TX, RX, etc.)
- Near the main IC

### Step 3: Connect Serial Adapter

**CRITICAL: Verify 3.3V with multimeter first!**

```
USB-Serial → DP700WD
   GND    →   GND (SD pin 3 or 6)
   RX     →   TX (SD pin 2 or test point)
   TX     →   RX (SD pin 7 or test point)
   VCC    →   DO NOT CONNECT (frame powers itself)
```

### Step 4: Monitor Serial Output

**Settings**: 115200 baud, 8N1 (or try 57600, 38400)

```bash
# Linux/Mac
screen /dev/ttyUSB0 115200
# or
minicom -D /dev/ttyUSB0 -b 115200

# Windows
PuTTY: COM port, 115200 baud, 8N1
```

Power cycle the frame and watch for boot messages!

---

## Phase 3: Bootloader Investigation

### What to Look For in Serial Output

1. **Bootloader Banner**
```
U-Boot 1.x.x
Actions SOC Boot
ROM Boot v1.x
```

2. **Boot Delay Message**
```
Hit any key to stop autoboot: 3
Press SPACE to enter setup
```

3. **Memory Information**
```
DRAM: 16 MB / 32 MB / 64 MB
Flash: 8 MB / 16 MB
```

### Commands to Try (if you get bootloader prompt)

```bash
# U-Boot commands
help                  # List available commands
printenv              # Show environment variables
bdinfo                # Board info
md 0x80000000 0x100   # Memory dump
mw 0x80000000 0x12345678  # Memory write (test RAM)

# Custom bootloader
?                     # Help
version               # Version info
boot                  # Continue boot
```

---

## Phase 4: Firmware Dumping

### Method 1: Via Bootloader (if accessible)
```bash
# U-Boot example
md.b 0x80000000 0x1000000  # Dump 16MB from RAM
# Copy output or use loadb/loady commands
```

### Method 2: Via SPI Flash Programmer
**Required**: SPI flash programmer (CH341A, Bus Pirate, etc.)

1. Locate SPI flash chip on PCB
   - Usually 8-pin SOIC package
   - Near main IC
   - Labels: Winbond W25Q64, Macronix MX25L, etc.

2. Read chip datasheet for pinout:
```
Standard SPI Flash (top view):
  1: CS#   (Chip Select)
  2: DO    (Data Out / MISO)
  3: WP#   (Write Protect)
  4: GND
  5: DI    (Data In / MOSI)
  6: CLK   (Clock)
  7: HOLD#
  8: VCC (+3.3V)
```

3. Use flashrom or proprietary tool:
```bash
flashrom -p ch341a_spi -r firmware_backup.bin
```

**⚠️ ALWAYS make multiple backup copies before modifying!**

---

## Phase 5: Running Custom Code

### Option A: SD Card Boot (if bootloader supports it)
1. Get bootloader prompt
2. Check for `fatload`, `ext2load`, or `mmc` commands
3. Prepare SD card with boot files:
```
/uImage       - Linux kernel
/rootfs.img   - Root filesystem
/boot.scr     - U-Boot script (optional)
```

4. Boot from SD:
```bash
fatload mmc 0:1 0x80008000 uImage
bootm 0x80008000
```

### Option B: USB Boot
Similar to SD boot, but using USB drive:
```bash
usb start
fatload usb 0:1 0x80008000 uImage
bootm 0x80008000
```

### Option C: Network Boot (TFTP)
Requires ethernet or USB-ethernet adapter:
```bash
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.50
tftpboot 0x80008000 uImage
bootm 0x80008000
```

### Option D: Flash Replacement
1. Build custom firmware image
2. Use SPI programmer to flash new image
3. **KEEP BACKUPS!** Brick risk is HIGH

---

## Phase 6: Linux + Python

### Building Linux for CT952 (MIPS)

#### Requirements
- Cross-compiler: `mipsel-linux-gnu-gcc` or buildroot
- Kernel: Linux 2.6.x or 3.x with Actions SOC support
- Root filesystem: Busybox + Python

#### Buildroot Configuration
```bash
git clone git://git.buildroot.org/buildroot
cd buildroot
make menuconfig

# Configure:
Target options → MIPS (little endian)
Target Architecture Variant → mips32
Toolchain → External toolchain
Target packages → Interpreter languages → Python3 (select)
Target packages → Show packages that are also provided by busybox

make
```

Result: `output/images/rootfs.tar`

### Minimal Python Setup

For extremely limited flash (16-32MB total):
```bash
# Option 1: MicroPython (smaller footprint)
git clone https://github.com/micropython/micropython
cd micropython/ports/unix
make CROSS_COMPILE=mipsel-linux-gnu-

# Option 2: Python 2.7 minimal
./configure --host=mipsel-linux-gnu --disable-ipv6 --disable-shared
make
make install DESTDIR=/path/to/rootfs

# Strip to save space
mipsel-linux-gnu-strip python
```

### Testing on Device
```bash
# Via serial console or SSH (if network available)
python -c "print('Hello from DP700WD!')"

# Display test (framebuffer)
python -c "from PIL import Image; Image.new('RGB', (320, 240), 'red').save('/dev/fb0')"
```

---

## Pinout Reference (Once Discovered)

**Document your findings here:**

### UART Pins
- TX: _____________
- RX: _____________
- GND: _____________
- VCC: _____________ (voltage: _____ V)

### JTAG Pins (if found)
- TDO: _____________
- TDI: _____________
- TMS: _____________
- TCK: _____________
- TRST: _____________
- GND: _____________

### SPI Flash
- Chip Model: _____________
- Size: _____________
- CS: _____________
- MISO: _____________
- MOSI: _____________
- CLK: _____________

### Other Test Points
- _____________: _____________
- _____________: _____________

---

## Troubleshooting

### No Serial Output
- ✓ Verify 3.3V logic level (NOT 5V!)
- ✓ Try swapping TX/RX
- ✓ Try different baud rates: 115200, 57600, 38400, 9600
- ✓ Check ground connection
- ✓ Verify debug mode is set in backdoor

### Backdoor Not Working
- ✓ IR receiver polarity correct?
- ✓ IR receiver getting power? (measure with multimeter)
- ✓ Try different IR remote codes
- ✓ Try different remote brands/protocols
- ✓ Check solder joints

### Bricked Device
- ✓ Don't panic!
- ✓ Use SPI programmer to reflash backup
- ✓ Check for JTAG pins as last resort
- ✓ Contact Actions Semiconductor support (unlikely to help)

---

## Safety & Warnings

⚠️ **ALWAYS make firmware backups before modifications**
⚠️ **Use 3.3V logic levels - 5V WILL damage the chip**
⚠️ **Disconnect power before soldering**
⚠️ **ESD precautions - use grounding strap**
⚠️ **Some modifications may void warranty (if still valid)**
⚠️ **Brick risk is real - proceed at your own risk**

---

## Community Resources

- OpenWrt Forum: https://forum.openwrt.org/
- EEVblog Forum: https://www.eevblog.com/forum/
- /r/ReverseEngineering: https://reddit.com/r/ReverseEngineering
- Hackaday.io: Search for "digital picture frame"

---

## Next Steps

- [ ] Solder IR receiver
- [ ] Test backdoor access
- [ ] Enable serial debug mode
- [ ] Locate UART pins
- [ ] Capture boot log
- [ ] Dump firmware (backup!)
- [ ] Research CT952 Linux support
- [ ] Build minimal Linux + Python
- [ ] Profit! 🎉

**Document everything you discover!** Take photos, notes, and share your findings with the community.

---

*Good luck with your Python picture frame project!*
