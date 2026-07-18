/*
 * JupiterSDK on CT952 -- USB HID boot-protocol keyboard decoder (jusbhid)
 *
 * Groundwork for eventual USB keyboard support. This is the transport-
 * independent half: it turns the 8-byte HID *boot protocol* keyboard
 * report into press/release events, and maps HID usage codes to ASCII.
 * It touches no registers and has no hardware dependency, so it is fully
 * host-testable (see test/test_usbhid.c) and endian-clean.
 *
 * Layering (top calls down):
 *
 *   SDK / app            button state, text entry
 *      ^
 *   jinp_map_hid()       HID usage -> JBTN_* mask         (jshim_ct952.c)
 *      ^
 *   jhid_kbd_feed()      report -> down/up events         (this file)
 *      ^
 *   USB transport        OHCI interrupt-IN endpoint       (TODO, chip side)
 *
 * The transport is the deferred, chip-specific piece: on this frame the
 * host controller is OHCI (interrupt.c: INT_PROC1_2ND_USB_OHCI, Jungo
 * stack), used today only for mass storage. A boot keyboard is a low/
 * full-speed device delivering an 8-byte report on a periodic interrupt
 * IN endpoint; enumeration (SET_PROTOCOL=boot, SET_CONFIGURATION) rides
 * endpoint 0. Once a report lands in DRAM each frame, it flows straight
 * into jhid_kbd_feed() below -- so this decoder is ready before the OHCI
 * modelling exists in the emulator.
 *
 * Report layout (HID boot keyboard, 8 bytes):
 *   [0] modifier bitmask (see JHID_MOD_*)
 *   [1] reserved (OEM)
 *   [2..7] up to 6 simultaneously-pressed key usage IDs (HID Usage
 *          Table page 0x07). 0 = empty slot. 0x01 in the slots =
 *          ErrorRollOver (too many keys): the whole key set is invalid
 *          for that report and is ignored here (modifiers still apply).
 */
#ifndef JUSBHID_H
#define JUSBHID_H

#include "jup_types.h"

/* Modifier bits (report byte 0 / HID usages 0xE0..0xE7) */
#define JHID_MOD_LCTRL   0x01u
#define JHID_MOD_LSHIFT  0x02u
#define JHID_MOD_LALT    0x04u
#define JHID_MOD_LGUI    0x08u
#define JHID_MOD_RCTRL   0x10u
#define JHID_MOD_RSHIFT  0x20u
#define JHID_MOD_RALT    0x40u
#define JHID_MOD_RGUI    0x80u
#define JHID_MOD_SHIFT   (JHID_MOD_LSHIFT | JHID_MOD_RSHIFT)
#define JHID_MOD_CTRL    (JHID_MOD_LCTRL  | JHID_MOD_RCTRL)
#define JHID_MOD_ALT     (JHID_MOD_LALT   | JHID_MOD_RALT)

/* Commonly-used HID keyboard usage IDs (page 0x07) */
#define JHID_USAGE_NONE      0x00u
#define JHID_USAGE_ROLLOVER  0x01u
#define JHID_USAGE_A         0x04u   /* letters run 0x04..0x1D = A..Z */
#define JHID_USAGE_Z         0x1Du
#define JHID_USAGE_1         0x1Eu   /* digits 0x1E..0x26 = 1..9, 0x27 = 0 */
#define JHID_USAGE_0         0x27u
#define JHID_USAGE_ENTER     0x28u
#define JHID_USAGE_ESC       0x29u
#define JHID_USAGE_BACKSPACE 0x2Au
#define JHID_USAGE_TAB       0x2Bu
#define JHID_USAGE_SPACE     0x2Cu
#define JHID_USAGE_MINUS     0x2Du
#define JHID_USAGE_EQUAL     0x2Eu
#define JHID_USAGE_RIGHT     0x4Fu
#define JHID_USAGE_LEFT      0x50u
#define JHID_USAGE_DOWN      0x51u
#define JHID_USAGE_UP        0x52u
/* Modifier keys also have usages 0xE0..0xE7; jhid_kbd_feed reports
 * modifier changes with those usage values so callers can treat them
 * uniformly with ordinary keys. */
#define JHID_USAGE_MOD_BASE  0xE0u

/* Decoder state: the previously-seen (normalised) report. Zero-init or
 * jhid_kbd_reset() before first use. */
typedef struct {
    uint8_t mods;       /* last modifier byte                     */
    uint8_t keys[6];    /* last real key set (error codes zeroed) */
} jhid_kbd_t;

/* Event callback: one call per key or modifier transition.
 *   usage      HID usage ID (0xE0..0xE7 for the 8 modifier keys)
 *   pressed    1 = went down, 0 = went up
 *   modifiers  modifier bitmask in effect for this report            */
typedef void (*jhid_event_fn)(void *ctx, uint8_t usage, int pressed,
                              uint8_t modifiers);

/* Clear decoder state (no keys held). */
void jhid_kbd_reset(jhid_kbd_t *kbd);

/* Feed one 8-byte boot report. Emits, in this deterministic order:
 *   1. modifier DOWNs (newly set, bit 0..7)
 *   2. key releases   (keys held before, absent now)
 *   3. key presses    (keys present now, not held before)
 *   4. modifier UPs   (newly cleared, bit 0..7)
 * so a chord like Shift+A yields Shift-down, A-down on press and A-up,
 * Shift-up on release. ErrorRollOver reports (0x01 in the key slots)
 * update modifiers only and leave the held-key set untouched -- no
 * phantom releases. Safe with sink == NULL (state still advances). */
void jhid_kbd_feed(jhid_kbd_t *kbd, const uint8_t report[8],
                   jhid_event_fn sink, void *ctx);

/* Translate a key usage to a US-layout ASCII character given the active
 * modifiers (shift only; ctrl/alt ignored). Returns 0 for keys with no
 * printable form. Enter -> '\n', Tab -> '\t', Backspace -> '\b'. */
char jhid_usage_to_ascii(uint8_t usage, uint8_t modifiers);

#endif /* JUSBHID_H */
