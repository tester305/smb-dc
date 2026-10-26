/*
 * APU accuracy test (host only).
 *
 *   Part 1  spec tests   - pitch, length-counter timing, envelope timing, sweep muting, mixer, DC
 *                          blocker, each against the documented hardware value.
 *   Part 2  game capture - runs Super Mario Bros., records every APU register write the game makes
 *                          (with CPU-cycle timestamps), and renders that traffic through the APU
 *                          under test exactly as the Dreamcast build does (writes, then 735 samples
 *                          per frame).  Files are written to the output directory:
 *                              events.bin          the recorded register writes
 *                              prod_{full,tonal,noise,pulse,tri}.raw   this APU's output (mono int16, 44.1 kHz)
 *                          tests/gme_ref renders the same events through blargg's Nes_Apu and
 *                          tests/compare.py scores the two.  Use tests/run_apu_accuracy.sh to do it all.
 *
 *   ./apu_test ROM.nes [frames] [outdir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include "nes.h"

#ifndef APU_SRC
#define APU_SRC "../src/apu.c"
#endif
#define apu_write apu_write_prod       /* production APU under test; apu_write below is a logger */
#include APU_SRC
#undef apu_write

#define SR      44100
#define SPF     735                    /* samples per video frame, as in the Dreamcast build */
#define CPUHZ   1789773.0
#define CPSD    (CPUHZ / SR)
#ifdef APU_GAIN
#define GAINC   APU_GAIN
#else
#define GAINC   30000.0
#endif
#define HPC     0.996

/* ---------------------------------------------------------------- register-write logger */
typedef struct { int frame; int cyc; uint16_t addr; uint8_t data; } Ev;
static Ev *evs; static int nev, evcap;
static int g_frame, g_logging;
static uint64_t g_cycles, g_frame_start;

void apu_write(uint16_t a, uint8_t d)
{
    if (g_logging) {
        if (nev == evcap) { evcap = evcap ? evcap * 2 : 4096; evs = realloc(evs, evcap * sizeof(Ev)); }
        evs[nev].frame = g_frame; evs[nev].cyc = (int)(g_cycles - g_frame_start);
        evs[nev].addr = a; evs[nev].data = d; nev++;
    }
    apu_write_prod(a, d);
}

/* same loop as nes_run_frame() in src/nes.c, plus CPU-cycle bookkeeping */
static void run_frame_logged(uint16_t *fb)
{
    int sl, dots = 0;
    g_frame_start = g_cycles;
    for (sl = 0; sl < 262; sl++) {
        if (sl == 241) ppu_vblank_start(); else if (sl == 261) ppu_prerender_start();
        if (sl < 240) ppu_render_scanline(sl, fb + sl * NES_SCREEN_W);
        dots += 341;
        while (dots > 0) { int c = cpu_step(); g_cycles += c; dots -= c * 3; }
        if (sl < 240) ppu_scanline_end(); else if (sl == 261) ppu_prerender_end();
    }
}

/* ---------------------------------------------------------------- spec tests */
/* NESdev length-counter table, kept independent of the APU source under test */
static const int R_LEN[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static int fails, passes;
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s  ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap); printf("\n"); va_end(ap);
    if (ok) passes++; else fails++;
}
static int16_t sbuf[SR * 4];
static void w(uint16_t a, uint8_t d) { apu_write_prod(a, d); }
static void gen(int16_t *dst, int n) { while (n > 0) { int c = n > SPF ? SPF : n; apu_generate(dst, c); dst += c; n -= c; } }

static double measure_freq(const int16_t *x, int n)
{
    int i, peak = 0, thr, state = 0, cnt = 0, first = -1, last = -1;
    for (i = 0; i < n; i++) if (abs(x[i]) > peak) peak = abs(x[i]);
    if (peak < 50) return 0;
    thr = peak / 8;
    for (i = 0; i < n; i++) {
        int s = x[i] > thr ? 1 : (x[i] < -thr ? -1 : state);
        if (state == -1 && s == 1) { cnt++; if (first < 0) first = i; last = i; }
        if (s != 0) state = s;
    }
    return cnt < 3 ? 0 : (double)(cnt - 1) * SR / (last - first);
}
static double rms(const int16_t *x, int n) { double s = 0; int i; for (i = 0; i < n; i++) s += (double)x[i] * x[i]; return sqrt(s / n); }

