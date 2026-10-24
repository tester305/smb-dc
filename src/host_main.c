/* Headless test harness: runs the ROM, scripts the controller, dumps PPM frames + a WAV. */
#include "nes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t fb[NES_SCREEN_W * NES_SCREEN_H];

static void save_ppm(const char *name)
{
    FILE *f = fopen(name, "wb");
    int i;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", NES_SCREEN_W, NES_SCREEN_H);
    for (i = 0; i < NES_SCREEN_W * NES_SCREEN_H; i++) {
        uint16_t c = fb[i];
        uint8_t px[3] = { (uint8_t)(((c >> 11) & 31) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void le32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void le16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *rom;
    long sz;
    int frame, total = argc > 2 ? atoi(argv[2]) : 1800;
    int16_t *audio;
    int spf = NES_AUDIO_RATE / 60, peak = 0, nonzero = 0;

    if (argc < 2) { fprintf(stderr, "usage: %s rom.nes [frames]\n", argv[0]); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { perror("rom"); return 1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    rom = malloc(sz); if (fread(rom, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);

    if (nes_load(rom, sz) != 0) { fprintf(stderr, "bad ROM\n"); return 1; }
    nes_reset();
    audio = malloc((size_t)total * spf * sizeof(int16_t));

    for (frame = 0; frame < total; frame++) {
        uint8_t pad = 0;
        if (frame >= 120 && frame < 126) pad |= PAD_START;           /* leave the title screen */
        if (frame >= 420) {                                          /* run right, jump now and then */
            pad |= PAD_RIGHT | PAD_B;
            if ((frame / 20) % 3 == 0) pad |= PAD_A;
        }
        nes_set_pad(pad);
        nes_run_frame(fb);
        nes_audio_render(audio + (size_t)frame * spf, spf);

        if (frame == 90 || frame == 300 || frame == 440 || frame == 700 || frame == 1100 || frame == total - 1) {
            char name[64];
            snprintf(name, sizeof name, "/tmp/frame_%04d.ppm", frame);
            save_ppm(name);
            printf("frame %4d: lives=%d world=%d level=%d  playerX(page,x)=(%d,%d)  score digits=%d%d%d%d%d%d\n",
                   frame, nes_peek_ram(0x075A) + 1, nes_peek_ram(0x075F) + 1, nes_peek_ram(0x0760) + 1,
                   nes_peek_ram(0x006D), nes_peek_ram(0x0086),
                   nes_peek_ram(0x07DD), nes_peek_ram(0x07DE), nes_peek_ram(0x07DF),
                   nes_peek_ram(0x07E0), nes_peek_ram(0x07E1), nes_peek_ram(0x07E2));
        }
    }

    { size_t i, n = (size_t)total * spf;
      for (i = 0; i < n; i++) { int a = abs(audio[i]); if (a > peak) peak = a; if (a) nonzero++; } }
    printf("audio: %d frames, peak=%d, nonzero samples=%d\n", total, peak, nonzero);

    f = fopen("/tmp/out.wav", "wb");
    fwrite("RIFF", 1, 4, f); le32(f, 36 + (uint32_t)total * spf * 2); fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 1); le32(f, NES_AUDIO_RATE); le32(f, NES_AUDIO_RATE * 2); le16(f, 2); le16(f, 16);
    fwrite("data", 1, 4, f); le32(f, (uint32_t)total * spf * 2);
    fwrite(audio, 2, (size_t)total * spf, f);
    fclose(f);
    return 0;
}
