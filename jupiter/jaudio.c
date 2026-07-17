/*
 * JupiterSDK on CT952 -- portable audio engine.
 *
 * DSP code extracted from Jupiter SDK lib/audio.c (the portable half:
 * mixer, APU, FM, PSG), decoupled from the V3s codec/DMA back-end and
 * restructured as a pull-model renderer: jaudio_render() sums every
 * active source into a caller-supplied mono int16 buffer.
 *
 * Synth algorithms are unchanged from the SDK; only the output plumbing
 * and the sample-rate constant differ. All state is static and integer.
 */
#include "jaudio.h"

static uint8_t master_volume = 200;

/* ================================================================== */
/* PCM channels with fractional resampling                             */
/* ================================================================== */
/* Per-channel position is split: pos_int is the integer sample index,
 * pos_frac the Q16 fractional accumulator. Each output sample:
 * pos_frac += pos_step; pos_int += pos_frac >> 16; pos_frac &= 0xFFFF. */
typedef struct {
    const int16_t *samples;
    uint32_t length;
    uint32_t pos_int;
    uint32_t pos_frac;
    uint32_t pos_step;
    uint8_t volume, loop, active;
} pcm_channel_t;

static pcm_channel_t channels[JAUDIO_MAX_CHANNELS];

void jaudio_pcm_play(uint32_t ch, const int16_t *s, uint32_t len,
                     uint8_t vol, uint8_t loop)
{
    pcm_channel_t *c;
    if (ch >= JAUDIO_MAX_CHANNELS) return;
    c = &channels[ch];
    c->active = 0;
    c->samples = s; c->length = len; c->pos_int = 0; c->pos_frac = 0;
    c->pos_step = 1u << 16;        /* native-rate playback */
    c->volume = vol; c->loop = loop; c->active = 1;
}

void jaudio_pcm_play_rate(uint32_t ch, const int16_t *s, uint32_t len,
                          uint8_t vol, uint8_t loop, uint32_t src_rate)
{
    pcm_channel_t *c;
    if (ch >= JAUDIO_MAX_CHANNELS) return;
    if (src_rate == 0) src_rate = JAUDIO_RATE;
    c = &channels[ch];
    c->active = 0;
    c->samples = s; c->length = len; c->pos_int = 0; c->pos_frac = 0;
    c->pos_step = (uint32_t)(((uint64_t)src_rate << 16) / JAUDIO_RATE);
    c->volume = vol; c->loop = loop; c->active = 1;
}

int jaudio_pcm_channel_busy(uint32_t ch)
{
    if (ch >= JAUDIO_MAX_CHANNELS) return 0;
    return channels[ch].active;
}

void jaudio_pcm_stop(uint32_t ch)
{
    if (ch < JAUDIO_MAX_CHANNELS) channels[ch].active = 0;
}

void jaudio_set_master_volume(uint8_t v) { master_volume = v; }

static int32_t pcm_tick(void)
{
    int32_t sum = 0;
    int ch;
    for (ch = 0; ch < JAUDIO_MAX_CHANNELS; ch++) {
        pcm_channel_t *c = &channels[ch];
        if (!c->active) continue;
        sum += ((int32_t)c->samples[c->pos_int] * c->volume) >> 8;
        c->pos_frac += c->pos_step;
        c->pos_int  += c->pos_frac >> 16;
        c->pos_frac &= 0xFFFF;
        if (c->pos_int >= c->length) {
            if (c->loop) c->pos_int -= c->length;
            else c->active = 0;
        }
    }
    return sum;
}

/* ================================================================== */
/* APU (NES/GB style)                                                  */
/* ================================================================== */
static const int8_t duty_table[4][8] = {
    {0,0,0,0,0,0,0,1},{1,0,0,0,0,0,0,1},{1,0,0,0,0,1,1,1},{0,1,1,1,1,1,1,0},
};

typedef struct {
    uint32_t phase, phase_step;
    uint8_t duty, volume, env_init_vol;
    int8_t env_dir;
    uint8_t env_period;
    uint16_t env_timer;
    uint8_t active;
} apu_pulse_t;

typedef struct {
    uint32_t phase, phase_step;
    uint8_t wave[32], volume_shift, active;
} apu_wave_t;

