/*
 * ct952emu -- run the USB OHCI boot-keyboard transport test on the
 * emulated CT952 and report the result. testmain() drives jusb_ohci
 * against the emulator's OHCI model + virtual keyboard and returns 0 when
 * the harvested reports match the scripted wire bytes and the HID decoder
 * saw every press. start.S stores the return at 0x80007FF0 and the done
 * flag 0xC0DED00D at 0x80007FF4.
 *
 *   usb_check <usb_kbd_test.bin>
 */
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *bin = (argc > 1) ? argv[1] : "tests/usb_kbd_test.bin";
    FILE *f = fopen(bin, "rb");
    uint8_t *img;
    long sz;
    machine_t *m;
    uint32_t flag, result;

    if (!f) { perror(bin); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    m = (machine_t *)malloc(sizeof(*m));
    if (!m || machine_init(m, img, (uint32_t)sz) != 0) return 2;
    free(img);
    m->uart_echo = 0;
    machine_run(m, 50000000ull);

    flag   = m->io[0x7FF4 / 4];
    result = m->io[0x7FF0 / 4];

    if (flag != 0xC0DED00Du) {
        printf("FAIL: image did not complete (flag=%08x)\n", flag);
        return 1;
    }
    if (result != 0) {
        if ((result & 0xF0000000u) == 0xD0000000u)
            printf("FAIL: transport stalled -- only %u report(s) arrived\n",
                   result & 0x0FFFFFFFu);
        else
            printf("FAIL: transport result=%08x (report-byte / decode mismatch)\n",
                   result);
        return 1;
    }
    printf("jusb_ohci on emulated CT952 OHCI: boot-keyboard reports "
           "byte-exact end to end, all presses decoded. OK\n");
    machine_free(m); free(m);
    return 0;
}
