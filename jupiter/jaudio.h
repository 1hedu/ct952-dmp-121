/*
 * JupiterSDK on CT952 -- portable audio engine
 *
 * Extracted from Jupiter SDK lib/audio.c: the software PCM mixer with
 * fractional resampling, the NES/GB-style APU synth (2 pulse + wave +
 * noise, with envelopes), and the Genesis-style sound (6-channel
 * 4-operator FM + SN76489-style PSG).
 *
 * The V3s codec/DMA back-end is gone. This is a PULL model: call
 * jaudio_render() to produce N mono 16-bit samples into your own buffer,
 * then hand that buffer to whatever output path the platform has
 * (CT952: the HAL raw-PCM path -- see jshim.h; tests: a WAV file).
 * All active sources (PCM channels + APU + FM/PSG) are summed into the
 * same output with master-volume scaling and saturation.
 *
 * Sample rate: JAUDIO_RATE (default 44100 -- the CT952 tone path runs
 * the DAC at 44.1 kHz). Integer-only DSP; no floats anywhere.
 */
#ifndef JAUDIO_H
#define JAUDIO_H

#include "jup_types.h"

#ifndef JAUDIO_RATE
#define JAUDIO_RATE 44100
#endif

#define JAUDIO_MAX_CHANNELS 4

/* ---- Core ---- */
void jaudio_init(void);                    /* reset all synth/mixer state */
void jaudio_render(int16_t *out, uint32_t num_samples); /* mono 16-bit */
void jaudio_set_master_volume(uint8_t volume);          /* 0..255 */

/* ---- PCM sample channels (fractional resampling) ---- */
void jaudio_pcm_play(uint32_t channel, const int16_t *samples,
                     uint32_t length, uint8_t volume, uint8_t loop);
void jaudio_pcm_play_rate(uint32_t channel, const int16_t *samples,
                          uint32_t length, uint8_t volume, uint8_t loop,
                          uint32_t src_rate);
int  jaudio_pcm_channel_busy(uint32_t channel);
void jaudio_pcm_stop(uint32_t channel);

/* ---- APU synth (NES/GB style: 2 pulse + wave + noise) ---- */
void jaudio_apu_note_on(uint32_t channel, uint32_t frequency,
                        uint8_t volume, uint8_t duty);
void jaudio_apu_note_on_env(uint32_t channel, uint32_t frequency,
                            uint8_t volume, uint8_t duty,
                            int8_t env_dir, uint8_t env_period);
void jaudio_apu_note_off(uint32_t channel);
void jaudio_apu_wave_set(const uint8_t wave[32]);
void jaudio_apu_wave_on(uint32_t frequency, uint8_t volume_shift);
void jaudio_apu_wave_off(void);
void jaudio_apu_noise_on(uint8_t volume, uint8_t period_code,
                         uint8_t width_mode);
void jaudio_apu_noise_on_env(uint8_t volume, uint8_t period_code,
                             uint8_t width_mode, int8_t env_dir,
                             uint8_t env_period);
void jaudio_apu_noise_off(void);
void jaudio_apu_all_off(void);

/* ---- Genesis-style FM (YM2612-like, 6ch x 4op) + PSG ---- */
#define JFM_NUM_CH 6

void jaudio_fm_set_algorithm(uint32_t ch, uint8_t algorithm, uint8_t feedback);
void jaudio_fm_set_freq(uint32_t ch, uint32_t freq_hz);
void jaudio_fm_set_operator(uint32_t ch, uint32_t op,
    uint8_t mul, uint8_t tl, uint8_t ar, uint8_t dr,
    uint8_t sl, uint8_t sr, uint8_t rr);
void jaudio_fm_key_on(uint32_t ch);
void jaudio_fm_key_off(uint32_t ch);
void jaudio_psg_tone_on(uint32_t ch, uint32_t freq_hz, uint8_t vol);
void jaudio_psg_tone_off(uint32_t ch);
void jaudio_psg_noise_on(uint8_t vol, uint8_t rate, uint8_t periodic);
void jaudio_psg_noise_off(void);
void jaudio_genesis_all_off(void);

#endif /* JAUDIO_H */
