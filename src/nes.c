/*
 * Cartridge loading, I/O bus, controller and frame loop.
 *
 * Game Genie implementation lives in cpu.c.
 */
#include "nes.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

static uint8_t pad;
static uint8_t pad_shift;
static uint8_t strobe;

int nes_load(const uint8_t *d, size_t n)
{
    int prg16;
    int chr8;
    int f6;
    int f7;
    size_t off;

    if (d == NULL || n < 16)
        return -1;

    if (memcmp(d, "NES\x1a", 4) != 0)
        return -1;

    prg16 = d[4];
    chr8 = d[5];
    f6 = d[6];
    f7 = d[7];

    /* This core supports mapper 0 / NROM only. */
    if (((f6 >> 4) | (f7 & 0xF0)) != 0)
        return -2;

    /* NROM has one or two 16 KiB PRG banks. */
    if (prg16 != 1 && prg16 != 2)
        return -2;

    off = 16 + ((f6 & 4) ? 512 : 0);

    if (n < off +
            (size_t)prg16 * 16384 +
            (size_t)chr8 * 8192)
        return -3;

    nes_prg = d + off;
    nes_prg_mask = (prg16 == 1) ? 0x3FFF : 0x7FFF;

    if (chr8 != 0) {
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

    pad = 0;
    pad_shift = 0;
    strobe = 0;
}

void nes_set_pad(uint8_t p)
{
    pad = p;
}

uint8_t nes_peek_ram(uint16_t a)
{
    return nes_ram[a & 0x07FF];
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

        for (i = 0; i < 256; i++) {
            ppu_oam_write(
                (uint8_t)i,
                cpu_peek((uint16_t)(base + i))
            );
        }

        cpu_stall(513);
        return;
    }

    if (a == 0x4016) {
        strobe = d & 1;
        pad_shift = pad;
        return;
    }

    if ((a >= 0x4000 && a <= 0x4013) ||
        a == 0x4015 ||
        a == 0x4017) {
        apu_write(a, d);
    }
}

void nes_run_frame(uint16_t *fb)
{
    int sl;
    int dots = 0;

    if (fb == NULL)
        return;

    for (sl = 0; sl < 262; sl++) {
        if (sl == 241)
            ppu_vblank_start();
        else if (sl == 261)
            ppu_prerender_start();

        if (sl < 240) {
            ppu_render_scanline(
                sl,
                fb + sl * NES_SCREEN_W
            );
        }

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
    if (dst == NULL || n <= 0)
        return;

    apu_generate(dst, n);
}