static void test_pitch(void)
{
    static const int pt[] = { 0x3F, 0x7E, 0xFD, 0x1FA, 0x3F4 };
    static const int tt[] = { 0x7E, 0x1F6 };
    int i;
    for (i = 0; i < 5; i++) {
        double exp_hz = CPUHZ / (16.0 * (pt[i] + 1)), got;
        apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, pt[i] & 255); w(0x4003, 8 | (pt[i] >> 8));
        gen(sbuf, SR); got = measure_freq(sbuf + 4410, SR - 4410);
        check(fabs(got / exp_hz - 1) < 0.003, "pulse pitch   timer %4d: %8.2f Hz (expected %8.2f, error %+.3f%%)", pt[i], got, exp_hz, (got / exp_hz - 1) * 100);
    }
    for (i = 0; i < 2; i++) {
        double exp_hz = CPUHZ / (32.0 * (tt[i] + 1)), got;
        apu_reset(); w(0x4015, 4); w(0x4008, 0xFF); w(0x400A, tt[i] & 255); w(0x400B, 8 | (tt[i] >> 8));
        gen(sbuf, SR); got = measure_freq(sbuf + 4410, SR - 4410);
        check(fabs(got / exp_hz - 1) < 0.003, "triangle pitch timer %4d: %8.2f Hz (expected %8.2f, error %+.3f%%)", tt[i], got, exp_hz, (got / exp_hz - 1) * 100);
    }
}

static double length_expiry(int idx, int smb_style)
{
    int n; int16_t tmp;
    apu_reset(); w(0x4015, 1); w(0x4000, 0x9F); w(0x4001, 0x08); w(0x4002, 0xFD);
    if (smb_style) w(0x4017, 0xFF);
    w(0x4003, (uint8_t)(idx << 3));
    for (n = 1; n < SR * 4; n++) {
        if (smb_style && n % SPF == 0) w(0x4017, 0xFF);        /* SMB rewrites $4017 every frame */
        apu_generate(&tmp, 1);
        if (!(apu_read_status() & 1)) return n / (double)SR;
    }
    return -1;
}
static void test_length(void)
{
    static const int idxs[] = { 3, 5, 0, 2, 4, 1 };            /* length values 2, 4, 10, 20, 40, 254 */
    int style, i;
    for (style = 0; style < 2; style++)
        for (i = 0; i < 6; i++) {
            int len = R_LEN[idxs[i]];
            double got = length_expiry(idxs[i], style), lo = (len - 1) / 120.0 - 0.001, hi = len / 120.0 + 0.001;
            check(got >= lo && got <= hi, "length counter %3d %-18s: expires %7.1f ms (hardware window %.1f - %.1f ms)",
                  len, style ? "(SMB $4017 writes)" : "(free running)", got * 1000, lo * 1000 + 1, hi * 1000 - 1);
        }
}

static void test_envelope(void)
{
    static const int periods[] = { 0, 3 };
    int k;
    for (k = 0; k < 2; k++) {
        int p = periods[k], win, mx = 0, started = 0; double t_sil = -1;
        apu_reset(); w(0x4015, 1); w(0x4000, (uint8_t)(0x80 | p)); w(0x4001, 0x08); w(0x4002, 0xFD); w(0x4003, 8);
        for (win = 0; win < 3 * SR / 64; win++) {
            int16_t b[64], lo = 32767, hi = -32768, i, p2p;
            apu_generate(b, 64);
            for (i = 0; i < 64; i++) { if (b[i] < lo) lo = b[i]; if (b[i] > hi) hi = b[i]; }
            p2p = hi - lo; if (p2p > mx) { mx = p2p; started = 1; }
            if (started && p2p < mx * 0.03) { t_sil = win * 64.0 / SR; break; }
        }
        {
            double exp_s = (1 + 15 * (p + 1)) / 240.0;
            check(t_sil > 0 && fabs(t_sil - exp_s) < 3 / 240.0, "envelope decay period %d: silent after %6.1f ms (hardware %.1f ms)", p, t_sil * 1000, exp_s * 1000);
        }
    }
}

static void test_sweep_mute(void)
{
    double r;
    apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, 5); w(0x4003, 8);
    gen(sbuf, 8820); r = rms(sbuf + 882, 7938);
    check(r < 5, "pulse period < 8 is silent                    (rms %.1f)", r);
    apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x00); w(0x4002, 0x00); w(0x4003, 8 | 5);
    gen(sbuf, 8820); r = rms(sbuf + 882, 7938);
    check(r < 5, "sweep target overflow (>$7FF) mutes pulse     (rms %.1f)", r);
    apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, 0x00); w(0x4003, 8 | 5);
    gen(sbuf, 8820); r = rms(sbuf + 882, 7938);
    check(r > 100, "negate mode does not mute at the same period (rms %.1f)", r);
}

