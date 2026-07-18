/*
 * ct952emu UART RX test: prove the modelled UART1 receive path works,
 * using the real hardware register interface. Polls the UART1 status
 * register for DATA_READY, reads each byte from the data register, and
 * echoes it back out the same UART TX -- the exact loop the firmware's
 * serial-debug monitor uses (HAL_UART_ReceiveChar / SendChar).
 *
 * Feed bytes with `--uart-in FILE` and capture TX with `--uart FILE`;
 * the captured output must equal the input (echo), terminated by '\n'.
 * Freestanding: no .data.
 */
#include "testapi.h"

#define UART1_DATA  0x80000070u
#define UART1_STAT  0x80000074u
#define STAT_RX_RDY 0x01u          /* DATA_READY */
#define STAT_TX_RDY 0x04u          /* TH_EMPTY   */

static unsigned rd(unsigned a) { return *(volatile unsigned *)a; }
static void wr(unsigned a, unsigned v) { *(volatile unsigned *)a = v; }

unsigned testmain(void)
{
    unsigned n = 0;
    for (;;) {
        unsigned c;
        while (!(rd(UART1_STAT) & STAT_RX_RDY))
            ;                       /* spin for an RX byte */
        c = rd(UART1_DATA) & 0xFF;
        while (!(rd(UART1_STAT) & STAT_TX_RDY))
            ;
        wr(UART1_DATA, c);          /* echo it back */
        n++;
        if (c == '\n')
            break;                  /* end of line: done */
    }
    return n;                       /* bytes echoed */
}
