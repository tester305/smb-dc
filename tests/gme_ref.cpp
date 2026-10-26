// Reference APU renderer: feeds recorded SMB register writes to blargg's Nes_Apu (game-music-emu),
// a widely used, band-limited, cycle-based APU.  Output: raw mono int16 @ 44100 Hz.
//   gme_ref events.bin MODE NFRAMES out.raw      MODE: 0 full, 1 pulse+triangle, 2 noise, 3 pulses, 4 triangle
#include <cstdio>
#include <cstdlib>
#include <vector>
#define private public      // enable_nonlinear() is private (Nsf_Emu is a friend); this test needs it
#include "Nes_Apu.h"
#include "Blip_Buffer.h"
#undef private

struct Ev { int32_t frame, cyc, addr, data; };
static int dmc_read(void*, nes_addr_t) { return 0; }   // SMB never uses the DMC

int main(int argc, char** argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s events.bin mode nframes out.raw\n", argv[0]); return 1; }
    int mode = atoi(argv[2]), nframes = atoi(argv[3]);
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    int32_t n = 0; if (fread(&n, 4, 1, f) != 1) return 1;
    std::vector<Ev> evs(n); if (n && fread(evs.data(), sizeof(Ev), n, f) != (size_t)n) return 1; fclose(f);

    const int SR = 44100, SPF = 735, FRAME_CYCLES = 29829;   // 735 samples worth of CPU cycles
    Blip_Buffer buf; buf.clock_rate(1789773); if (buf.set_sample_rate(SR)) return 2;
    buf.bass_freq(40);                                         // DC blocker, like the NES' output stage
    Nes_Apu apu; apu.output(&buf); apu.dmc_reader(dmc_read, NULL);
    apu.reset(false); apu.volume(1.0); apu.enable_nonlinear(1.0);
    apu.treble_eq(blip_eq_t(0.0));                             // flat: Blip_Synth defaults to -8 dB treble

    FILE* out = fopen(argv[4], "wb"); if (!out) { perror(argv[4]); return 1; }
    std::vector<blip_sample_t> samples(SPF * 2);
    int ei = 0;
    for (int fr = 0; fr < nframes; fr++) {
        // production applies a frame's writes before generating that frame's samples -> same here
        for (; ei < n && evs[ei].frame == fr; ei++) {
            int a = evs[ei].addr, d = evs[ei].data;
            if (mode == 1) { if (a >= 0x400C && a <= 0x400F) continue; if (a == 0x4015) d &= ~0x18; }
            if (mode == 2) { if (a >= 0x4000 && a <= 0x400B) continue; if (a == 0x4015) d &= 0x08; }
            if (mode == 3) { if (a >= 0x4008 && a <= 0x400F) continue; if (a == 0x4015) d &= 0x03; }
            if (mode == 4) { if ((a >= 0x4000 && a <= 0x4007) || (a >= 0x400C && a <= 0x400F)) continue; if (a == 0x4015) d &= 0x04; }
            apu.write_register(0, a, d);
        }
        apu.end_frame(FRAME_CYCLES);
        buf.end_frame(FRAME_CYCLES);
        long got = buf.read_samples(samples.data(), SPF);
        for (long i = got; i < SPF; i++) samples[i] = 0;       // pad if a frame came up short
        fwrite(samples.data(), 2, SPF, out);
    }
    fclose(out);
    return 0;
}