static void test_mixer_and_dc(void)
{
    {
        double m; int i;
        apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, 0xFD); w(0x4003, 8);
        gen(sbuf, 2 * SR); for (m = 0, i = SR; i < 2 * SR; i++) m += sbuf[i];
        check(fabs(m / SR) < 20, "DC blocker: mean of a steady note = %.1f LSB (limit 20)", m / SR);
    }
}



/* ---- aliasing: energy below 18 kHz that is not on a harmonic of a steady pulse, relative to the harmonics ----
   (energy between 18 kHz and Nyquist is ignored: harmonics just above Nyquist fold to 20-22 kHz where nobody can hear them) */
static void fft(double *re, double *im, int n)
{
    int i, j = 0, k, len;
    for (i = 1; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len, wr = cos(ang), wi = sin(ang);
        for (i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (k = 0; k < len / 2; k++) {
                double ur = re[i + k], ui = im[i + k], vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci, vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                { double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t; }
            }
        }
    }
}
static double alias_db(int timer)
{
    enum { N = 32768 }; static double re[N], im[N]; double f0 = CPUHZ / (16.0 * (timer + 1)), in = 0, out = 0; int i, k;
    apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, timer & 255); w(0x4003, 8 | (timer >> 8));
    gen(sbuf, 4410 + N);
    for (i = 0; i < N; i++) { re[i] = sbuf[4410 + i] * (0.5 - 0.5 * cos(2 * M_PI * i / N)); im[i] = 0; }
    fft(re, im, N);
    for (k = 3; k < (int)(18000.0 * N / SR); k++) {         /* audible band only (< 18 kHz) */
        double hz = (double)k * SR / N, h = hz / f0, nearest = floor(h + 0.5), p = re[k] * re[k] + im[k] * im[k];
        int onh = fabs(h - nearest) * f0 * N / SR < 4.0 && nearest >= 1;     /* within 4 bins of a harmonic */
        if (onh) in += p; else out += p;
    }
    return 10 * log10(out / in + 1e-30);
}
static void test_alias(void)
{
    static const int ts[] = { 63, 40, 27, 17 };
    int i;
    for (i = 0; i < 4; i++) {
        double f0 = CPUHZ / (16.0 * (ts[i] + 1)), a = alias_db(ts[i]);
        check(a < -40, "aliasing, %.0f Hz pulse: non-harmonic energy below 18 kHz is %.1f dB under the harmonics (limit -40 dB)", f0, -a);
    }
}

