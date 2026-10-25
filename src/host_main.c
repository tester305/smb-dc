/* Headless test harness: runs the ROM, scripts the controller, dumps PPM frames + a WAV. */
#include "nes.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

static uint16_t fb[NES_SCREEN_W * NES_SCREEN_H];

static int write_all(FILE *f, const void *data, size_t size, size_t count)
{
    return fwrite(data, size, count, f) == count;
}

static uint8_t expand5(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t expand6(unsigned v) { return (uint8_t)((v << 2) | (v >> 4)); }

static int save_ppm(const char *name)
{
    FILE *f = fopen(name, "wb");
    int i, ok = 1;
    if (!f) { perror(name); return 0; }
    if (fprintf(f, "P6\n%d %d\n255\n", NES_SCREEN_W, NES_SCREEN_H) < 0)
        ok = 0;
    for (i = 0; ok && i < NES_SCREEN_W * NES_SCREEN_H; i++) {
        uint16_t c = fb[i];
        uint8_t px[3] = {
            expand5((c >> 11) & 31),
            expand6((c >> 5) & 63),
            expand5(c & 31)
        };
        if (!write_all(f, px, 1, sizeof px)) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "error writing PPM: %s\n", name);
    return ok;
}

static int le32(FILE *f, uint32_t v)
{
    return fputc((int)(v & 0xff), f) != EOF &&
           fputc((int)((v >> 8) & 0xff), f) != EOF &&
           fputc((int)((v >> 16) & 0xff), f) != EOF &&
           fputc((int)((v >> 24) & 0xff), f) != EOF;
}

static int le16(FILE *f, uint16_t v)
{
    return fputc((int)(v & 0xff), f) != EOF &&
           fputc((int)((v >> 8) & 0xff), f) != EOF;
}

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f;
    long end;
    uint8_t *buf;
    size_t size;

    *out_size = 0;
    f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "could not determine ROM size: %s\n", path);
        fclose(f);
        return NULL;
    }
    if (end < 16) {
        fprintf(stderr, "ROM is too small to be an iNES file\n");
        fclose(f);
        return NULL;
    }
    size = (size_t)end;
    buf = (uint8_t *)malloc(size);
    if (!buf) {
        fprintf(stderr, "out of memory reading ROM (%lu bytes)\n", (unsigned long)size);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, size, f) != size) {
        fprintf(stderr, "short read while loading ROM: %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "error closing ROM: %s\n", path);
        free(buf);
        return NULL;
    }
    *out_size = size;
    return buf;
}

