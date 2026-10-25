/*
 * Minimal NES core (mapper 0 / NROM) used to run the Super Mario Bros.
 * disassembly build on the Sega Dreamcast.
 */
#ifndef NES_CORE_H
#define NES_CORE_H

#include <stdint.h>
#include <stddef.h>

#define NES_SCREEN_W   256
#define NES_SCREEN_H   240
#define NES_AUDIO_RATE 44100

enum {
    PAD_A      = 0x01,
    PAD_B      = 0x02,
    PAD_SELECT = 0x04,
    PAD_START  = 0x08,
    PAD_UP     = 0x10,
    PAD_DOWN   = 0x20,
    PAD_LEFT   = 0x40,
    PAD_RIGHT  = 0x80
};

/* Public API */
int nes_load(const uint8_t *image, size_t size);
void nes_reset(void);
void nes_power_on(void);
void nes_set_pad(uint8_t pad);

/* Runs one frame. Framebuffer is 256x240 RGB565. */
void nes_run_frame(uint16_t *fb);

/* Generate mono 16-bit audio samples. */
void nes_audio_render(int16_t *dst, int n);

uint8_t nes_peek_ram(uint16_t addr);

/*
 * Game Genie:
 * 6-character codes replace a ROM byte.
 * 8-character codes replace a ROM byte when the compare matches.
 *
 * nes_game_genie_add returns 0 on success, -1 on invalid code
 * or when the cheat list is full.
 */
void nes_game_genie_clear(void);
int nes_game_genie_add(const char *code);
int nes_game_genie_count(void);

/* Internal shared state */
extern uint8_t nes_ram[0x800];

extern const uint8_t *nes_prg;
extern uint16_t nes_prg_mask;

extern const uint8_t *nes_chr;
extern int nes_chr_is_ram;
extern uint8_t nes_chr_ram[0x2000];

extern int nes_mirror_vertical;
extern int nes_nmi_pending;

/* CPU */
void cpu_reset(void);
int cpu_step(void);
void cpu_stall(int cycles);
uint8_t cpu_peek(uint16_t addr);

/* I/O bus */
uint8_t nes_io_read(uint16_t addr);
void nes_io_write(uint16_t addr, uint8_t data);

/* PPU */
void ppu_reset(void);
uint8_t ppu_read_reg(uint16_t addr);
void ppu_write_reg(uint16_t addr, uint8_t data);
void ppu_oam_write(uint8_t offset, uint8_t data);
uint8_t ppu_oam_addr(void);
void ppu_vblank_start(void);
void ppu_prerender_start(void);
void ppu_prerender_end(void);
void ppu_render_scanline(int y, uint16_t *row);
void ppu_scanline_end(void);

/* APU */
void apu_reset(void);
void apu_write(uint16_t addr, uint8_t data);
uint8_t apu_read_status(void);
void apu_generate(int16_t *out, int n);

#endif