static void test_levels(void)
{
    /* exact NES mixer: pulse = 95.52/(8128/n+100), tnd = 163.67/(24329/k+100); output = x30000, AC part */
    double pe = 0.5 * 95.52 / (8128.0 / 15 + 100) * GAINC, ts = 0, tm = 0, te, ne = 0.5 * 163.67 / (24329.0 / 30 + 100) * GAINC, r;
    int i;
    for (i = 0; i < 32; i++) { int v = i < 16 ? 15 - i : i - 16; double y = 163.67 / (24329.0 / (3 * (v ? v : 1)) + 100) * (v ? 1 : 0); ts += y; tm += y * y; }
    te = sqrt(tm / 32 - (ts / 32) * (ts / 32)) * GAINC;
    apu_reset(); w(0x4015, 1); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, 0xFD); w(0x4003, 8);
    gen(sbuf, SR); r = rms(sbuf + 4410, SR - 4410);
    check(fabs(r / pe - 1) < 0.03, "pulse level (vol 15, 50%% duty): rms %.0f, exact mixer %.0f (%+.1f%%)", r, pe, (r / pe - 1) * 100);
    apu_reset(); w(0x4015, 4); w(0x4008, 0xFF); w(0x400A, 0x7E); w(0x400B, 8);
    gen(sbuf, SR); r = rms(sbuf + 4410, SR - 4410);
    check(fabs(r / te - 1) < 0.03, "triangle level (full):          rms %.0f, exact mixer %.0f (%+.1f%%)", r, te, (r / te - 1) * 100);
    apu_reset(); w(0x4015, 8); w(0x400C, 0x3F); w(0x400E, 0x0C); w(0x400F, 8);
    gen(sbuf, SR); r = rms(sbuf + 4410, SR - 4410);
    check(fabs(r / ne - 1) < 0.08, "noise level (vol 15):           rms %.0f, exact mixer %.0f (%+.1f%%)", r, ne, (r / ne - 1) * 100);
    {   /* two pulses + triangle together: the mixer is non-linear, so levels do not simply add */
        double a, b;
        apu_reset(); w(0x4015, 7); w(0x4000, 0xBF); w(0x4001, 0x08); w(0x4002, 0xFD); w(0x4003, 8);
        w(0x4004, 0xBF); w(0x4005, 0x08); w(0x4006, 0x7E); w(0x4007, 8); w(0x4008, 0xFF); w(0x400A, 0xFD); w(0x400B, 8);
        gen(sbuf, SR); r = rms(sbuf + 4410, SR - 4410);
        /* exact value from a brute-force period average of the non-linear formula over one 1 s window */
        { double sum = 0, sum2 = 0; int n = 0; long c;
          for (c = 0; c < 1789773L; c += 7) {
              int p0 = (int)((c / (16 * 254)) % 1 * 0) + (((c / 254) & 7) >= 1 && ((c / 254) & 7) <= 2 ? 15 : 0);   /* duty 50%: steps 1-4 high */
              int sq0 = (((c / (2 * 254)) % 8) >= 1 && ((c / (2 * 254)) % 8) <= 4) ? 15 : 0;
              int sq1 = (((c / (2 * 127)) % 8) >= 1 && ((c / (2 * 127)) % 8) <= 4) ? 15 : 0;
              int st = (int)((c / 254) % 32), tv = st < 16 ? 15 - st : st - 16;
              double y = ((sq0 + sq1) ? 95.52 / (8128.0 / (sq0 + sq1) + 100) : 0) + (tv ? 163.67 / (24329.0 / (3 * tv) + 100) : 0);
              (void)p0; sum += y; sum2 += y * y; n++;
          }
          b = sqrt(sum2 / n - (sum / n) * (sum / n)) * GAINC; a = r; }
        check(fabs(a / b - 1) < 0.05, "pulse+pulse+triangle together:   rms %.0f, exact mixer %.0f (%+.1f%%)", a, b, (a / b - 1) * 100);
    }
}


#include <time.h>
static void bench_speed(int nframes)
{
    int rep, f, ei; double best = 1e9; int16_t o[SPF];
    for (rep = 0; rep < 5; rep++) {
        struct timespec a, b; double t = 0;
        apu_reset(); ei = 0;
        for (f = 0; f < nframes; f++) {
            while (ei < nev && evs[ei].frame == f) { apu_write_prod(evs[ei].addr, evs[ei].data); ei++; }
            clock_gettime(CLOCK_MONOTONIC, &a); apu_generate(o, SPF); clock_gettime(CLOCK_MONOTONIC, &b);
            t += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
        }
        if (t < best) best = t;
    }
    printf("  speed: %.1f ms to render %d frames (%.2f us per frame, %.0f ns per sample) - host CPU, best of 5\n",
           best * 1000, nframes, best * 1e6 / nframes, best * 1e9 / ((double)nframes * SPF));
}