int main(int argc, char **argv)
{
    uint8_t *rom = NULL;
    size_t rom_size = 0;
    int frame, total = 1800, peak = 0, nonzero = 0;
    int16_t *audio = NULL;
    size_t total_samples, sample_pos = 0;
    FILE *wav = NULL;
    int exit_code = 1;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s rom.nes [frames]\n", argv[0]);
        return 2;
    }
    if (argc == 3) {
        char *endptr;
        long parsed;
        if (!argv[2][0]) {
            fprintf(stderr, "frames must be a positive integer\n");
            return 2;
        }
        parsed = strtol(argv[2], &endptr, 10);
        if (*endptr || parsed < 1 || parsed > 1000000) {
            fprintf(stderr, "frames must be between 1 and 1000000\n");
            return 2;
        }
        total = (int)parsed;
    }
    if (NES_AUDIO_RATE <= 0) {
        fprintf(stderr, "NES_AUDIO_RATE must be positive\n");
        return 2;
    }

    rom = read_file(argv[1], &rom_size);
    if (!rom) goto cleanup;
    if (nes_load(rom, rom_size) != 0) {
        fprintf(stderr, "bad or unsupported ROM (expected mapper-0/NROM iNES)\n");
        goto cleanup;
    }
    nes_reset();

    /* Distribute fractional samples across frames instead of truncating rate/60. */
    if ((size_t)total > SIZE_MAX / (size_t)NES_AUDIO_RATE) {
        fprintf(stderr, "requested audio length is too large\n");
        goto cleanup;
    }
    total_samples = ((size_t)total * (size_t)NES_AUDIO_RATE + 59u) / 60u;
    if (total_samples > SIZE_MAX / sizeof(*audio)) {
        fprintf(stderr, "requested audio buffer is too large\n");
        goto cleanup;
    }
    audio = (int16_t *)malloc(total_samples * sizeof(*audio));
    if (!audio) {
        fprintf(stderr, "out of memory allocating audio buffer\n");
        goto cleanup;
    }

    for (frame = 0; frame < total; frame++) {
        uint8_t pad = 0;
        size_t begin = ((size_t)frame * (size_t)NES_AUDIO_RATE) / 60u;
        size_t end = ((size_t)(frame + 1) * (size_t)NES_AUDIO_RATE) / 60u;
        size_t count = end - begin;

        if (frame >= 120 && frame < 126) pad |= PAD_START; /* leave title screen */
        if (frame >= 420) {
            pad |= PAD_RIGHT | PAD_B;
            if ((frame / 20) % 3 == 0) pad |= PAD_A;
        }
        nes_set_pad(pad);
        nes_run_frame(fb);
        nes_audio_render(audio + sample_pos, (int)count);
        sample_pos += count;

        if (frame == 90 || frame == 300 || frame == 440 || frame == 700 ||
            frame == 1100 || frame == total - 1) {
            char name[64];
            snprintf(name, sizeof name, "/tmp/frame_%04d.ppm", frame);
            if (!save_ppm(name)) goto cleanup;
            printf("frame %4d: lives=%d world=%d level=%d  playerX(page,x)=(%d,%d)  score digits=%d%d%d%d%d%d\n",
                   frame, nes_peek_ram(0x075A) + 1, nes_peek_ram(0x075F) + 1,
                   nes_peek_ram(0x0760) + 1, nes_peek_ram(0x006D),
                   nes_peek_ram(0x0086), nes_peek_ram(0x07DD),
                   nes_peek_ram(0x07DE), nes_peek_ram(0x07DF),
                   nes_peek_ram(0x07E0), nes_peek_ram(0x07E1),
                   nes_peek_ram(0x07E2));
        }
    }

    {
        size_t i;
        for (i = 0; i < sample_pos; i++) {
            int a = audio[i];
            if (a < 0) a = -a;
            if (a > peak) peak = a;
            if (audio[i]) nonzero++;
        }
    }
    printf("audio: %d frames, %lu samples, peak=%d, nonzero samples=%d\n",
           total, (unsigned long)sample_pos, peak, nonzero);

    if (sample_pos > (UINT32_MAX - 36u) / 2u) {
        fprintf(stderr, "WAV would exceed the classic RIFF 32-bit size limit\n");
        goto cleanup;
    }
    wav = fopen("/tmp/out.wav", "wb");
    if (!wav) { perror("/tmp/out.wav"); goto cleanup; }
    if (!write_all(wav, "RIFF", 1, 4) || !le32(wav, 36u + (uint32_t)sample_pos * 2u) ||
        !write_all(wav, "WAVEfmt ", 1, 8) || !le32(wav, 16) ||
        !le16(wav, 1) || !le16(wav, 1) || !le32(wav, (uint32_t)NES_AUDIO_RATE) ||
        !le32(wav, (uint32_t)NES_AUDIO_RATE * 2u) || !le16(wav, 2) || !le16(wav, 16) ||
        !write_all(wav, "data", 1, 4) || !le32(wav, (uint32_t)sample_pos * 2u) ||
        !write_all(wav, audio, sizeof(*audio), sample_pos)) {
        fprintf(stderr, "error writing /tmp/out.wav\n");
        goto cleanup;
    }
    if (fclose(wav) != 0) {
        wav = NULL;
        fprintf(stderr, "error closing /tmp/out.wav\n");
        goto cleanup;
    }
    wav = NULL;
    printf("wrote /tmp/out.wav and selected /tmp/frame_*.ppm screenshots\n");
    exit_code = 0;

cleanup:
    if (wav) fclose(wav);
    free(audio);
    free(rom);
    return exit_code;
}
