# Holtek USB keyboard — driver spec (boot keyboard only)

Target device: **VID 0x04D9 / PID 0x1203**, strings `HOLTEK` / `USB+PS2 Keyboard`.

This document covers **only USB interface 0**, the boot keyboard. That is the
only interface this driver needs to implement. Everything here was read live
from the hardware, not from a datasheet.

---

## 1. Device summary

| | |
|---|---|
| Speed | **Low speed (1.5 Mb/s)** — control + interrupt transfers only |
| bcdUSB | 2.00 |
| bMaxPacketSize0 | 8 |
| idVendor / idProduct | 0x04D9 / 0x1203 |
| bcdDevice | 0x0280 |
| Configurations | 1 (`bConfigurationValue` = 1) |
| Power | Bus-powered, 100 mA, remote-wakeup supported |
| Interfaces | 2 (see §5 — **interface 1 is not used**) |

The device descriptor sets `iManufacturer` / `iProduct` / `iSerialNumber` to
**0**, so there is no string to read the normal way. The strings do exist at
indices 1 and 2 (`HOLTEK`, `USB+PS2 Keyboard`) but nothing references them.
Do not rely on string descriptors to identify this device — match on VID/PID.

## 2. Interface 0 — the boot keyboard

| field | value |
|---|---|
| bInterfaceNumber | **0** |
| bInterfaceClass | 0x03 (HID) |
| bInterfaceSubClass | **0x01 (Boot)** |
| bInterfaceProtocol | **0x01 (Keyboard)** |
| bNumEndpoints | 1 |
| Endpoint | **0x81, IN, Interrupt, 8 bytes, bInterval 10 (≈100 Hz)** |
| HID version | 1.11 |
| Report descriptor | 62 bytes (see §6) |

**There is no OUT endpoint.** This matters — see §4.

## 3. Input report — 8 bytes on endpoint 0x81

Standard boot-keyboard layout. No report ID.

```
byte 0   modifier bitmap
           bit 0  Left Ctrl     bit 4  Right Ctrl
           bit 1  Left Shift    bit 5  Right Shift
           bit 2  Left Alt      bit 6  Right Alt
           bit 3  Left GUI      bit 7  Right GUI
byte 1   reserved, always 0x00
byte 2   key slot 1  \
byte 3   key slot 2   |
byte 4   key slot 3   |  HID Keyboard/Keypad usage codes
byte 5   key slot 4   |  0x00 = slot empty
byte 6   key slot 5   |
byte 7   key slot 6  /
```

Rules:

- Up to **6 simultaneous keys** plus the 8 modifiers.
- Slot order is not stable. Treat the six slots as an unordered set: a key is
  down if its usage appears in *any* slot. Compare each report against the
  previous one to derive press and release events.
- `0x00` means empty slot, not a key.
- If more keys are held than the matrix can encode, the firmware fills **all
  six slots with `0x01` (ErrorRollOver)**. Discard such a report entirely —
  do not emit six keypresses, and do not treat it as all keys released.
- The device sends a report on every state change. With `SET_IDLE(0)` it will
  not repeat unchanged reports; auto-repeat is the host's job.

Usage codes are standard HID Keyboard/Keypad page (0x07): `0x04`–`0x1D` = A–Z,
`0x1E`–`0x27` = 1–0, `0x28` Enter, `0x29` Escape, `0x2A` Backspace, `0x2B` Tab,
`0x2C` Space, `0x3A`–`0x45` = F1–F12, `0xE0`–`0xE7` = the modifiers.

Verified against this hardware by live capture: F1–F12 arrive as usages
`0x3A`–`0x45`, contiguous, with no gap between F10 and F11.

## 4. LEDs — must go over the control pipe

**This is the most common way to get this device wrong, and the reason LED
output silently does nothing.**

Interface 0 has exactly one endpoint, `0x81 IN`. There is **no interrupt OUT
endpoint**. Any driver that tries to write the LED report to an OUT pipe will
fail or no-op, while typing continues to work perfectly — because input uses a
completely separate path.

The LED report must be delivered as a **control transfer on endpoint 0**:

```
bmRequestType = 0x21   (host-to-device | class | recipient: interface)
bRequest      = 0x09   (SET_REPORT)
wValue        = 0x0200 (report type 2 = Output, report ID 0)
wIndex        = 0x0000 (interface number — 0, NOT an endpoint address)
wLength       = 1
data          = 1 byte:
                  bit 0  Num Lock
                  bit 1  Caps Lock
                  bit 2  Scroll Lock
                  bits 3-7  padding, send 0
```

Checklist when LEDs don't light:

1. `wIndex` is the **interface number (0)**, not `0x81` or `0x01`.
2. `wValue` high byte is **0x02** (Output). `0x03` is Feature and will be
   rejected or ignored.
