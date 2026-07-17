/*
 * ct952emu CPU test payload. Compiled twice: cross for SPARC (run in
 * the emulator) and natively (reference result). testmain()'s value
 * must match bit-for-bit.
 *
 * Deliberately exercises: deep recursion (register-window over/
 * underflow traps), umul/smul/udiv/sdiv + remainders, all shifts,
 * signed/unsigned compares and annulled branches, byte/half/word/
 * double loads and stores, tail calls, 64-bit arithmetic (compiler-
 * expanded), and switch tables.
 */
#include "testapi.h"

static unsigned fib(unsigned n)
{
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static unsigned crcish(const unsigned char *p, unsigned n)
{
    unsigned crc = 0xFFFFFFFFu;
    unsigned i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static unsigned mixmath(unsigned seed)
{
    unsigned a = seed, acc = 0;
    int i;
    for (i = 1; i <= 97; i++) {
        unsigned u = (unsigned)i;
        acc += a / u + a % u;
        acc ^= (a * u);
        acc += (unsigned)((int)a / (int)(u | 1));
        acc ^= (unsigned)((int)a % (int)(u | 1));
        acc += a << (i & 31);
        acc ^= a >> (i & 31);
        acc += (unsigned)((int)a >> (i & 31));
        a = a * 1103515245u + 12345u;
    }
    return acc;
}

static unsigned long long mix64(unsigned seed)
{
    unsigned long long x = seed;
    int i;
    for (i = 0; i < 40; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        x ^= x >> 17;
        x += (unsigned long long)i << 33;
    }
    return x;
}

static short sarr[64];
static unsigned char barr[256];
static unsigned long long darr[16];

static unsigned memops(void)
{
    unsigned acc = 0;
    int i;
    for (i = 0; i < 256; i++)
        barr[i] = (unsigned char)(i * 7 + 3);
    for (i = 0; i < 64; i++)
        sarr[i] = (short)(i * -321 + 17);
    for (i = 0; i < 16; i++)
        darr[i] = mix64((unsigned)i);
    for (i = 0; i < 64; i++)
        acc += (unsigned)sarr[i] * 3u;
    for (i = 0; i < 16; i++)
        acc ^= (unsigned)(darr[i] >> 32) + (unsigned)darr[i];
    acc ^= crcish(barr, sizeof(barr));
    return acc;
}

static unsigned dispatch(unsigned v)
{
    unsigned acc = 0;
    int i;
    for (i = 0; i < 32; i++) {
        switch ((v + (unsigned)i) & 7) {
        case 0: acc += v * 3; break;
        case 1: acc ^= v >> 3; break;
        case 2: acc += v | 0x1234; break;
        case 3: acc ^= v & 0xF0F0; break;
        case 4: acc += ~v; break;
        case 5: acc ^= v << 5; break;
        case 6: acc += v - i; break;
        default: acc ^= v + i; break;
        }
        v = v * 69069u + 1u;
    }
    return acc;
}

unsigned testmain(void)
{
    unsigned r = 0;
    r ^= fib(17);              /* deep windows */
    r += mixmath(0xDEADBEEFu);
    r ^= memops();
    r += dispatch(r);
    r ^= (unsigned)(mix64(r) >> 13);
    return r;
}