typedef struct {
    uint16_t lfsr;
    uint32_t timer, period;
    uint8_t width_mode, volume, env_init_vol;
    int8_t env_dir;
    uint8_t env_period;
    uint16_t env_timer;
    uint8_t active;
} apu_noise_t;

static apu_pulse_t apu_pulse[2];
static apu_wave_t  apu_wave;
static apu_noise_t apu_noise;
static uint32_t    apu_env_tick_ctr = 0;

/* Envelope tick interval in samples (was 750 @ 48 kHz in the SDK) */
#define ENV_INTERVAL (JAUDIO_RATE / 64)

/* Must fill the full 32-bit range: duty uses top 3 bits, wave top 5 */
static uint32_t freq_to_step(uint32_t f)
{
    return f * (0xFFFFFFFFU / JAUDIO_RATE);
}

static int16_t apu_tick(void)
{
    int32_t sum = 0;
    int i;
    for (i = 0; i < 2; i++) {
        apu_pulse_t *p = &apu_pulse[i];
        uint32_t idx;
        if (!p->active) continue;
        idx = (p->phase >> 29) & 7;
        sum += (duty_table[p->duty][idx] ? (int32_t)p->volume
                                         : -(int32_t)p->volume) * 546;
        p->phase += p->phase_step;
    }
    if (apu_wave.active) {
        uint32_t idx = (apu_wave.phase >> 27) & 31;
        int32_t s = ((int32_t)apu_wave.wave[idx] - 8) >> apu_wave.volume_shift;
        sum += s * 1092;
        apu_wave.phase += apu_wave.phase_step;
    }
    if (apu_noise.active) {
        apu_noise.timer++;
        if (apu_noise.timer >= apu_noise.period) {
            uint16_t x;
            apu_noise.timer = 0;
            x = (apu_noise.lfsr ^ (apu_noise.lfsr >> 1)) & 1;
            apu_noise.lfsr = (apu_noise.lfsr >> 1) | (uint16_t)(x << 14);
            if (apu_noise.width_mode)
                apu_noise.lfsr = (uint16_t)((apu_noise.lfsr & ~(1 << 6)) | (x << 6));
        }
        sum += ((apu_noise.lfsr & 1) ? (int32_t)apu_noise.volume
                                     : -(int32_t)apu_noise.volume) * 546;
    }
    if (sum > 32767) sum = 32767;
    if (sum < -32768) sum = -32768;
    return (int16_t)sum;
}

static void apu_envelope_tick(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        apu_pulse_t *p = &apu_pulse[i];
        if (!p->active || p->env_dir == 0 || p->env_period == 0) continue;
        if (++p->env_timer >= p->env_period) {
            int8_t nv;
            p->env_timer = 0;
            nv = (int8_t)((int8_t)p->volume + p->env_dir);
            if (nv >= 0 && nv <= 15) p->volume = (uint8_t)nv;
        }
    }
    {
        apu_noise_t *n = &apu_noise;
        if (!n->active || n->env_dir == 0 || n->env_period == 0) return;
        if (++n->env_timer >= n->env_period) {
            int8_t nv;
            n->env_timer = 0;
            nv = (int8_t)((int8_t)n->volume + n->env_dir);
            if (nv >= 0 && nv <= 15) n->volume = (uint8_t)nv;
        }
    }
}

void jaudio_apu_note_on(uint32_t ch, uint32_t f, uint8_t v, uint8_t d)
{
    apu_pulse_t *p;
    if (ch > 1) return;
    p = &apu_pulse[ch];
    p->phase = 0; p->phase_step = freq_to_step(f);
    p->duty = d & 3; p->volume = v & 15;
    p->env_init_vol = p->volume;
    p->env_dir = 0; p->env_period = 0; p->env_timer = 0;
    p->active = 1;
}

void jaudio_apu_note_on_env(uint32_t ch, uint32_t f, uint8_t v, uint8_t d,
                            int8_t ed, uint8_t ep)
{
    apu_pulse_t *p;
    if (ch > 1) return;
    p = &apu_pulse[ch];
    p->phase = 0; p->phase_step = freq_to_step(f);
    p->duty = d & 3; p->volume = v & 15;
    p->env_init_vol = p->volume;
    p->env_dir = ed; p->env_period = ep; p->env_timer = 0;
    p->active = 1;
}

void jaudio_apu_note_off(uint32_t ch)
{
    if (ch <= 1) apu_pulse[ch].active = 0;
}

