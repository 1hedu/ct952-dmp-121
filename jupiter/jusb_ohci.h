/*
 * JupiterSDK on CT952 -- USB OHCI transport for the HID boot keyboard.
 *
 * The transport half that jusbhid's decoder sits on top of. OHCI (USB 1.1
 * Open Host Controller Interface) is standardised by its own spec; only
 * the register base is chip-specific. On this frame the host controller
 * is OHCI (interrupt.c: INT_PROC1_2ND_USB_OHCI, driven today by the Jungo
 * mass-storage stack), so the register block and the ED/TD/HCCA DMA model
 * below are spec-standard -- see the OHCI 1.0a spec for field details.
 *
 * This is the periodic interrupt-IN path only: enough to poll a boot
 * keyboard's 8-byte report every frame. Enumeration (control transfers:
 * SET_ADDRESS / SET_CONFIGURATION / SET_PROTOCOL=boot) is NOT here -- the
 * device is treated as already configured on its interrupt endpoint. That
 * control-list path is the remaining transport work.
 *
 * The driver is polled (no interrupt wiring needed): jusb_ohci_poll()
 * checks the WritebackDoneHead status and harvests a completed report.
 * It is verified on the emulated CT952 OHCI model (emu: make usbcheck),
 * the same closed-loop method used for jgpu/jspr.
 *
 * ENDIANNESS: EDs/TDs/HCCA are read by the controller and written by the
 * CPU as plain 32-bit words; on this big-endian SoC both sides use the
 * same byte order, so the descriptors are native-endian here. (Real
 * silicon's controller-side swap, if any, is a hardware-session detail.)
 */
#ifndef JUSB_OHCI_H
#define JUSB_OHCI_H

#include "jup_types.h"
#include "jusb_ohci_regs.h"   /* OHCI register/descriptor layout (shared) */

/* Bring up a USB boot keyboard from a 256-byte-aligned DRAM work area
 * (>= JUSB_OHCI_WORKAREA bytes): start the controller, enumerate the
 * device over the control list (GET_DESCRIPTOR / SET_ADDRESS /
 * SET_CONFIGURATION / SET_PROTOCOL=boot), then arm the periodic
 * interrupt-IN transfer on the endpoint discovered from the device's
 * configuration descriptor. Returns that endpoint number (>= 1) on
 * success, or -1 if enumeration failed. */
int jusb_ohci_init(uint32_t dram_workarea);

/* Poll for a completed report. Returns 1 and fills report[8] if a fresh
 * 8-byte report arrived since the last call (re-arming the next transfer),
 * else 0. Non-blocking. */
int jusb_ohci_poll(uint8_t report[8]);

#endif /* JUSB_OHCI_H */
