/* Small software APU: 2 pulse, triangle, noise (no DMC - Super Mario Bros. doesn't use it). */
#include "nes.h"
#include <string.h>

#define CPU_HZ 1789773.0f

typedef struct {
    uint8_t  enabled, duty, loop, const_vol, vol, length;
    uint8_t  env_start, env_div, env_decay;
    uint8_t  sw_en, sw_period, sw_neg, sw_shift, sw_reload, sw_div, ones_comp;
    uint16_t timer;
    float    phase;
} Pulse;

static Pulse   pu[2];
static struct { uint8_t enabled, ctrl, lin_reload_val, lin_reload, lin, length; uint16_t timer; float phase; } tri;
static struct { uint8_t enabled, loop, const_vol, vol, length, env_start, env_div, env_decay, mode, per; uint16_t lfsr; float acc; } nz;
static float   fc_acc;
static int     fc_step;
static float   pulse_tab[31], tnd_tab[203];

static const uint8_t len_tab[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t duty_tab[4][8] = {
    {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
static const uint16_t noise_per[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint8_t tri_seq[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

void apu_reset(void)
{
    int i;
    memset(pu, 0, sizeof pu);
    memset(&tri, 0, sizeof tri);
    memset(&nz, 0, sizeof nz);
    pu[0].ones_comp = 1;
    nz.lfsr = 1;
    fc_acc = 0; fc_step = 0;
    pulse_tab[0] = 0;
    for (i = 1; i < 31; i++)  pulse_tab[i] = 95.52f / (8128.0f / i + 100.0f);
    tnd_tab[0] = 0;
    for (i = 1; i < 203; i++) tnd_tab[i] = 163.67f / (24329.0f / i + 100.0f);
}

void apu_write(uint16_t a, uint8_t d)
{
    if (a >= 0x4000 && a <= 0x4007) {
        Pulse *p = &pu[(a >> 2) & 1];
        switch (a & 3) {
        case 0: p->duty = d >> 6; p->loop = (d >> 5) & 1; p->const_vol = (d >> 4) & 1; p->vol = d & 15; break;
        case 1: p->sw_en = d >> 7; p->sw_period = (d >> 4) & 7; p->sw_neg = (d >> 3) & 1; p->sw_shift = d & 7; p->sw_reload = 1; break;
        case 2: p->timer = (uint16_t)((p->timer & 0x700) | d); break;
        case 3:
            p->timer = (uint16_t)((p->timer & 0xFF) | ((d & 7) << 8));
            if (p->enabled) p->length = len_tab[d >> 3];
            p->env_start = 1; p->phase = 0;
            break;
        }
        return;
    }
    switch (a) {
    case 0x4008: tri.ctrl = d >> 7; tri.lin_reload_val = d & 0x7F; break;
    case 0x400A: tri.timer = (uint16_t)((tri.timer & 0x700) | d); break;
    case 0x400B:
        tri.timer = (uint16_t)((tri.timer & 0xFF) | ((d & 7) << 8));
        if (tri.enabled) tri.length = len_tab[d >> 3];
        tri.lin_reload = 1;
        break;
    case 0x400C: nz.loop = (d >> 5) & 1; nz.const_vol = (d >> 4) & 1; nz.vol = d & 15; break;
    case 0x400E: nz.mode = d >> 7; nz.per = d & 15; break;
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
    default: break;
    }
}

uint8_t apu_read_status(void)
{
    return (uint8_t)((pu[0].length ? 1 : 0) | (pu[1].length ? 2 : 0) | (tri.length ? 4 : 0) | (nz.length ? 8 : 0));
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
    for (i = 0; i < 2; i++) clock_env(&pu[i].env_start, &pu[i].env_div, &pu[i].env_decay, pu[i].vol, pu[i].loop);
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
        if (p->sw_div == 0 && p->sw_en && p->sw_shift && p->timer >= 8 && target <= 0x7FF) p->timer = (uint16_t)target;
        if (p->sw_div == 0 || p->sw_reload) { p->sw_div = p->sw_period; p->sw_reload = 0; }
        else p->sw_div--;
    }
    if (tri.length && !tri.ctrl) tri.length--;
    if (nz.length && !nz.loop)   nz.length--;
}

void apu_generate(int16_t *out, int n)
{
    const float cps = CPU_HZ / NES_AUDIO_RATE;       /* CPU cycles per output sample */
    int i;
    for (i = 0; i < n; i++) {
        int p_sum = 0, t_out = 0, n_out = 0, k;

        fc_acc += 240.0f / NES_AUDIO_RATE;
        if (fc_acc >= 1.0f) {
            fc_acc -= 1.0f;
            fc_step = (fc_step + 1) & 3;
            clock_quarter();
            if (!(fc_step & 1)) clock_half();
        }

        for (k = 0; k < 2; k++) {
            Pulse *p = &pu[k];
            p->phase += (CPU_HZ / (16.0f * (p->timer + 1))) / NES_AUDIO_RATE;
            if (p->phase >= 1.0f) p->phase -= (float)(int)p->phase;
            if (p->enabled && p->length && p->timer >= 8 &&
                !(!p->sw_neg && sweep_target(p) > 0x7FF) &&
                duty_tab[p->duty][(int)(p->phase * 8) & 7])
                p_sum += p->const_vol ? p->vol : p->env_decay;
        }

        tri.phase += (CPU_HZ / (32.0f * (tri.timer + 1))) / NES_AUDIO_RATE;
        if (tri.phase >= 1.0f) tri.phase -= (float)(int)tri.phase;
        if (tri.enabled && tri.length && tri.lin && tri.timer >= 2)
            t_out = tri_seq[(int)(tri.phase * 32) & 31];

        nz.acc += cps;
        while (nz.acc >= noise_per[nz.per]) {
            uint16_t fb;
            nz.acc -= noise_per[nz.per];
            fb = (uint16_t)((nz.lfsr & 1) ^ ((nz.lfsr >> (nz.mode ? 6 : 1)) & 1));
            nz.lfsr = (uint16_t)((nz.lfsr >> 1) | (fb << 14));
        }
        if (nz.enabled && nz.length && !(nz.lfsr & 1)) n_out = nz.const_vol ? nz.vol : nz.env_decay;

        {
            float s = pulse_tab[p_sum] + tnd_tab[3 * t_out + 2 * n_out];
            out[i] = (int16_t)(s * 26000.0f);
        }
    }
}