void jaudio_apu_wave_set(const uint8_t w[32])
{
    int i;
    for (i = 0; i < 32; i++) apu_wave.wave[i] = w[i] & 0xF;
}

void jaudio_apu_wave_on(uint32_t f, uint8_t vs)
{
    apu_wave.phase = 0;
    apu_wave.phase_step = freq_to_step(f);
    apu_wave.volume_shift = vs & 3;
    apu_wave.active = 1;
}

void jaudio_apu_wave_off(void) { apu_wave.active = 0; }

void jaudio_apu_noise_on(uint8_t v, uint8_t pc, uint8_t wm)
{
    static const uint32_t d[8] = {1, 2, 4, 6, 8, 10, 12, 14};
    apu_noise.lfsr = 0x7FFF;
    apu_noise.timer = 0;
    apu_noise.period = (pc < 8) ? d[pc] : 14;
    apu_noise.width_mode = wm;
    apu_noise.volume = v & 15;
    apu_noise.env_init_vol = apu_noise.volume;
    apu_noise.env_dir = 0; apu_noise.env_period = 0; apu_noise.env_timer = 0;
    apu_noise.active = 1;
}

void jaudio_apu_noise_on_env(uint8_t v, uint8_t pc, uint8_t wm,
                             int8_t ed, uint8_t ep)
{
    jaudio_apu_noise_on(v, pc, wm);
    apu_noise.env_dir = ed;
    apu_noise.env_period = ep;
}

void jaudio_apu_noise_off(void) { apu_noise.active = 0; }

void jaudio_apu_all_off(void)
{
    apu_pulse[0].active = 0;
    apu_pulse[1].active = 0;
    apu_wave.active = 0;
    apu_noise.active = 0;
}

/* ================================================================== */
/* Genesis-style sound (YM2612-like FM + SN76489-style PSG)            */
/* ================================================================== */

/* 256-entry quarter sine table (0 to pi/2), same data as the SDK */
static const int16_t fm_qsine[256] = {
        0,   201,   402,   603,   804,  1005,  1206,  1407,
     1608,  1809,  2009,  2210,  2410,  2611,  2811,  3012,
     3212,  3412,  3612,  3811,  4011,  4210,  4410,  4609,
     4808,  5007,  5205,  5404,  5602,  5800,  5998,  6195,
     6393,  6590,  6786,  6983,  7179,  7375,  7571,  7767,
     7962,  8157,  8351,  8545,  8739,  8933,  9126,  9319,
     9512,  9704,  9896, 10087, 10278, 10469, 10659, 10849,
    11039, 11228, 11417, 11605, 11793, 11980, 12167, 12353,
    12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269,
    15446, 15623, 15800, 15976, 16151, 16325, 16499, 16673,
    16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357,
    19519, 19680, 19841, 20000, 20159, 20317, 20475, 20631,
    20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027,
    23170, 23311, 23452, 23592, 23731, 23870, 24007, 24143,
    24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198,
    26319, 26438, 26556, 26674, 26790, 26905, 27019, 27133,
    27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803,
    28898, 28992, 29085, 29177, 29268, 29358, 29447, 29534,
    29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783,
    30852, 30919, 30985, 31050, 31113, 31176, 31237, 31297,
    31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098,
    32137, 32176, 32213, 32250, 32285, 32318, 32351, 32382,
    32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717,
    32728, 32737, 32745, 32752, 32757, 32761, 32765, 32766,
};

/* Full sine lookup from quarter table: 10-bit phase -> signed 16-bit */
static int16_t fm_sine(uint32_t phase10)
{
    uint32_t idx = phase10 & 0x3FF;
    uint32_t pos = idx & 0xFF;
    int16_t val = fm_qsine[(idx & 0x100) ? (255 - pos) : pos];
    return (idx & 0x200) ? (int16_t)(-val) : val;
}

enum { ENV_OFF, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };

/* Rate table: maps rate 0-31 to envelope step per tick */
static const uint16_t env_rate_tab[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28,
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 256, 320, 512, 768, 1023,
};

typedef struct {
    uint32_t phase;
    uint32_t phase_inc;
    uint8_t  mul;
    int8_t   detune;
    uint8_t  tl;
    uint8_t  ar, dr, sr, rr, sl;
    uint16_t env;
    uint8_t  env_state;
} fm_op_t;

