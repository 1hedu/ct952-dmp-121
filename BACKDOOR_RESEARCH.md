# Coby DP700WD Digital Picture Frame - Backdoor Research

## Hardware
- **Model**: Coby DP700WD Digital Picture Frame
- **Chipset**: Actions Semiconductor CT952 (CT909P IC System)
- **Architecture**: Likely MIPS-based
- **Firmware**: DMP-121 (Digital Media Player)

## Backdoor Access

### IR Remote Sequence
**Activation**: Press on IR remote: `Vol Up → Vol Down → Vol Up → Vol Down`

This sequence is defined in `storage.h:532-533`:
```c
BYTE __bBackDoorSequence[BACK_DOOR_SEQUENCE_NUM] =
    {KEY_VOL_UP, KEY_VOL_DOWN, KEY_VOL_UP, KEY_VOL_DOWN};
```

### Available Backdoor Functions

Once activated (displays "6000 + debug_index" in top right), you can navigate with:
- **UP/DOWN**: Cycle through backdoor menu items
- **LEFT/RIGHT**: Adjust values
- **MENU/EXIT**: Exit backdoor
- **MUTE/ZOOM**: Display firmware version info

#### Menu Items:

1. **SETUP_BACK_DOOR_REGION** (non-CT950 builds)
   - Change DVD region code (Regions 1-8, or ALL=255)
   - Useful for region-free playback

2. **SETUP_BACK_DOOR_DEBUG_MODE** (all builds)
   - Configure serial port routing for debugging
   - 12 debug modes available (see below)

### Debug Modes (Serial Port Configuration)

The debug modes route serial communication to different hardware paths:

| Mode | DSU1_TX | DSU1_RX | UART1_TX | UART1_RX | UART2_TX | UART2_RX | Notes |
|------|---------|---------|----------|----------|----------|----------|-------|
| 0    | OFF     | OFF     | OFF      | OFF      | OFF      | OFF      | All disabled |
| 1    | NIM     | NIM     | OFF      | OFF      | OFF      | OFF      | DSU to tuner/NIM |
| 2    | CARD    | CARD    | OFF      | OFF      | OFF      | OFF      | DSU to card reader |
| 3    | OFF     | OFF     | NIM      | NIM      | OFF      | OFF      | UART1 to NIM |
| 4    | NIM     | NIM     | NIM      | NIM      | OFF      | OFF      | Both to NIM |
| 5    | CARD    | CARD    | NIM      | NIM      | OFF      | OFF      | Mixed routing |
| 6    | OFF     | OFF     | CARD     | CARD     | OFF      | OFF      | UART1 to card |
| 7    | NIM     | NIM     | CARD     | CARD     | OFF      | OFF      | Mixed routing |
| 8    | CARD    | CARD    | CARD     | CARD     | OFF      | OFF      | Both to card |
| 9    | CARD    | CARD    | CARD_TX  | OFF      | OFF      | GPIO_RX  | Card + GPIO |
| 10   | NIM     | NIM     | CARD_TX  | OFF      | OFF      | GPIO_RX  | NIM + GPIO |
| 11   | OFF     | OFF     | CARD_TX  | OFF      | OFF      | GPIO_RX  | Card TX + GPIO RX |

**Key Paths:**
- **NIM_PATH**: Tuner/Network Interface Module connection
- **CARD_READER_PATH**: SD/MMC card reader connection
- **EXPAND_GPIO_PATH**: GPIO expansion port

**Best for Serial Debug**: Mode 2, 6, 8, or 11 - routes UART to card reader pins (easier to access)

### Version Information Display

Press **MUTE** or **ZOOM** while in backdoor mode to display:
- Customer S/W version
- F/W version (Firmware)
- FAE version (Field Application Engineer)
- MPEG decoder version
- JPEG decoder version
- Display controller version
- DivX codec version
- Navigator version
- USB stack version
- Parser version
- Card reader version
- Info module version
- TFT controller version
- Audio processor version
- Servo version (DVD models only)

Example from `backdoor.c:491-495`:
```c
#ifdef IMAGE_FRAME_SETUP
    __dwVersionRelease = (DMP_SW_VERSION<<16) | (DMP_SW_MINOR_VERSION<<8);
    __dwVersionFAE = (DMP_SW_VERSION<<16) | (DMP_SW_MINOR_VERSION<<8) | FAE_DMP_SW_MINOR_VERSION;
#endif
```

## Hardware Modification for IR Backdoor

You mentioned needing to solder an IR receiver to the PCB. Likely connections:
- IR receiver module (e.g., TSOP38238)
- VCC: +3.3V or +5V
- GND: Ground
- OUT: GPIO pin configured for IR input

Look for:
- Unpopulated IR receiver footprint on PCB
- GPIO pins labeled IR_IN or similar
- Test points near existing remote sensor

## USB Capabilities

The firmware includes:
- **USB host support** (`usb.a`, `usb.h`)
- **Mass storage class** (for reading USB drives)
- **Card reader support** (SD/MMC)

However, **NO evidence** of:
- USB gadget/device mode
- USB display functionality (libam7xxx)
- USB serial console

The DP700WD appears to be USB host only (not a USB display device like the DP722).

## Getting Python Running - Challenges

### Option 1: Serial Console + Linux Boot
**Requirements:**
1. Access UART via backdoor mode
2. Interrupt boot sequence
3. Check if bootloader supports loading from SD/USB
4. Boot minimal Linux (e.g., buildroot, OpenWrt)
5. Install Python

**Challenges:**
- Likely only 16-64MB RAM
- Limited flash storage
- No MMU (MIPS architecture variant)
- May need custom Linux kernel

### Option 2: Firmware Modification
**Requirements:**
1. Dump existing firmware
2. Reverse engineer firmware format
3. Add Python interpreter to firmware image
4. Reflash device

**Challenges:**
- Bare metal firmware (no OS layer)
- Would need to port Python to bare CT952
- Extremely limited resources

### Option 3: Bootloader Replacement
**Requirements:**
1. Identify bootloader (likely U-Boot or proprietary)
2. Replace with U-Boot configured for network/USB boot
3. Boot Linux from external storage
4. Run Python in Linux environment

**Recommended Path:**
1. ✅ Solder IR receiver
2. ✅ Activate backdoor (Vol Up/Down/Up/Down)
3. ✅ Set debug mode to route UART to accessible pins (mode 2, 6, or 11)
4. ✅ Connect USB-to-serial adapter to card reader or GPIO pins
5. ✅ Monitor boot sequence for bootloader messages
6. ✅ Attempt to interrupt boot and access bootloader console
7. ⚠️ Dump firmware for backup
8. ⚠️ Research CT952 Linux support (OpenWrt, etc.)
9. ⚠️ Build minimal Linux with Python for MIPS
10. ⚠️ Boot from SD card or USB

## Additional Resources Needed

To proceed, we need:
- **PCB photos** - to identify UART pins
- **Serial dump of boot** - to see bootloader
- **Firmware dump** - for backup and analysis
- **Memory map** - RAM/Flash layout
- **CT952 datasheet** - if available

## Known Similar Projects

Look for:
- **OpenWrt** support for Actions Semiconductor chips
- **Picture frame hacking** communities (OpenFrame project)
- **MIPS Linux** distributions for embedded devices

## Contact Points for Further Research

- Actions Semiconductor developer forums
- OpenWrt forum (embedded Linux)
- EEVblog forum (hardware hacking)
- /r/ReverseEngineering

---

*Research compiled from firmware source analysis of ct952-dmp-121 codebase*
