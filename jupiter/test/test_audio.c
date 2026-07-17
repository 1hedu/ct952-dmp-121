/*
 * JupiterSDK on CT952 -- audio engine verification.
 *
 * Exercises every jaudio source: PCM channel with resampling, APU
 * (pulse/wave/noise + envelopes), FM (several algorithms) and PSG.
 * Prints a CRC32 of each rendered buffer and writes a WAV for
 * listening. Run natively and under qemu-sparc64; identical CRCs
 * prove endian-cleanliness of the DSP code.
 */
#include <stdio.h>
#include <string.h>
#include "jaudio.h"

#define RENDER_SAMPLES (JAUDIO_RATE / 2)   /* 0.5 s per section */

static int16_t buf[RENDER_SAMPLES * 4];

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

/* Write a mono 16-bit WAV with explicit little-endian byte order so the
 * file is identical regardless of host endianness. */
static void wav_u32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f); fputc((int)((v >> 24) & 0xFF), f);
}
static void wav_u16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f);
}
static void dump_wav(const char *path, const int16_t *s, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); wav_u32(f, 36 + n * 2);
    fwrite("WAVEfmt ", 1, 8, f);
    wav_u32(f, 16); wav_u16(f, 1); wav_u16(f, 1);
    wav_u32(f, JAUDIO_RATE); wav_u32(f, JAUDIO_RATE * 2);
    wav_u16(f, 2); wav_u16(f, 16);
    fwrite("data", 1, 4, f); wav_u32(f, n * 2);
    for (i = 0; i < n; i++) wav_u16(f, (uint16_t)s[i]);
    fclose(f);
}

static void crc_section(const char *name, const int16_t *s, uint32_t n)
{
    /* CRC over explicit little-endian bytes so both hosts agree */
    static uint8_t tmp[sizeof(buf)];
    uint32_t i;
    for (i = 0; i < n; i++) {
        tmp[i * 2] = (uint8_t)(s[i] & 0xFF);
        tmp[i * 2 + 1] = (uint8_t)((s[i] >> 8) & 0xFF);
    }
    printf("CRC %s=%08x\n", name, (unsigned)crc32_buf(tmp, n * 2));
}

int main(void)
{
    uint32_t total = 0;
    static const uint32_t notes[4] = {523, 659, 784, 1047};
    uint32_t i;

    /* ---- Section 1: APU jingle (the demo app's entry sound) ---- */
    jaudio_init();
    jaudio_apu_noise_on_env(10, 4, 0, -1, 2);
    for (i = 0; i < 4; i++) {
        jaudio_apu_note_on_env(0, notes[i], 12, 2, -1, 6);
        jaudio_render(buf + total, JAUDIO_RATE / 10);
        total += JAUDIO_RATE / 10;
        jaudio_apu_noise_off();
    }
    jaudio_apu_all_off();
    crc_section("apu_jingle", buf, total);

    /* ---- Section 2: APU wave + second pulse + noise widths ---- */
    {
        uint8_t wave[32];
        for (i = 0; i < 32; i++) wave[i] = (uint8_t)(i & 15);  /* saw */
        jaudio_apu_wave_set(wave);
        jaudio_apu_wave_on(220, 0);
        jaudio_apu_note_on(1, 330, 10, 1);
        jaudio_apu_noise_on(8, 6, 1);
        jaudio_render(buf + total, RENDER_SAMPLES / 2);
        crc_section("apu_wave", buf + total, RENDER_SAMPLES / 2);
        total += RENDER_SAMPLES / 2;
        jaudio_apu_all_off();
    }

    /* ---- Section 3: FM -- algorithm 0 (serial) and 4 (two pairs) ---- */
    jaudio_fm_set_algorithm(0, 0, 3);
    jaudio_fm_set_operator(0, 0, 2, 30, 28, 8, 4, 4, 8);
    jaudio_fm_set_operator(0, 1, 1, 20, 26, 6, 6, 3, 8);
    jaudio_fm_set_operator(0, 2, 1, 10, 24, 6, 6, 3, 8);
    jaudio_fm_set_operator(0, 3, 1, 0, 28, 10, 5, 2, 10);
    jaudio_fm_set_freq(0, 220);
    jaudio_fm_key_on(0);

    jaudio_fm_set_algorithm(1, 4, 0);
    jaudio_fm_set_operator(1, 0, 4, 40, 28, 8, 4, 4, 8);
    jaudio_fm_set_operator(1, 1, 1, 4, 28, 10, 5, 2, 10);
    jaudio_fm_set_operator(1, 2, 2, 40, 28, 8, 4, 4, 8);
    jaudio_fm_set_operator(1, 3, 1, 4, 28, 10, 5, 2, 10);
    jaudio_fm_set_freq(1, 277);
    jaudio_fm_key_on(1);

    jaudio_render(buf + total, RENDER_SAMPLES / 2);
    jaudio_fm_key_off(0);
    jaudio_fm_key_off(1);
    jaudio_render(buf + total + RENDER_SAMPLES / 2, RENDER_SAMPLES / 4);
    crc_section("fm", buf + total, RENDER_SAMPLES / 2 + RENDER_SAMPLES / 4);
    total += RENDER_SAMPLES / 2 + RENDER_SAMPLES / 4;
    jaudio_genesis_all_off();

    /* ---- Section 4: PSG + PCM resampling ---- */
    {
        static int16_t sample[100];
        for (i = 0; i < 100; i++)
            sample[i] = (int16_t)((i < 50) ? 8000 : -8000);  /* square */
        jaudio_psg_tone_on(0, 440, 13);
        jaudio_psg_tone_on(1, 554, 10);
        jaudio_psg_noise_on(9, 5, 0);
        jaudio_pcm_play_rate(0, sample, 100, 200, 1, 22050);
        jaudio_render(buf + total, RENDER_SAMPLES / 2);
        crc_section("psg_pcm", buf + total, RENDER_SAMPLES / 2);
        total += RENDER_SAMPLES / 2;
        jaudio_genesis_all_off();
        jaudio_pcm_stop(0);
    }

    crc_section("all", buf, total);
    dump_wav("out_audio.wav", buf, total);

    /* Sanity: output must not be silent */
    {
        int nonzero = 0;
        for (i = 0; i < total; i++)
            if (buf[i] != 0) { nonzero = 1; break; }
        if (!nonzero) { printf("FAIL: silent output\n"); return 1; }
    }

    printf("audio tests OK (%u samples)\n", (unsigned)total);
    return 0;
}