typedef struct {
    fm_op_t  op[4];
    uint32_t freq_step;
    uint8_t  algorithm;
    uint8_t  feedback;
    int32_t  fb_out[2];
    uint8_t  active;
} fm_ch_t;

static fm_ch_t fm_ch[JFM_NUM_CH];

/* Envelope prescaler: the real YM2612 advances envelopes every 3rd
 * sample. fm_env_tick_now is set per-sample by the render loop. */
static uint8_t fm_env_tick_now = 1;
static uint8_t fm_env_div_count = 0;

typedef struct {
    uint32_t phase;
    uint32_t phase_step;
    uint8_t  vol;            /* 0=max, 15=off (Genesis convention) */
    uint8_t  active;
} psg_tone_t;

static psg_tone_t psg_tone[3];
static apu_noise_t psg_noise_ch;  /* reuse the noise struct */

static void fm_env_tick(fm_op_t *op)
{
    if (!fm_env_tick_now) return;
    switch (op->env_state) {
    case ENV_ATK: {
        uint16_t rate = env_rate_tab[op->ar & 31];
        uint16_t dec;
        if (rate == 0) break;
        /* YM2612 attack: exponential decrease toward 0 (not linear).
         * dec = rate * (1 + env/16) -- fast at top, slows near full. */
        dec = (uint16_t)(rate + ((uint32_t)rate * op->env >> 4));
        if (op->env > dec)
            op->env = (uint16_t)(op->env - dec);
        else {
            op->env = 0;
            op->env_state = ENV_DEC;
        }
        break;
    }
    case ENV_DEC: {
        uint16_t target = (uint16_t)((op->sl & 15) * 64); /* SL 0-15 -> 0-960 */
        uint16_t rate = env_rate_tab[op->dr & 31];
        if (op->env < target) {
            op->env = (uint16_t)(op->env + rate);
            if (op->env >= target) {
                op->env = target;
                op->env_state = ENV_SUS;
            }
        } else {
            op->env_state = ENV_SUS;
        }
        break;
    }
    case ENV_SUS: {
        uint16_t rate = env_rate_tab[op->sr & 31];
        if (op->env < 1023) {
            op->env = (uint16_t)(op->env + rate);
            if (op->env > 1023) op->env = 1023;
        }
        break;
    }
    case ENV_REL: {
        uint16_t rate = env_rate_tab[(op->rr & 15) * 2 + 1];
        if (op->env < 1023) {
            op->env = (uint16_t)(op->env + rate);
            if (op->env > 1023) {
                op->env = 1023;
                op->env_state = ENV_OFF;
            }
        }
        break;
    }
    default:
        op->env = 1023;
        break;
    }
}

static int32_t fm_op_calc(fm_op_t *op, int32_t modulation)
{
    uint32_t ph, phase10, mul_inc;
    int32_t out;

    if (op->env_state == ENV_OFF) return 0;

    /* Phase with modulation (modulation is +/-32767 range) */
    ph = op->phase + (uint32_t)(modulation << 5);
    phase10 = (ph >> 22) & 0x3FF;

    out = fm_sine(phase10);

    /* Envelope: env 0=full, 1023=silent */
    out = (out * (1023 - (int32_t)op->env)) >> 10;

    /* Total level: tl 0=full, 127=silent */
    out = (out * (127 - (int32_t)op->tl)) >> 7;

    /* Advance phase with multiplier */
    mul_inc = (op->mul == 0) ? (op->phase_inc >> 1)
                             : (op->phase_inc * op->mul);
    op->phase += mul_inc + (uint32_t)(int32_t)op->detune;

    fm_env_tick(op);

    return out;
}

