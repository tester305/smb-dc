/*
 * Small software APU: 2 pulse, triangle, noise (no DMC - SMB only writes 0 to $4011 each frame).
 *
 * Accuracy
 *  - Exact NES mixer (the documented non-linear pulse/tnd curves, interpolated from tables)
 *    instead of the linear approximation, which was 25-70% too quiet and mis-balanced the channels.
 *  - Pulses are synthesised at twice the output rate with a triangular window (second-order integral
 *    of the wave) and decimated through a 39-tap half-band FIR (flat to 18 kHz, -40 dB at 26 kHz),
 *    which pushes aliasing to about -45 dB; the old box-filtered, 44.1 kHz version sat at -23 dB
 *    and sounded harsh.  Triangle and noise use exact box averaging.
 *  - The triangle holds its last value when its length/linear counter stops it (as hardware does).
 *  - Frame counter: SMB writes $FF to $4017 every frame; that resets the sequencer and, with bit 7
 *    set, immediately clocks a quarter+half frame, so SMB's APU sees 4 quarter / 2 half-frame clocks
 *    per video frame and note lengths, envelopes and the linear counter time out like a real NES.
 *  - $4003/$4007 restart the duty sequencer; the noise divider keeps its countdown when $400E changes.
 *  - A DC-blocking high-pass removes the unipolar offset (thumps on note start/stop).
 *
 * Speed
 *  - Audio is rendered in segments between frame-counter ticks (4 per video frame): inside a segment
 *    every parameter is constant, so each channel runs in its own tight loop with hoisted constants,
 *    one table evaluation per sample, and silent channels cost nothing.
 *  - Completely silent stretches (title screen, pauses) are filled with zeros.
 */
#include "nes.h"
#include <string.h>
#include <math.h>

#define CPU_HZ    1789773.0f
#define APU_GAIN  24000.0f      /* output loudness */
#define HP_COEF   0.996f        /* DC blocker, about 28 Hz corner */
#define CPS       (CPU_HZ / NES_AUDIO_RATE)
#define SEG_MAX   192           /* a frame-counter tick comes at least every 184 samples */
#define FIR_N     39            /* half-band decimator length (odd) */
#define FIR_C     19            /* centre tap index */
#define FIR_ODD   10            /* non-zero taps on each side: offsets 1,3,...,19 */

typedef struct {
    uint8_t  enabled, duty, loop, const_vol, vol, length;
    uint8_t  env_start, env_div, env_decay;
    uint8_t  sw_en, sw_period, sw_neg, sw_shift, sw_reload, sw_div, ones_comp;
    uint16_t timer;
    float    phase;
    float    dx, inv_dx2;           /* phase step per sample, 1/dx^2 */
    uint8_t  gate_ok;               /* timer >= 8 and sweep target in range */
    uint8_t  out_vol;               /* const_vol ? vol : env_decay */
} Pulse;

static Pulse pu[2];
static struct {
    uint8_t  enabled, ctrl, lin_reload_val, lin_reload, lin, length;
    uint16_t timer;
    float    phase, dx, inv_dx, hold;
} tri;
static struct {
    uint8_t  enabled, loop, const_vol, vol, length, env_start, env_div, env_decay, mode, per;
    uint16_t lfsr;
    float    period, next;          /* divider period, CPU cycles until the next LFSR clock */
} nz;
static int   fc_num, fc_step, fc_mode;
static float hp_x, hp_y;

/* pulse wave tables: A2 is the second integral of the (mean-removed) duty wave, piecewise quadratic */
static float p_mean[4], p_a2[4][8], p_b[4][8], p_c[4][8];
#define MIX_RES 8                               /* table entries per unit of channel level */
static float tri_val[32], tri_cum[33];
static float mix_pulse[30 * MIX_RES + 2], mix_tnd[75 * MIX_RES + 2];
static float fir_h[FIR_ODD], fir_hist[FIR_N - 1];   /* fir_h[i] = tap at offset 2*i+1; centre tap is 0.5 */
static int   fir_flush;                              /* input samples left before the filter is known to be empty */

