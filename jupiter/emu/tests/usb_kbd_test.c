/*
 * ct952emu USB transport verification: run the Jupiter OHCI boot-keyboard
 * driver (jusb_ohci) against the emulator's OHCI host-controller model and
 * its virtual keyboard, then feed the harvested reports through the HID
 * decoder (jusbhid). Closes the transport loop the same way jgpu/jspr
 * closed the GPU loop: real driver register programming -> real controller
 * semantics (HCCA/ED/TD DMA walk) -> the exact report bytes -> decoded key
 * events.
 *
 * The virtual keyboard delivers USB_KBD_SCRIPT (shared header) one entry
 * per interrupt-IN poll; this test asserts it received those exact bytes
 * and that the decoder saw every press. Freestanding SPARC: start.S calls
 * testmain(); return 0 = pass.
 */
#include "jup_types.h"
#include "jusbhid.h"
#include "jusb_ohci.h"
#include "testapi.h"
#include "usb_kbd_script.h"

#define WORKAREA 0x40380000u        /* 256-byte-aligned DRAM work area */

static jhid_kbd_t kbd;
static int ev_presses;              /* key + modifier DOWN events seen */
static uint8_t got[USB_KBD_SCRIPT_N][8];

static void sink(void *ctx, uint8_t usage, int pressed, uint8_t mods)
{
    (void)ctx; (void)usage; (void)mods;
    if (pressed) ev_presses++;
}

unsigned testmain(void)
{
    uint8_t report[8];
    long spins = 0;
    int gotn = 0, i, j, mism = 0, ep;

    jhid_kbd_reset(&kbd);

    /* Enumerate; the returned endpoint must be the one advertised in the
     * device's configuration descriptor (interrupt IN endpoint 1). */
    ep = jusb_ohci_init(WORKAREA);
    if (ep != USB_KBD_ENDPOINT)
        return 0xE0000000u | (unsigned)(ep & 0xFFFF);

    /* Poll until every scripted report has arrived (bounded spin). */
    while (gotn < USB_KBD_SCRIPT_N && spins < 5000000L) {
        if (jusb_ohci_poll(report)) {
            for (j = 0; j < 8; j++) got[gotn][j] = report[j];
            jhid_kbd_feed(&kbd, report, sink, 0);
            gotn++;
        }
        spins++;
    }

    if (gotn != USB_KBD_SCRIPT_N)
        return 0xD0000000u | (unsigned)gotn;   /* transport stalled */

    /* Transport delivered the exact wire bytes? */
    for (i = 0; i < USB_KBD_SCRIPT_N; i++)
        for (j = 0; j < 8; j++)
            if (got[i][j] != USB_KBD_SCRIPT[i][j]) mism++;

    /* Decoder saw all 5 DOWN events: Up, Down, A, Enter + the LShift that
     * precedes A (the script's one modifier). */
    if (ev_presses != 5) mism += 0x10000;

    return (unsigned)mism;    /* 0 = exact bytes + all presses decoded */
}
