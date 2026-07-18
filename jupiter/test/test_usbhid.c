/*
 * JupiterSDK on CT952 -- USB HID boot keyboard decoder (jusbhid) test.
 *
 * Feeds scripted 8-byte boot reports through jhid_kbd_feed and records
 * every emitted event into a flat log, then CRCs it. The log is also
 * checked against a hand-written expected event stream, so this is both
 * a correctness test (exact events) and an endianness proof (the CRC of
 * the byte log must match on little- and big-endian, per run_tests.sh).
 *
 * Covers: single key down/up, chord with modifier (Shift+A ordering),
 * N-key rollover across a multi-key transition, ErrorRollOver phantom
 * suppression, and ASCII mapping (shifted + unshifted).
 */
#include <stdio.h>
#include <string.h>
#include "jusbhid.h"

/* Flat event log: 3 bytes per event (usage, pressed, mods). */
static uint8_t log_buf[512];
static int     log_n;

static void sink(void *ctx, uint8_t usage, int pressed, uint8_t mods)
{
    (void)ctx;
    if (log_n + 3 > (int)sizeof(log_buf)) return;
    log_buf[log_n++] = usage;
    log_buf[log_n++] = (uint8_t)pressed;
    log_buf[log_n++] = mods;
}

static uint32_t crc32_buf(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* Build an 8-byte report from a modifier byte and up to 6 usages. */
static void rpt(uint8_t out[8], uint8_t mods,
                uint8_t k0, uint8_t k1, uint8_t k2,
                uint8_t k3, uint8_t k4, uint8_t k5)
{
    out[0] = mods; out[1] = 0;
    out[2] = k0; out[3] = k1; out[4] = k2;
    out[5] = k3; out[6] = k4; out[7] = k5;
}

int main(void)
{
    jhid_kbd_t kbd;
    uint8_t r[8];
    int fail = 0;

    /* Expected event stream, in emission order. Each triple is
     * {usage, pressed, mods}. Derived by hand from the scripted reports
     * and jusbhid's documented ordering (mod-downs, releases, presses,
     * mod-ups). */
    static const uint8_t expect[] = {
        /* 1. press A            */ JHID_USAGE_A, 1, 0,
        /* 2. release A          */ JHID_USAGE_A, 0, 0,
        /* 3. Shift down, then B */ JHID_USAGE_MOD_BASE + 1, 1, JHID_MOD_LSHIFT,
                                    JHID_USAGE_A + 1, 1, JHID_MOD_LSHIFT,  /* B */
        /* 4. release B, Shift up*/ JHID_USAGE_A + 1, 0, 0,
                                    JHID_USAGE_MOD_BASE + 1, 0, 0,
        /* 5. C down             */ JHID_USAGE_A + 2, 1, 0,               /* C */
        /* 6. C+D+E (add D,E)    */ JHID_USAGE_A + 3, 1, 0,               /* D */
                                    JHID_USAGE_A + 4, 1, 0,               /* E */
        /* 7. rollover: no change*/
        /* 8. release all        */ JHID_USAGE_A + 2, 0, 0,              /* C */
                                    JHID_USAGE_A + 3, 0, 0,              /* D */
                                    JHID_USAGE_A + 4, 0, 0,              /* E */
    };

    jhid_kbd_reset(&kbd);

    /* 1. A down */               rpt(r, 0, JHID_USAGE_A,0,0,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 2. all up */              rpt(r, 0, 0,0,0,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 3. Shift+B */             rpt(r, JHID_MOD_LSHIFT, 0x05,0,0,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 4. all up */              rpt(r, 0, 0,0,0,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 5. C down */              rpt(r, 0, 0x06,0,0,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 6. C,D,E held */          rpt(r, 0, 0x06,0x07,0x08,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 7. ErrorRollOver phantom (all slots 0x01): must emit nothing and
     *    keep C,D,E held */     rpt(r, 0, 0x01,0x01,0x01,0x01,0x01,0x01);
    jhid_kbd_feed(&kbd, r, sink, 0);
    /* 8. all up (releases C,D,E in slot order) */
                                 rpt(r, 0, 0,0,0,0,0,0);
    jhid_kbd_feed(&kbd, r, sink, 0);

    if (log_n != (int)sizeof(expect) ||
        memcmp(log_buf, expect, sizeof(expect)) != 0) {
        int i;
        printf("FAIL: event stream mismatch (got %d bytes, want %d)\n",
               log_n, (int)sizeof(expect));
        for (i = 0; i < log_n && i < (int)sizeof(expect); i += 3)
            printf("  [%2d] got %02x %d %02x  want %02x %d %02x\n", i / 3,
                   log_buf[i], log_buf[i+1], log_buf[i+2],
                   expect[i], expect[i+1], expect[i+2]);
        fail = 1;
    }

    /* ASCII mapping spot-checks */
    if (jhid_usage_to_ascii(JHID_USAGE_A, 0) != 'a' ||
        jhid_usage_to_ascii(JHID_USAGE_A, JHID_MOD_LSHIFT) != 'A' ||
        jhid_usage_to_ascii(JHID_USAGE_1, 0) != '1' ||
        jhid_usage_to_ascii(JHID_USAGE_1, JHID_MOD_RSHIFT) != '!' ||
        jhid_usage_to_ascii(JHID_USAGE_0, 0) != '0' ||
        jhid_usage_to_ascii(JHID_USAGE_SPACE, 0) != ' ' ||
        jhid_usage_to_ascii(JHID_USAGE_ENTER, 0) != '\n' ||
        jhid_usage_to_ascii(JHID_USAGE_UP, 0) != 0) {
        printf("FAIL: ASCII mapping wrong\n");
        fail = 1;
    }

    if (fail) return 1;

    printf("CRC usbhid=%08x\n", (unsigned)crc32_buf(log_buf, (uint32_t)log_n));
    printf("usbhid tests OK\n");
    return 0;
}