static const uint8_t len_tab[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t duty_tab[4][8] = {
    {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
static const uint16_t noise_per[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint8_t tri_seq[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

static void pulse_cache(Pulse *p)
{
    p->dx      = CPS / (16.0f * (p->timer + 1));
    p->inv_dx2 = 1.0f / (p->dx * p->dx);
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
    int i, d, k;
    memset(pu, 0, sizeof pu);
    memset(&tri, 0, sizeof tri);
    memset(&nz, 0, sizeof nz);
    pu[0].ones_comp = 1;
    nz.lfsr = 1;
    nz.period = (float)noise_per[0];
    nz.next = nz.period;
    fc_num = 0; fc_step = 0; fc_mode = 0; hp_x = hp_y = 0;

    for (d = 0; d < 4; d++) {
        float cum[9], a1[9], c1 = 0.0f, m = 0.0f, a2 = 0.0f;
        cum[0] = 0;
        for (k = 0; k < 8; k++) cum[k + 1] = cum[k] + duty_tab[d][k] / 8.0f;
        m = cum[8];
        for (k = 0; k <= 8; k++) a1[k] = cum[k] - m * k / 8.0f;          /* A1 = integral(w) - m*x */
        for (k = 0; k < 8; k++) c1 += 0.5f * (a1[k] + a1[k + 1]) / 8.0f;  /* mean of A1 */
        p_mean[d] = m;
        for (k = 0; k < 8; k++) {
            p_a2[d][k] = a2;
            p_b[d][k]  = a1[k] - c1;
            p_c[d][k]  = 0.5f * (duty_tab[d][k] - m);
            a2 += (0.5f * (a1[k] + a1[k + 1]) - c1) / 8.0f;
        }
    }
    tri_cum[0] = 0;
    for (i = 0; i < 32; i++) {
        tri_val[i] = tri_seq[i];
        tri_cum[i + 1] = tri_cum[i] + tri_val[i] / 32.0f;
    }
    /* exact NES mixer curves (nesdev "APU Mixer"): pulse in 0..30, triangle/noise/DMC mix in 0..76 */
    for (i = 0; i < 30 * MIX_RES + 2; i++) {
        float x = (float)i / MIX_RES;
        mix_pulse[i] = x > 0.0f ? 95.52f / (8128.0f / x + 100.0f) : 0.0f;
    }
    for (i = 0; i < 75 * MIX_RES + 2; i++) {
        float x = (float)i / MIX_RES;
        mix_tnd[i] = x > 0.0f ? 163.67f / (24329.0f / x + 100.0f) : 0.0f;
    }

    {   /* half-band low-pass: sinc(k/2)/2 * Kaiser(beta 6.5), normalised to unity gain at DC */
        float sum = 0.5f;
        for (i = 0; i < FIR_ODD; i++) {
            int   o = 2 * i + 1;
            float r = (float)o / (float)FIR_C, term = 1.0f, bes = 1.0f, x2 = 6.5f * 6.5f * (1.0f - r * r) * 0.25f, w;
            for (k = 1; k < 24; k++) { term *= x2 / ((float)k * (float)k); bes += term; }
            {   float b0 = 1.0f, t0 = 1.0f, x0 = 6.5f * 6.5f * 0.25f;
                for (k = 1; k < 24; k++) { t0 *= x0 / ((float)k * (float)k); b0 += t0; }
                w = bes / b0;
            }
            fir_h[i] = (float)(sin(3.14159265358979 * o / 2.0) / (3.14159265358979 * o)) * w;
            sum += 2.0f * fir_h[i];
        }
        for (i = 0; i < FIR_ODD; i++) fir_h[i] /= sum;
    }
    memset(fir_hist, 0, sizeof fir_hist);
    fir_flush = 0;

    pulse_cache(&pu[0]);
    pulse_cache(&pu[1]);
    tri_cache();
}

static void clock_quarter(void);
static void clock_half(void);

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
            pulse_cache(p);
            break;
        case 2:
            p->timer = (uint16_t)((p->timer & 0x700) | d);
            pulse_cache(p);
            break;
        case 3:
            p->timer = (uint16_t)((p->timer & 0xFF) | ((d & 7) << 8));
            if (p->enabled) p->length = len_tab[d >> 3];
            p->env_start = 1;
            p->phase = 0;                 /* duty sequencer restarts (envelope too) */
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
        nz.period = (float)noise_per[nz.per];   /* current countdown is left alone */
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
    case 0x4017:
        fc_mode = d >> 7;
        fc_step = 0;
        fc_num = 0;
        if (fc_mode) { clock_quarter(); clock_half(); }   /* immediate clock, bit 7 set */
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
        pulse_cache(p);
    }
    if (tri.length && !tri.ctrl) tri.length--;
    if (nz.length && !nz.loop)   nz.length--;
}

/* ------------------------------------------------------------------ per-segment channel rendering */

static inline float pulse_a2(int d, float x)       /* x in [0,1) */
{
    float s = x * 8.0f;
    int   k = (int)s;
    float u = (s - (float)k) * 0.125f;
    return p_a2[d][k] + u * (p_b[d][k] + u * p_c[d][k]);
}

/* adds the pulse's level (0..15) to dst[0..n) at TWICE the output rate; returns 1 if it was audible */
static int render_pulse(Pulse *p, float *dst, int n)
{
    const float dx = 0.5f * p->dx;                           /* phase step per 88.2 kHz sample */
    float x, xm, gprev, gcur;
    int i;
    if (!(p->enabled && p->length && p->gate_ok && p->out_vol)) {
        p->phase += n * dx;
        p->phase -= (float)(int)p->phase;
        return 0;
    }
    {
        const int   d    = p->duty;
        const float mean = p_mean[d], inv2 = 4.0f * p->inv_dx2, vol = p->out_vol;
        x  = p->phase + 0.5f * dx;  if (x >= 1.0f)  x  -= 1.0f;        /* window centre */
        xm = x - dx;                if (xm < 0.0f)  xm += 1.0f;
        gprev = pulse_a2(d, xm);
        gcur  = pulse_a2(d, x);
        for (i = 0; i < n; i++) {
            float xn = x + dx, gn;
            if (xn >= 1.0f) xn -= 1.0f;
            gn = pulse_a2(d, xn);
            dst[i] += vol * (mean + (gn - 2.0f * gcur + gprev) * inv2);
            gprev = gcur; gcur = gn; x = xn;
        }
    }
    p->phase += n * dx;
    p->phase -= (float)(int)p->phase;
    return 1;
}

static inline float tri_int(float x)
{
    float s = x * 32.0f;
    int   k = (int)s;
    return tri_cum[k] + tri_val[k] * (s - (float)k) * (1.0f / 32.0f);
}

/* writes the triangle level (0..15) to dst[0..n); returns 1 if it is not constant zero */
static int render_tri(float *dst, int n)
{
    int i;
    if (!(tri.enabled && tri.length && tri.lin)) {          /* halted: hold the last level */
        for (i = 0; i < n; i++) dst[i] = tri.hold;
        return tri.hold != 0.0f;
    }
    if (tri.timer < 2) {                                      /* ultrasonic: averages to the mid level */
        for (i = 0; i < n; i++) dst[i] = 7.5f;
        tri.hold = 7.5f;
        return 1;
    }
    {
        const float dx = tri.dx, inv = tri.inv_dx;
        float x = tri.phase, fprev = tri_int(x), turns = 0.0f;
        for (i = 0; i < n; i++) {
            float xn = x + dx, fnow;
            if (xn >= 1.0f) { xn -= 1.0f; turns += 7.5f; }     /* 7.5 = integral of the wave over a period */
            fnow = tri_int(xn) + turns;
            dst[i] = (fnow - fprev) * inv;
            fprev = fnow; x = xn;
        }
        tri.phase = x;
        tri.hold = tri_val[(int)(x * 32.0f) & 31];
    }
    return 1;
}

static int render_noise(float *dst, int n)
{
    const float lvl = (nz.enabled && nz.length) ? (float)(nz.const_vol ? nz.vol : nz.env_decay) : 0.0f;
    const float per = nz.period;
    float next = nz.next;
    int i;
    if (lvl == 0.0f) {                                        /* silent: skip LFSR work, keep the divider timing */
        for (i = 0; i < n; i++) dst[i] = 0.0f;
        next -= n * CPS;
        while (next <= 0.0f) next += per;
        nz.next = next;
        return 0;
    }
    {
        const float inv_cps = lvl * (1.0f / CPS);
        const int   tap = nz.mode ? 6 : 1;
        uint16_t lfsr = nz.lfsr;
        for (i = 0; i < n; i++) {
            float rem = CPS, acc = 0.0f;
            while (rem > 0.0f) {
                float take = next < rem ? next : rem;
                if (!(lfsr & 1)) acc += take;
                rem -= take; next -= take;
                if (next <= 0.0f) {
                    uint16_t fb = (uint16_t)((lfsr & 1) ^ ((lfsr >> tap) & 1));
                    lfsr = (uint16_t)((lfsr >> 1) | (fb << 14));
                    next += per;
                }
            }
            dst[i] = acc * inv_cps;
        }
        nz.lfsr = lfsr; nz.next = next;
    }
    return 1;
}

/* Mixer curve lookup.  The filtered signal can overshoot slightly below 0 or above the maximum (filter
   ringing at note edges), so continue the curve's end slopes instead of reading outside the table;
   a fine table keeps the interpolation kinks - which would otherwise alias - negligible. */
static inline float mix_lookup(const float *tab, float x, int maxi)
{
    float s = x * (float)MIX_RES, f;
    int   i;
    if (s < 0.0f) return s * (tab[1] - tab[0]);
    i = (int)s;
    if (i >= maxi) return tab[maxi] + (s - (float)maxi) * (tab[maxi] - tab[maxi - 1]);
    f = s - (float)i;
    return tab[i] + (tab[i + 1] - tab[i]) * f;
}

/* both pulses at 2x rate into one buffer, then low-pass + decimate to the output rate */
static int render_pulses(float *sp, int n)
{
    static float buf[(FIR_N - 1) + 2 * SEG_MAX];
    float *in = buf + (FIR_N - 1);
    int i, j, on;

    for (i = 0; i < 2 * n; i++) in[i] = 0.0f;
    on  = render_pulse(&pu[0], in, 2 * n);
    on |= render_pulse(&pu[1], in, 2 * n);
    if (on) fir_flush = FIR_N;
    if (fir_flush <= 0) {
        for (i = 0; i < n; i++) sp[i] = 0.0f;
        return 0;
    }
    memcpy(buf, fir_hist, sizeof fir_hist);
    for (j = 0; j < n; j++) {
        const float *c = buf + 2 * j + FIR_C;
        float y = 0.5f * c[0];
        for (i = 0; i < FIR_ODD; i++) y += fir_h[i] * (c[-(2 * i + 1)] + c[2 * i + 1]);
        sp[j] = y;
    }
    memcpy(fir_hist, buf + 2 * n, sizeof fir_hist);
    if (!on) {
        fir_flush -= 2 * n;
        if (fir_flush <= 0) { fir_flush = 0; memset(fir_hist, 0, sizeof fir_hist); }
    }
    return 1;
}

static void render_segment(int16_t *out, int n)
{
    static float sp[SEG_MAX], st[SEG_MAX], sn[SEG_MAX];
    int on, k;

    on  = render_pulses(sp, n);
    on |= render_tri(st, n);
    on |= render_noise(sn, n);

    if (!on && hp_y > -0.0005f && hp_y < 0.0005f && hp_x == 0.0f) {   /* silence */
        memset(out, 0, (size_t)n * sizeof(int16_t));
        return;
    }
    for (k = 0; k < n; k++) {
        float s = mix_lookup(mix_pulse, sp[k], 30 * MIX_RES) + mix_lookup(mix_tnd, 3.0f * st[k] + 2.0f * sn[k], 75 * MIX_RES);
        float y = HP_COEF * (hp_y + s - hp_x), v;
        hp_x = s; hp_y = y;
        v = y * APU_GAIN;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[k] = (int16_t)v;
    }
}

void apu_generate(int16_t *out, int n)
{
    while (n > 0) {
        int m = (NES_AUDIO_RATE - fc_num + 239) / 240;     /* samples until the next frame-counter tick */
        int seg = n < m ? n : m;
        if (seg > SEG_MAX) seg = SEG_MAX;
        render_segment(out, seg);
        out += seg; n -= seg;
        fc_num += 240 * seg;
        if (fc_num >= NES_AUDIO_RATE) {
            fc_num -= NES_AUDIO_RATE;
            if (fc_mode) {
                if (++fc_step >= 5) fc_step = 0;
                if (fc_step != 4) {
                    clock_quarter();
                    if (fc_step == 2 || fc_step == 0)   /* half frames at steps 2 and 5 */
                        clock_half();
                }
            } else {
                fc_step = (fc_step + 1) & 3;
                clock_quarter();
                if (!(fc_step & 1))
                    clock_half();
            }
        }
    }
}
