/*
 * ct952emu -- scripted boot-keyboard reports for the emulated OHCI
 * virtual keyboard. Shared verbatim by the emulator's virtual device
 * (machine.c) and the bare-metal transport test (tests/usb_kbd_test.c)
 * so the two agree by construction: the device delivers one entry per
 * successful interrupt-IN poll, and the test asserts it received exactly
 * these bytes.
 *
 * Each entry is an 8-byte HID boot report: [mod, 0, k0..k5].
 */
#ifndef CT952EMU_USB_KBD_SCRIPT_H
#define CT952EMU_USB_KBD_SCRIPT_H

static const unsigned char USB_KBD_SCRIPT[][8] = {
    { 0x00, 0, 0x52, 0, 0, 0, 0, 0 },   /* Up      down          */
    { 0x00, 0, 0x00, 0, 0, 0, 0, 0 },   /*         release       */
    { 0x00, 0, 0x51, 0, 0, 0, 0, 0 },   /* Down    down          */
    { 0x00, 0, 0x00, 0, 0, 0, 0, 0 },   /*         release       */
    { 0x02, 0, 0x04, 0, 0, 0, 0, 0 },   /* LShift + A            */
    { 0x00, 0, 0x00, 0, 0, 0, 0, 0 },   /*         release       */
    { 0x00, 0, 0x28, 0, 0, 0, 0, 0 },   /* Enter   down          */
    { 0x00, 0, 0x00, 0, 0, 0, 0, 0 },   /*         release       */
};
#define USB_KBD_SCRIPT_N ((int)(sizeof(USB_KBD_SCRIPT)/sizeof(USB_KBD_SCRIPT[0])))

/* USB_KBD_ENDPOINT (the interrupt IN endpoint) is defined in
 * jusb_ohci_regs.h, shared by the driver and the controller model. */

#endif /* CT952EMU_USB_KBD_SCRIPT_H */
