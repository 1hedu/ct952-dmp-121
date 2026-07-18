/*
 * JupiterSDK on CT952 -- USB HID boot keyboard decoder implementation.
 *
 * Pure logic: diff the incoming report against the last one, emit the
 * transitions. No hardware, no allocation, endian-agnostic (byte access
 * only). See jusbhid.h for the layering and report layout.
 */
#include "jusbhid.h"

void jhid_kbd_reset(jhid_kbd_t *kbd)
{
    int i;
    if (!kbd) return;
    kbd->mods = 0;
    for (i = 0; i < 6; i++) kbd->keys[i] = 0;
}

/* Is usage `u` present in the 6-slot key array `set`? (0 = empty slot) */
static int in_set(const uint8_t *set, uint8_t u)
{
    int i;
    if (!u) return 1;                 /* empty slot: never a transition */
    for (i = 0; i < 6; i++)
        if (set[i] == u) return 1;
    return 0;
}

/* A real, pressable key usage (not empty / not an error code). */
static int is_key(uint8_t u) { return u >= 0x04u; }

void jhid_kbd_feed(jhid_kbd_t *kbd, const uint8_t report[8],
                   jhid_event_fn sink, void *ctx)
{
    uint8_t nmods, nkeys[6];
    int i, bit, rollover = 0;

    if (!kbd || !report) return;

    nmods = report[0];
    for (i = 0; i < 6; i++) {
        uint8_t u = report[2 + i];
        if (u == JHID_USAGE_ROLLOVER) rollover = 1;   /* phantom report */
        nkeys[i] = u;
    }

    /* 1. modifier DOWNs (newly set), bit order */
    for (bit = 0; bit < 8; bit++) {
        uint8_t m = (uint8_t)(1u << bit);
        if ((nmods & m) && !(kbd->mods & m) && sink)
            sink(ctx, (uint8_t)(JHID_USAGE_MOD_BASE + bit), 1, nmods);
    }

    if (!rollover) {
        /* 2. key releases: held before, absent now */
        for (i = 0; i < 6; i++) {
            uint8_t u = kbd->keys[i];
            if (is_key(u) && !in_set(nkeys, u) && sink)
                sink(ctx, u, 0, nmods);
        }
        /* 3. key presses: present now, not held before */
        for (i = 0; i < 6; i++) {
            uint8_t u = nkeys[i];
            if (is_key(u) && !in_set(kbd->keys, u) && sink)
                sink(ctx, u, 1, nmods);
        }
        /* commit key set, normalising error codes to empty */
        for (i = 0; i < 6; i++)
            kbd->keys[i] = is_key(nkeys[i]) ? nkeys[i] : 0;
    }
    /* rollover: leave kbd->keys untouched (no phantom releases) */

    /* 4. modifier UPs (newly cleared), bit order */
    for (bit = 0; bit < 8; bit++) {
        uint8_t m = (uint8_t)(1u << bit);
        if (!(nmods & m) && (kbd->mods & m) && sink)
            sink(ctx, (uint8_t)(JHID_USAGE_MOD_BASE + bit), 0, nmods);
    }

    kbd->mods = nmods;
}

char jhid_usage_to_ascii(uint8_t usage, uint8_t modifiers)
{
    int shift = (modifiers & JHID_MOD_SHIFT) != 0;

    if (usage >= JHID_USAGE_A && usage <= JHID_USAGE_Z) {
        char c = (char)('a' + (usage - JHID_USAGE_A));
        return shift ? (char)(c - 'a' + 'A') : c;
    }
    if (usage >= JHID_USAGE_1 && usage <= JHID_USAGE_0) {
        /* 0x1E..0x27 = 1 2 3 4 5 6 7 8 9 0 */
        static const char base[10]  = { '1','2','3','4','5','6','7','8','9','0' };
        static const char shft[10]  = { '!','@','#','$','%','^','&','*','(',')' };
        int idx = usage - JHID_USAGE_1;
        return shift ? shft[idx] : base[idx];
    }
    switch (usage) {
    case JHID_USAGE_ENTER:     return '\n';
    case JHID_USAGE_TAB:       return '\t';
    case JHID_USAGE_BACKSPACE: return '\b';
    case JHID_USAGE_SPACE:     return ' ';
    case JHID_USAGE_MINUS:     return shift ? '_' : '-';
    case JHID_USAGE_EQUAL:     return shift ? '+' : '=';
    default:                   return 0;
    }
}
