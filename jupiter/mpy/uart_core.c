#include <stdint.h>
#include "py/mpconfig.h"

/* CT952/CT909 UART1, as modelled by ct952emu:
 *   DATA @ 0x80000070, STAT @ 0x80000074.
 *   STAT: bit0 (0x1) = RX byte available; TX is always ready.
 * Byte access is big-endian; the emulator merges sub-word writes, so a
 * byte store to DATA delivers one character. */
#define UART1_DATA (*(volatile uint32_t *)0x80000070u)
#define UART1_STAT (*(volatile uint32_t *)0x80000074u)
#define UART_RX_READY 0x1u

/* USB HID keyboard driver (modusb_kbd.c): non-blocking single-key read, -1 if
 * no key / no keyboard. Lets the REPL take input from a USB keyboard as well as
 * UART1. */
extern int usb_kbd_c_getchar(void);

// Receive single character (blocking): poll the USB keyboard and UART1 RX,
// returning whichever produces a character first.
int mp_hal_stdin_rx_chr(void) {
    for (;;) {
        int c = usb_kbd_c_getchar();
        if (c >= 0) {
            return c;
        }
        if (UART1_STAT & UART_RX_READY) {
            return (int)(UART1_DATA & 0xFF);
        }
    }
}

// Send string of given length.
mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len) {
    for (mp_uint_t i = 0; i < len; i++) {
        UART1_DATA = (uint8_t)str[i];
    }
    return len;
}