3. `wLength` is **1**. The report has no report-ID prefix byte, because the
   descriptor declares no report ID — do not send a leading `0x00`.
4. The transfer is host-to-device; the data stage carries the byte out.
5. Low-speed control packets are limited to 8 bytes, which is ample for 1 byte,
   but some stacks mis-set the max packet size for low speed — `bMaxPacketSize0`
   is **8**, not 64.

## 5. Interface 1 — do not bind it

The device also exposes interface 1 (endpoint `0x82 IN`, interrupt, 8 bytes,
bInterval 10) declaring a Consumer Control collection with 24 media/browser bits
and a System Control collection with power/sleep/wake bits.

**This keyboard has none of those keys.** The Holtek controller ships one
firmware image across a whole product line; this unit's matrix does not populate
those functions. Confirmed by live capture: with Fn held and the entire F-row
pressed, interface 1 produced **zero reports**. The Fn key itself never reaches
the host at all — it is resolved inside the keyboard's own scanner and is not a
HID usage.

Binding interface 1 costs an interrupt pipe polled 100 times a second that will
never carry data. On a constrained device, skip it. It is mentioned here only so
that finding a second interface during enumeration does not look like a bug.

## 6. Report descriptor

The device reports `wDescriptorLength = 62` bytes. Because it is a **boot
keyboard (subclass 1, protocol 1)** you can issue `SET_PROTOCOL(boot)` and
hard-code the §3 layout without parsing anything.

The descriptor below was reconstructed from the parsed form the host OS built;
it is functionally equivalent to the device's own and is included for reference.
It is 56 bytes against the device's 62 — the difference is item encoding, not
missing fields (the same bit layout can be spelled several ways). Do not treat
it as byte-identical to what the device would return over the wire.

```
Usage Page (Generic Desktop)
Usage (Keyboard)
Collection (Application)
    Usage Page (Keyboard/Keypad)
    Usage Minimum (0xE0 Left Ctrl)
    Usage Maximum (0xE7 Right GUI)
    Logical Minimum (0)
    Logical Maximum (1)
    Report Size (1)
    Report Count (8)
    Input (Data, Variable, Absolute)          ; byte 0 - modifiers
    Input (Constant)                          ; byte 1 - reserved
    Usage Minimum (0x00)
    Usage Maximum (0x91)
    Logical Maximum (255)
    Report Size (8)
    Report Count (6)
    Input (Data, Array, Absolute)             ; bytes 2-7 - key slots
    Usage Page (LEDs)
    Usage Minimum (0x01 Num Lock)
    Usage Maximum (0x03 Scroll Lock)
    Logical Maximum (1)
    Report Size (1)
    Report Count (3)
    Output (Data, Variable, Absolute)         ; LED bits
    Report Count (5)
    Output (Constant)                         ; LED byte padding
End Collection
```

```c
static const unsigned char report_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x81, 0x01,
    0x19, 0x00, 0x29, 0x91, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81,
    0x00, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x03, 0x91, 0x02, 0x95, 0x05, 0x91, 0x01, 0xC0,
};
```

## 7. Bring-up sequence

```
1. SET_CONFIGURATION(1)
2. SET_PROTOCOL(boot)  on interface 0
     bmRequestType 0x21, bRequest 0x0B, wValue 0x0000, wIndex 0x0000
   (optional — the report-protocol layout is identical, see §6)
3. SET_IDLE(0)         on interface 0     <-- send this
     bmRequestType 0x21, bRequest 0x0A, wValue 0x0000, wIndex 0x0000
   Duration 0 = report only on change. Without it some firmware re-sends the
   current state every idle period and you get phantom repeats.
4. Poll endpoint 0x81 for 8-byte interrupt IN transfers, ~10 ms interval.
5. LEDs: control SET_REPORT per §4, whenever lock state changes.
```

## 8. Provenance

Read live from the running hardware with `hid_investigate.py`:

- USB device / configuration / interface / HID / endpoint descriptors, via the
  hub driver's `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION`.
- Field bit offsets decoded from the host's parsed HID data.
- Key behaviour confirmed by live input capture (F-row, Fn combinations,
  interface 1 silence).

Full unabridged dump, including the interface 1 collections, host driver stack
and registry state, is in `hid_dump_VID_04D9_PID_1203.md` / `.json`.

Not covered here: the raw report descriptor exactly as the device returns it.
Capturing that requires a USB bus trace (USBPcap/Wireshark) or a device-side
`GET_DESCRIPTOR(Report)` on interface 0 — worth doing only if §6's
reconstruction is ever in doubt. §3 is what the driver actually depends on, and
it is confirmed correct by observed traffic.