static int32_t fm_ch_tick(fm_ch_t *ch)
{
    fm_op_t *o = ch->op;
    int32_t fb = 0;
    int32_t out = 0;
    int32_t o1, o2, o3, o4;

    if (!ch->active) return 0;

    /* OP1 self-feedback */
    if (ch->feedback > 0)
        fb = (ch->fb_out[0] + ch->fb_out[1]) >> (10 - ch->feedback);

    /*
     * YM2612 algorithms (C = carrier -> output, M = modulator):
     * 0: [1->2->3->4]      1: [1+2]->3->4      2: [2->3]+[1->]->4
     * 3: [1->2]+[3->]->4   4: [1->2]+[3->4]    5: 1->[2]+[3]+[4]
     * 6: [1->2]+3+4      7: 1+2+3+4
     */
    switch (ch->algorithm) {
    case 0:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], o1);
        o3 = fm_op_calc(&o[2], o2);
        o4 = fm_op_calc(&o[3], o3);
        out = o4;
        break;
    case 1:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], 0);
        o3 = fm_op_calc(&o[2], o1 + o2);
        o4 = fm_op_calc(&o[3], o3);
        out = o4;
        break;
    case 2:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], 0);
        o3 = fm_op_calc(&o[2], o2);
        o4 = fm_op_calc(&o[3], o1 + o3);
        out = o4;
        break;
    case 3:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], o1);
        o3 = fm_op_calc(&o[2], 0);
        o4 = fm_op_calc(&o[3], o2 + o3);
        out = o4;
        break;
    case 4:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], o1);
        o3 = fm_op_calc(&o[2], 0);
        o4 = fm_op_calc(&o[3], o3);
        out = o2 + o4;
        break;
    case 5:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], o1);
        o3 = fm_op_calc(&o[2], o1);
        o4 = fm_op_calc(&o[3], o1);
        out = o2 + o3 + o4;
        break;
    case 6:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], o1);
        o3 = fm_op_calc(&o[2], 0);
        o4 = fm_op_calc(&o[3], 0);
        out = o2 + o3 + o4;
        break;
    case 7:
        o1 = fm_op_calc(&o[0], fb);
        o2 = fm_op_calc(&o[1], 0);
        o3 = fm_op_calc(&o[2], 0);
        o4 = fm_op_calc(&o[3], 0);
        out = o1 + o2 + o3 + o4;
        break;
    default:
        o1 = fm_op_calc(&o[0], fb);
        out = 0;
        break;
    }

    /* Update feedback history (always from OP1) */
    ch->fb_out[1] = ch->fb_out[0];
    ch->fb_out[0] = o1;

    return out;
}

static int16_t psg_tone_tick(psg_tone_t *t)
{
    int32_t out, atten;
    if (!t->active || t->vol >= 15) return 0;
    out = (t->phase & 0x80000000U) ? 1 : -1;
    atten = 15 - t->vol;          /* 0=silent, 15=max */
    out = out * atten * 546;      /* scale to +/-8190 range */
    t->phase += t->phase_step;
    return (int16_t)out;
}

static int32_t genesis_tick(void)
{
    int32_t sum = 0;
    int c, t;

    /* Envelope prescaler: tick every 3rd sample (matches YM2612 hw) */
    fm_env_tick_now = (fm_env_div_count == 0) ? 1 : 0;
    if (++fm_env_div_count >= 3) fm_env_div_count = 0;

    for (c = 0; c < JFM_NUM_CH; c++)
        sum += fm_ch_tick(&fm_ch[c]);

    for (t = 0; t < 3; t++)
        sum += psg_tone_tick(&psg_tone[t]);

    if (psg_noise_ch.active) {
        int32_t nout;
        psg_noise_ch.timer++;
        if (psg_noise_ch.timer >= psg_noise_ch.period) {
            uint16_t x;
            psg_noise_ch.timer = 0;
            x = (psg_noise_ch.lfsr ^ (psg_noise_ch.lfsr >> 1)) & 1;
            psg_noise_ch.lfsr = (psg_noise_ch.lfsr >> 1) | (uint16_t)(x << 14);
            if (psg_noise_ch.width_mode)
                psg_noise_ch.lfsr =
                    (uint16_t)((psg_noise_ch.lfsr & ~(1 << 6)) | (x << 6));
        }
        nout = (psg_noise_ch.lfsr & 1) ? (int32_t)psg_noise_ch.volume
                                       : -(int32_t)psg_noise_ch.volume;
        sum += nout * 546;
    }

    return sum;
}

/* ---- FM public API ---- */

void jaudio_fm_set_algorithm(uint32_t ch, uint8_t alg, uint8_t fb)
{
    if (ch >= JFM_NUM_CH) return;
    fm_ch[ch].algorithm = alg & 7;
    fm_ch[ch].feedback = fb & 7;
}

