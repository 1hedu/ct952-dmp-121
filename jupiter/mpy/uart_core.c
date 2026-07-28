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

// Receive single character (blocking).
int mp_hal_stdin_rx_chr(void) {
    while ((UART1_STAT & UART_RX_READY) == 0) {
    }
    return (int)(UART1_DATA & 0xFF);
}

// Send string of given length.
mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len) {
    for (mp_uint_t i = 0; i < len; i++) {
        UART1_DATA = (uint8_t)str[i];
    }
    return len;
}