/* ---------------------------------------------------------------- game capture */
static int filt(int mode, uint16_t a, uint8_t d, uint8_t *dout)     /* 0 full, 1 pulse+triangle, 2 noise, 3 pulses, 4 triangle; returns 0 = drop */
{
    *dout = d;
    if (mode == 1) { if (a >= 0x400C && a <= 0x400F) return 0; if (a == 0x4015) *dout = d & ~0x18; }
    if (mode == 2) { if (a >= 0x4000 && a <= 0x400B) return 0; if (a == 0x4015) *dout = d & 0x08; }
    if (mode == 3) { if (a >= 0x4008 && a <= 0x400F) return 0; if (a == 0x4015) *dout = d & 0x03; }
    if (mode == 4) { if ((a >= 0x4000 && a <= 0x4007) || (a >= 0x400C && a <= 0x400F)) return 0; if (a == 0x4015) *dout = d & 0x04; }
    return 1;
}
static void dump_prod(const char *path, int mode, int nframes)
{
    FILE *fp = fopen(path, "wb"); int16_t o[SPF]; int f, ei = 0;
    if (!fp) { perror(path); exit(2); }
    apu_reset();
    for (f = 0; f < nframes; f++) {
        while (ei < nev && evs[ei].frame == f) { uint8_t d; if (filt(mode, evs[ei].addr, evs[ei].data, &d)) apu_write_prod(evs[ei].addr, d); ei++; }
        apu_generate(o, SPF);
        fwrite(o, 2, SPF, fp);
    }
    fclose(fp);
}
static void print_stats(void)
{
    int i, d0[4] = {0}, d1[4] = {0}, sw0 = 0, sw1 = 0, dmc = 0, nm[2] = {0}, np[16] = {0}, n4017 = 0, v4017 = -1;
    int tri_hold = 0;
    for (i = 0; i < nev; i++) {
        int a = evs[i].addr, d = evs[i].data;
        if (a == 0x4000) d0[d >> 6]++;
        if (a == 0x4004) d1[d >> 6]++;
        if (a == 0x4001 && (d & 0x80)) sw0++;
        if (a == 0x4005 && (d & 0x80)) sw1++;
        if (a >= 0x4010 && a <= 0x4013) dmc++;
        if (a == 0x400E) { nm[d >> 7]++; np[d & 15]++; }
        if (a == 0x4017) { n4017++; v4017 = d; }
        if (a == 0x4008 && (d & 0x80)) tri_hold++;
    }
    printf("  SMB register usage: pulse1 duty writes %d/%d/%d/%d, pulse2 %d/%d/%d/%d; sweep enabled %d+%d; DMC writes %d\n",
           d0[0], d0[1], d0[2], d0[3], d1[0], d1[1], d1[2], d1[3], sw0, sw1, dmc);
    printf("  noise mode0/mode1 writes %d/%d; periods used:", nm[0], nm[1]);
    for (i = 0; i < 16; i++) if (np[i]) printf(" [%d]x%d", i, np[i]);
    printf("\n  $4017 writes %d (last value $%02X), triangle control-halt writes %d\n", n4017, v4017 & 255, tri_hold);
}

int main(int argc, char **argv)
{
    static uint16_t fb[NES_SCREEN_W * NES_SCREEN_H];
    int nframes = argc > 2 ? atoi(argv[2]) : 1800, f;
    const char *outdir = argc > 3 ? argv[3] : ".";
    printf("APU under test: %s\n\n== Part 1: spec tests ==\n", APU_SRC);
    test_pitch(); test_length(); test_envelope(); test_sweep_mute(); test_mixer_and_dc(); test_levels(); test_alias();
    printf("\n  %d passed, %d failed\n", passes, fails);

    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb"); uint8_t *rom; long sz; char path[512];
        if (!fp) { perror(argv[1]); return 2; }
        fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        rom = malloc(sz); if (fread(rom, 1, sz, fp) != (size_t)sz) return 2; fclose(fp);
        if (nes_load(rom, sz)) { fprintf(stderr, "bad ROM\n"); return 2; }
        nes_reset();
        g_logging = 1;
        for (f = 0; f < nframes; f++) {
            uint8_t pad = 0;
            if (f >= 120 && f < 126) pad |= PAD_START;                     /* press Start on the title screen */
            if (f >= 420) { pad |= PAD_RIGHT | PAD_B; if ((f / 20) % 3 == 0) pad |= PAD_A; }
            g_frame = f; nes_set_pad(pad); run_frame_logged(fb);
        }
        g_logging = 0;
        printf("\n== Part 2: game capture ==\n  %d frames, %d APU register writes\n", nframes, nev);
        print_stats();
        snprintf(path, sizeof path, "%s/events.bin", outdir);
        fp = fopen(path, "wb"); if (!fp) { perror(path); return 2; }
        { int32_t n32 = nev, i; fwrite(&n32, 4, 1, fp);
          for (i = 0; i < nev; i++) { int32_t r[4] = { evs[i].frame, evs[i].cyc, evs[i].addr, evs[i].data }; fwrite(r, 4, 4, fp); } }
        fclose(fp);
        snprintf(path, sizeof path, "%s/prod_full.raw",  outdir); dump_prod(path, 0, nframes);
        snprintf(path, sizeof path, "%s/prod_tonal.raw", outdir); dump_prod(path, 1, nframes);
        snprintf(path, sizeof path, "%s/prod_noise.raw", outdir); dump_prod(path, 2, nframes);
        snprintf(path, sizeof path, "%s/prod_pulse.raw", outdir); dump_prod(path, 3, nframes);
        snprintf(path, sizeof path, "%s/prod_tri.raw",   outdir); dump_prod(path, 4, nframes);
        bench_speed(nframes);
    }
    return fails ? 1 : 0;
}