void jaudio_fm_set_freq(uint32_t ch, uint32_t freq_hz)
{
    int i;
    if (ch >= JFM_NUM_CH) return;
    fm_ch[ch].freq_step = freq_hz * (0xFFFFFFFFU / JAUDIO_RATE);
    for (i = 0; i < 4; i++)
        fm_ch[ch].op[i].phase_inc = fm_ch[ch].freq_step;
}

void jaudio_fm_set_operator(uint32_t ch, uint32_t op,
    uint8_t mul, uint8_t tl, uint8_t ar, uint8_t dr,
    uint8_t sl, uint8_t sr, uint8_t rr)
{
    fm_op_t *o;
    if (ch >= JFM_NUM_CH || op >= 4) return;
    o = &fm_ch[ch].op[op];
    o->mul = mul & 15;
    o->tl  = tl & 127;
    o->ar  = ar & 31;
    o->dr  = dr & 31;
    o->sl  = sl & 15;
    o->sr  = sr & 31;
    o->rr  = rr & 15;
    o->detune = 0;
}

void jaudio_fm_key_on(uint32_t ch)
{
    int i;
    if (ch >= JFM_NUM_CH) return;
    fm_ch[ch].active = 1;
    for (i = 0; i < 4; i++) {
        fm_ch[ch].op[i].phase = 0;
        fm_ch[ch].op[i].env = 1023;
        fm_ch[ch].op[i].env_state = ENV_ATK;
    }
    fm_ch[ch].fb_out[0] = fm_ch[ch].fb_out[1] = 0;
}

void jaudio_fm_key_off(uint32_t ch)
{
    int i;
    if (ch >= JFM_NUM_CH) return;
    for (i = 0; i < 4; i++) {
        if (fm_ch[ch].op[i].env_state != ENV_OFF)
            fm_ch[ch].op[i].env_state = ENV_REL;
    }
}

/* ---- PSG API ---- */

void jaudio_psg_tone_on(uint32_t ch, uint32_t freq_hz, uint8_t vol)
{
    if (ch >= 3) return;
    psg_tone[ch].phase = 0;
    psg_tone[ch].phase_step = freq_hz * (0xFFFFFFFFU / JAUDIO_RATE);
    psg_tone[ch].vol = vol & 15;
    psg_tone[ch].active = 1;
}

void jaudio_psg_tone_off(uint32_t ch)
{
    if (ch >= 3) return;
    psg_tone[ch].active = 0;
}

void jaudio_psg_noise_on(uint8_t vol, uint8_t rate, uint8_t periodic)
{
    static const uint32_t d[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    psg_noise_ch.lfsr = 0x7FFF;
    psg_noise_ch.timer = 0;
    psg_noise_ch.period = (rate < 8) ? d[rate] : 128;
    psg_noise_ch.width_mode = periodic;
    psg_noise_ch.volume = (uint8_t)(15 - (vol & 15));
    psg_noise_ch.active = 1;
}

void jaudio_psg_noise_off(void)
{
    psg_noise_ch.active = 0;
}

void jaudio_genesis_all_off(void)
{
    int c, o, t;
    for (c = 0; c < JFM_NUM_CH; c++) {
        fm_ch[c].active = 0;
        for (o = 0; o < 4; o++)
            fm_ch[c].op[o].env_state = ENV_OFF;
    }
    for (t = 0; t < 3; t++)
        psg_tone[t].active = 0;
    psg_noise_ch.active = 0;
}

/* ================================================================== */
/* Core                                                                */
/* ================================================================== */

void jaudio_init(void)
{
    int i;
    for (i = 0; i < JAUDIO_MAX_CHANNELS; i++)
        channels[i].active = 0;
    jaudio_apu_all_off();
    jaudio_genesis_all_off();
    apu_env_tick_ctr = 0;
    fm_env_div_count = 0;
    master_volume = 200;
}

void jaudio_render(int16_t *out, uint32_t num_samples)
{
    uint32_t s;
    for (s = 0; s < num_samples; s++) {
        int32_t sum = pcm_tick();
        sum += apu_tick();
        sum += genesis_tick();

        if (++apu_env_tick_ctr >= ENV_INTERVAL) {
            apu_env_tick_ctr = 0;
            apu_envelope_tick();
        }

        sum = (sum * master_volume) >> 8;
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        out[s] = (int16_t)sum;
    }
}
