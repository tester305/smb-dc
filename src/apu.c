/*
 * Small software APU: 2 pulse, triangle, noise (no DMC - Super Mario Bros. doesn't use it).
 *
 * Every channel is box-filtered: instead of sampling the square/triangle/noise waves at
 * 44.1 kHz (which aliases into harsh, inharmonic whine), the wave is averaged over each
 * output sample.  The mix then goes through a DC-blocking high-pass filter (the NES output
 * is unipolar, which otherwise makes every note start/stop thump).
 *
 * --- this revision --------------------------------------------------------------------------
 * Speed (hot loop no longer does any float division per sample):
 *  - Each channel's phase step (dx), its sweep-mute flag and its output volume are CACHED
 *    and only recomputed when the relevant registers actually change (register writes and
 *    sweep clocking at 120 Hz) - previously 3 float divisions + target math ran every sample.
 *  - Wave integral/average use precomputed reciprocals (multiply) instead of divisions.
 *  - Noise divider/LFSR runs in a tight local loop with the volume level hoisted out (it
 *    can only change on quarter-frame clocks or register writes, never inside a sample).
 * Correctness:
 *  - $400E noise-period writes now take effect immediately; the old code kept the previous
 *    divider countdown, which audibly delayed/smeared fast drum rolls and pitch changes.
 *  - $4017 frame-counter mode is implemented (5-step sequence) instead of ignored.
 *  - Duty phase is no longer reset on $4003 writes - real hardware doesn't reset it, and
 *    resetting caused small waveform discontinuities on rapidly retriggered effects.
 *  - Sweep updates never write back a wrapped negative period (defensive clamp).
 * Usage tips:
 *  - Build with -O2 or -O3; at -O0 this file runs ~5-8x slower for no benefit.
 *  - Call apu_generate() in batches (per audio callback / frame), never 1 sample per call.
 * -------------------------------------------------------------------------------------------
 */
#include "nes.h"
#include <string.h>

#define CPU_HZ    1789773.0f
#define APU_GAIN  30000.0f      /* output loudness; lower this if it is still too loud */
#define HP_COEF   0.996f        /* DC blocker, about 28 Hz corner */

#define CPS       (CPU_HZ / NES_AUDIO_RATE)   /* CPU cycles per output sample */

typedef struct {
    uint8_t  enabled, duty, loop, const_vol, vol, length;
    uint8_t  env_start, env_div, env_decay;
    uint8_t  sw_en, sw_period, sw_neg, sw_shift, sw_reload, sw_div, ones_comp;
    uint16_t timer;
    float    phase;
    /* cached values, refreshed only when the relevant registers change */
    float    dx, inv_dx;   /* phase advance per output sample (and its reciprocal) */
    uint8_t  gate_ok;      /* timer >= 8 and (negate mode or positive target in range) */
    uint8_t  out_vol;      /* const_vol ? vol : env_decay */
} Pulse;

static Pulse pu[2];
static struct {
    uint8_t  enabled, ctrl, lin_reload_val, lin_reload, lin, length;
    uint16_t timer;
    float    phase, dx, inv_dx;
} tri;
static struct {
    uint8_t  enabled, loop, const_vol, vol, length, env_start, env_div, env_decay, mode, per;
    uint16_t lfsr;
    float    period, t;    /* divider period in CPU cycles, phase within current step */
} nz;
static float fc_acc, hp_x, hp_y;
static int   fc_step, fc_mode;
static float duty_val[4][8], duty_cum[4][9], tri_val[32], tri_cum[33];

