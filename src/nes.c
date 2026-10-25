/* Cartridge loading, I/O bus, controller, Game Genie and frame loop. */
#include "nes.h"
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define NES_MAX_CHEATS 16

typedef struct {
    uint16_t address;
    uint8_t value;
    uint8_t compare;
    uint8_t has_compare;
} nes_game_genie_cheat_t;

static nes_game_genie_cheat_t game_genie_cheats[NES_MAX_CHEATS];
static int game_genie_count;

static uint8_t pad, pad_shift, strobe;

static int game_genie_nibble(char c)
{
    static const char alphabet[] = "APZLGITYEOXUKSVN";
    int i;

    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');

    for (i = 0; i < 16; i++) {
        if (alphabet[i] == c)
            return i;
    }

    return -1;
}

void nes_game_genie_clear(void)
{
    memset(game_genie_cheats, 0, sizeof game_genie_cheats);
    game_genie_count = 0;
}

int nes_game_genie_add(const char *code)
{
    uint8_t n[8];
    uint16_t address;
    uint8_t value, compare = 0;
    size_t len, i;
    nes_game_genie_cheat_t *cheat;

    if (code == NULL)
        return -1;

    len = strlen(code);
    if (len != 6 && len != 8)
        return -1;

    if (game_genie_count >= NES_MAX_CHEATS)
        return -2;

    for (i = 0; i < len; i++) {
        int v = game_genie_nibble(code[i]);

        if (v < 0)
            return -1;

        n[i] = (uint8_t)v;
    }

    address = (uint16_t)(
        0x8000 |
        ((n[3] & 7) << 12) |
        ((n[5] & 7) << 8)  |
        ((n[4] & 8) << 8)  |
        ((n[2] & 7) << 4)  |
        ((n[1] & 8) << 4)  |
        (n[4] & 7)         |
        (n[3] & 8)
    );

    value = (uint8_t)(
        ((n[1] & 7) << 4) |
        ((n[0] & 8) << 4) |
        (n[0] & 7)         |
        (n[5] & 8)
    );

    if (len == 8) {
        compare = (uint8_t)(
            ((n[7] & 7) << 4) |
            ((n[6] & 8) << 4) |
            (n[6] & 7)         |
            (n[7] & 8)
        );
    }

    cheat = &game_genie_cheats[game_genie_count++];
    cheat->address = address;
    cheat->value = value;
    cheat->compare = compare;
    cheat->has_compare = (len == 8);

    return game_genie_count - 1;
}

/*
 * Call this from the CPU PRG-ROM read path.
 * This function alone does not apply cheats automatically.
 */
uint8_t nes_game_genie_apply(uint16_t address, uint8_t original)
{
    int i;

    for (i = 0; i < game_genie_count; i++) {
        const nes_game_genie_cheat_t *cheat = &game_genie_cheats[i];

        if (cheat->address != address)
            continue;

        if (cheat->has_compare && cheat->compare != original)
            continue;

        return cheat->value;
    }

    return original;
}

int nes_load(const uint8_t *d, size_t n)
{
    int prg16, chr8, f6, f7;
    size_t off;

    if (n < 16 || memcmp(d, "NES\x1a", 4) != 0)
        return -1;

    prg16 = d[4];
    chr8 = d[5];
    f6 = d[6];
    f7 = d[7];

    if (((f6 >> 4) | (f7 & 0xF0)) != 0)
        return -2;

    off = 16 + ((f6 & 4) ? 512 : 0);

    if (n < off + (size_t)prg16 * 16384 +
                 (size_t)chr8 * 8192)
        return -3;

    nes_prg = d + off;
    nes_prg_mask = (prg16 == 1) ? 0x3FFF : 0x7FFF;

    if (chr8) {
        nes_chr = d + off + (size_t)prg16 * 16384;
        nes_chr_is_ram = 0;
    } else {
        memset(nes_chr_ram, 0, sizeof nes_chr_ram);
        nes_chr = nes_chr_ram;
        nes_chr_is_ram = 1;
    }

    nes_mirror_vertical = f6 & 1;
    return 0;
}

void nes_reset(void)
{
    ppu_reset();
    apu_reset();
    cpu_reset();
    pad = pad_shift = strobe = 0;
}

void nes_set_pad(uint8_t p)
{
    pad = p;
}

uint8_t nes_peek_ram(uint16_t a)
{
    return nes_ram[a & 0x7FF];
}

uint8_t nes_io_read(uint16_t a)
{
    if (a < 0x4000)
        return ppu_read_reg(a);

    if (a == 0x4016) {
        uint8_t r = (uint8_t)((pad_shift & 1) | 0x40);

        if (!strobe)
            pad_shift = (uint8_t)((pad_shift >> 1) | 0x80);

        return r;
    }

    if (a == 0x4015)
        return apu_read_status();

    if (a == 0x4017)
        return 0x40;

    return 0;
}

void nes_io_write(uint16_t a, uint8_t d)
{
    if (a < 0x4000) {
        ppu_write_reg(a, d);
        return;
    }

    if (a == 0x4014) {
        uint16_t base = (uint16_t)(d << 8);
        int i;

        for (i = 0; i < 256; i++)
            ppu_oam_write(
                (uint8_t)i,
                cpu_peek((uint16_t)(base + i))
            );

        cpu_stall(513);
        return;
    }

    if (a == 0x4016) {
        strobe = d & 1;
        pad_shift = pad;
        return;
    }

    if ((a >= 0x4000 && a <= 0x4013) ||
        a == 0x4015 || a == 0x4017)
        apu_write(a, d);
}

void nes_run_frame(uint16_t *fb)
{
    int sl, dots = 0;

    for (sl = 0; sl < 262; sl++) {
        if (sl == 241)
            ppu_vblank_start();
        else if (sl == 261)
            ppu_prerender_start();

        if (sl < 240)
            ppu_render_scanline(
                sl, fb + sl * NES_SCREEN_W
            );

        dots += 341;

        while (dots > 0) {
            int cycles = cpu_step();

            if (cycles <= 0)
                return;

            dots -= cycles * 3;
        }

        if (sl < 240)
            ppu_scanline_end();
        else if (sl == 261)
            ppu_prerender_end();
    }
}

void nes_audio_render(int16_t *dst, int n)
{
    apu_generate(dst, n);
}