static const uint8_t len_tab[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t duty_tab[4][8] = {
    {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
static const uint16_t noise_per[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint8_t tri_seq[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

/* refresh cached pulse values - call whenever timer or sweep registers change */
static void pulse_cache(Pulse *p)
{
    p->dx     = CPS / (16.0f * (p->timer + 1));
    p->inv_dx = 1.0f / p->dx;
    p->gate_ok = (uint8_t)(p->timer >= 8 &&
                 (p->sw_neg || p->timer + (p->timer >> p->sw_shift) <= 0x7FF));
}

static void tri_cache(void)
{
    tri.dx     = CPS / (32.0f * (tri.timer + 1));
    tri.inv_dx = 1.0f / tri.dx;
}

void apu_reset(void)
{
    int i, d;
    memset(pu, 0, sizeof pu);
    memset(&tri, 0, sizeof tri);
    memset(&nz, 0, sizeof nz);
    pu[0].ones_comp = 1;
    nz.lfsr = 1;
    nz.period = (float)noise_per[0];   /* keep the divider loop well-defined */
    fc_acc = 0; fc_step = 0; fc_mode = 0; hp_x = hp_y = 0;

    for (d = 0; d < 4; d++) {
        duty_cum[d][0] = 0;
        for (i = 0; i < 8; i++) {
            duty_val[d][i] = duty_tab[d][i];
            duty_cum[d][i + 1] = duty_cum[d][i] + duty_val[d][i] / 8.0f;
        }
    }
    tri_cum[0] = 0;
    for (i = 0; i < 32; i++) {
        tri_val[i] = tri_seq[i];
        tri_cum[i + 1] = tri_cum[i] + tri_val[i] / 32.0f;
    }
    pulse_cache(&pu[0]);
    pulse_cache(&pu[1]);
    tri_cache();
}

void apu_write(uint16_t a, uint8_t d)
{
    if (a >= 0x4000 && a <= 0x4007) {
        Pulse *p = &pu[(a >> 2) & 1];
        switch (a & 3) {
        case 0:
            p->duty = d >> 6; p->loop = (d >> 5) & 1;
            p->const_vol = (d >> 4) & 1; p->vol = d & 15;
            p->out_vol = p->const_vol ? p->vol : p->env_decay;
            break;
        case 1:
            p->sw_en = d >> 7; p->sw_period = (d >> 4) & 7;
            p->sw_neg = (d >> 3) & 1; p->sw_shift = d & 7;
            p->sw_reload = 1;
            pulse_cache(p);               /* gate_ok depends on negate/shift */
            break;
        case 2:
            p->timer = (uint16_t)((p->timer & 0x700) | d);
            pulse_cache(p);
            break;
        case 3:
            p->timer = (uint16_t)((p->timer & 0xFF) | ((d & 7) << 8));
            if (p->enabled) p->length = len_tab[d >> 3];
            p->env_start = 1;             /* envelope restarts; duty phase does not */
            pulse_cache(p);
            break;
        }
        return;
    }
    switch (a) {
    case 0x4008: tri.ctrl = d >> 7; tri.lin_reload_val = d & 0x7F; break;
    case 0x400A:
        tri.timer = (uint16_t)((tri.timer & 0x700) | d);
        tri_cache();
        break;
    case 0x400B:
        tri.timer = (uint16_t)((tri.timer & 0xFF) | ((d & 7) << 8));
        if (tri.enabled) tri.length = len_tab[d >> 3];
        tri.lin_reload = 1;
        tri_cache();
        break;
    case 0x400C: nz.loop = (d >> 5) & 1; nz.const_vol = (d >> 4) & 1; nz.vol = d & 15; break;
    case 0x400E:
        nz.mode = d >> 7; nz.per = d & 15;
        nz.period = (float)noise_per[nz.per];
        nz.t = 0;                         /* new rate takes effect immediately */
        break;
    case 0x400F:
        if (nz.enabled) nz.length = len_tab[d >> 3];
        nz.env_start = 1;
        break;
    case 0x4015:
        pu[0].enabled = d & 1;        if (!pu[0].enabled) pu[0].length = 0;
        pu[1].enabled = (d >> 1) & 1; if (!pu[1].enabled) pu[1].length = 0;
        tri.enabled   = (d >> 2) & 1; if (!tri.enabled)   tri.length = 0;
        nz.enabled    = (d >> 3) & 1; if (!nz.enabled)    nz.length = 0;
        break;
    case 0x4017:                          /* frame counter: bit 7 = 5-step mode */
        fc_mode = d >> 7;
        fc_step = 0;
        break;
    default: break;
    }
}

uint8_t apu_read_status(void)
{
    return (uint8_t)((pu[0].length ? 1 : 0) | (pu[1].length ? 2 : 0) |
                     (tri.length ? 4 : 0) | (nz.length ? 8 : 0));
}

static int sweep_target(const Pulse *p)
{
    int d = p->timer >> p->sw_shift;
    return p->sw_neg ? p->timer - d - (p->ones_comp ? 1 : 0) : p->timer + d;
}

static void clock_env(uint8_t *start, uint8_t *div, uint8_t *decay, uint8_t period, uint8_t loop)
{
    if (*start) { *start = 0; *decay = 15; *div = period; }
    else if (*div == 0) { *div = period; if (*decay) (*decay)--; else if (loop) *decay = 15; }
    else (*div)--;
}

static void clock_quarter(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        clock_env(&pu[i].env_start, &pu[i].env_div, &pu[i].env_decay, pu[i].vol, pu[i].loop);
        pu[i].out_vol = pu[i].const_vol ? pu[i].vol : pu[i].env_decay;
    }
    clock_env(&nz.env_start, &nz.env_div, &nz.env_decay, nz.vol, nz.loop);
    if (tri.lin_reload) tri.lin = tri.lin_reload_val;
    else if (tri.lin)   tri.lin--;
    if (!tri.ctrl) tri.lin_reload = 0;
}

static void clock_half(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        Pulse *p = &pu[i];
        int target = sweep_target(p);
        if (p->length && !p->loop) p->length--;
        if (p->sw_div == 0 && p->sw_en && p->sw_shift &&
            p->timer >= 8 && target >= 0 && target <= 0x7FF)
            p->timer = (uint16_t)target;
        if (p->sw_div == 0 || p->sw_reload) { p->sw_div = p->sw_period; p->sw_reload = 0; }
        else p->sw_div--;
        pulse_cache(p);                   /* dx / gate follow the (possibly new) timer */
    }
    if (tri.length && !tri.ctrl) tri.length--;
    if (nz.length && !nz.loop)   nz.length--;
}

/* integral of a stepped periodic wave (n steps per period) from 0 to x, x in [0,1] */
static inline float wave_int(const float *cum, const float *val, float inv_n, int n, float x)
{
    float s = x * n;
    int k = (int)s;
    if (k >= n) return cum[n];
    return cum[k] + val[k] * (s - (float)k) * inv_n;
}

/* average value of the wave over [x0, x0+dx] (wrapping), i.e. a box filter */
static inline float wave_area(const float *cum, const float *val, float inv_n, int n,
                              float x0, float dx, float inv_dx)
{
    float x1 = x0 + dx, area;
    if (dx >= 1.0f) return cum[n];
    if (x1 <= 1.0f) area = wave_int(cum, val, inv_n, n, x1) - wave_int(cum, val, inv_n, n, x0);
    else            area = (cum[n] - wave_int(cum, val, inv_n, n, x0))
                         + wave_int(cum, val, inv_n, n, x1 - 1.0f);
    return area * inv_dx;
}

void apu_generate(int16_t *out, int n)
{
    const float cps     = CPS;
    const float inv_cps = 1.0f / CPS;
    int i;

    for (i = 0; i < n; i++) {
        float p_sum = 0.0f, t_out = 0.0f, n_out = 0.0f, s, y, v;
        int k;

        /* frame counter: 240 Hz quarter frames, 120 Hz half frames */
        fc_acc += 240.0f / NES_AUDIO_RATE;
        if (fc_acc >= 1.0f) {
            fc_acc -= 1.0f;
            if (fc_mode) { if (++fc_step >= 5) fc_step = 0; }
            else         { fc_step = (fc_step + 1) & 3; }
            clock_quarter();
            if (fc_mode ? (fc_step == 2 || fc_step == 4) : !(fc_step & 1))
                clock_half();
        }

        for (k = 0; k < 2; k++) {
            Pulse *p = &pu[k];
            if (p->enabled && p->length && p->gate_ok)
                p_sum += p->out_vol *
                         wave_area(duty_cum[p->duty], duty_val[p->duty],
                                   1.0f / 8.0f, 8, p->phase, p->dx, p->inv_dx);
            p->phase += p->dx;
            if (p->phase >= 1.0f) p->phase -= (float)(int)p->phase;
        }

        if (tri.enabled && tri.length && tri.lin && tri.timer >= 2)
            t_out = wave_area(tri_cum, tri_val, 1.0f / 32.0f, 32,
                              tri.phase, tri.dx, tri.inv_dx);
        tri.phase += tri.dx;
        if (tri.phase >= 1.0f) tri.phase -= (float)(int)tri.phase;

        {
            /* level is loop-invariant within a sample: it only changes on quarter-frame
             * clocks or register writes, never inside this loop */
            float lvl = (nz.enabled && nz.length) ? (float)(nz.const_vol ? nz.vol : nz.env_decay) : 0.0f;
            float rem = cps, acc = 0.0f, t = nz.t;
            const float per = nz.period;
            const int   tap = nz.mode ? 6 : 1;
            uint16_t lfsr = nz.lfsr;
            while (rem > 0.0f) {
                float take = per - t;
                if (take > rem) take = rem;
                if (lvl > 0.0f && !(lfsr & 1)) acc += lvl * take;
                t += take; rem -= take;
                if (t >= per) {
                    uint16_t fb = (uint16_t)((lfsr & 1) ^ ((lfsr >> tap) & 1));
                    lfsr = (uint16_t)((lfsr >> 1) | (fb << 14));
                    t -= per;
                }
            }
            nz.lfsr = lfsr; nz.t = t;
            n_out = acc * inv_cps;
        }

        /* NES mixer, linear approximation (accurate to ~1%) */
        s = 0.00752f * p_sum + 0.00851f * t_out + 0.00494f * n_out;

        y = HP_COEF * (hp_y + s - hp_x);
        hp_x = s; hp_y = y;

        v = y * APU_GAIN;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)v;
    }
}
