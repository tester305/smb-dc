/* Scanline-based 2C02 PPU renderer (RGB565 output). */
#include "nes.h"
#include <string.h>

const uint8_t *nes_chr;
int            nes_chr_is_ram;
uint8_t        nes_chr_ram[0x2000];
int            nes_mirror_vertical = 1;

static uint8_t  vram[0x800], pal[32], oam[256];
static uint8_t  ctrl, mask, status, oam_addr, fine_x, wlatch, read_buf;
static uint16_t v, t;
static uint16_t rgb565[64];

static const uint8_t nes_rgb[64][3] = {
    {84,84,84},{0,30,116},{8,16,144},{48,0,136},{68,0,100},{92,0,48},{84,4,0},{60,24,0},
    {32,42,0},{8,58,0},{0,64,0},{0,60,0},{0,50,60},{0,0,0},{0,0,0},{0,0,0},
    {152,150,152},{8,76,196},{48,50,236},{92,30,228},{136,20,176},{160,20,100},{152,34,32},{120,60,0},
    {84,90,0},{40,114,0},{8,124,0},{0,118,40},{0,102,120},{0,0,0},{0,0,0},{0,0,0},
    {236,238,236},{76,154,236},{120,124,236},{176,98,236},{228,84,236},{236,88,180},{236,106,100},{212,136,32},
    {160,170,0},{116,196,0},{76,208,32},{56,204,108},{56,180,204},{60,60,60},{0,0,0},{0,0,0},
    {236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},
    {204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{0,0,0},{0,0,0}
};

void ppu_reset(void)
{
    int i;
    memset(vram, 0, sizeof vram);
    memset(pal, 0, sizeof pal);
    memset(oam, 0, sizeof oam);
    ctrl = mask = status = oam_addr = fine_x = wlatch = read_buf = 0;
    v = t = 0;
    for (i = 0; i < 64; i++)
        rgb565[i] = (uint16_t)(((nes_rgb[i][0] >> 3) << 11) | ((nes_rgb[i][1] >> 2) << 5) | (nes_rgb[i][2] >> 3));
}

static inline unsigned nt_index(uint16_t a)
{
    unsigned tbl;
    a &= 0x0FFF;
    tbl = a >> 10;
    tbl = nes_mirror_vertical ? (tbl & 1) : (tbl >> 1);
    return (tbl << 10) | (a & 0x3FF);
}

static uint8_t mem_read(uint16_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000) return nes_chr[a];
    if (a < 0x3F00) return vram[nt_index(a)];
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= (uint16_t)~0x10;
    return pal[a];
}

static void mem_write(uint16_t a, uint8_t d)
{
    a &= 0x3FFF;
    if (a < 0x2000) { if (nes_chr_is_ram) nes_chr_ram[a] = d; }
    else if (a < 0x3F00) vram[nt_index(a)] = d;
    else {
        a &= 0x1F;
        if ((a & 0x13) == 0x10) a &= (uint16_t)~0x10;
        pal[a] = d & 0x3F;
    }
}

uint8_t ppu_read_reg(uint16_t addr)
{
    uint8_t r = 0;
    switch (addr & 7) {
    case 2:
        r = (uint8_t)(status & 0xE0);
        status &= (uint8_t)~0x80;
        wlatch = 0;
        break;
    case 4:
        r = oam[oam_addr];
        break;
    case 7: {
        uint16_t a = v & 0x3FFF;
        if (a >= 0x3F00) { r = mem_read(a); read_buf = mem_read((uint16_t)(a & 0x2FFF)); }
        else             { r = read_buf;    read_buf = mem_read(a); }
        v = (uint16_t)((v + ((ctrl & 4) ? 32 : 1)) & 0x7FFF);
        break;
    }
    default: break;
    }
    return r;
}

void ppu_write_reg(uint16_t addr, uint8_t d)
{
    switch (addr & 7) {
    case 0: {
        uint8_t old = ctrl;
        ctrl = d;
        t = (uint16_t)((t & 0x73FF) | ((d & 3) << 10));
        if (!(old & 0x80) && (d & 0x80) && (status & 0x80)) nes_nmi_pending = 1;
        break;
    }
    case 1: mask = d; break;
    case 3: oam_addr = d; break;
    case 4: oam[oam_addr++] = d; break;
    case 5:
        if (!wlatch) { t = (uint16_t)((t & ~0x1F) | (d >> 3)); fine_x = d & 7; wlatch = 1; }
        else         { t = (uint16_t)((t & ~0x73E0) | ((d & 7) << 12) | ((d & 0xF8) << 2)); wlatch = 0; }
        break;
    case 6:
        if (!wlatch) { t = (uint16_t)((t & 0x00FF) | ((d & 0x3F) << 8)); wlatch = 1; }
        else         { t = (uint16_t)((t & 0xFF00) | d); v = t; wlatch = 0; }
        break;
    case 7:
        mem_write(v & 0x3FFF, d);
        v = (uint16_t)((v + ((ctrl & 4) ? 32 : 1)) & 0x7FFF);
        break;
    }
}

void    ppu_oam_write(uint8_t off, uint8_t d) { oam[(uint8_t)(oam_addr + off)] = d; }
uint8_t ppu_oam_addr(void)                    { return oam_addr; }

void ppu_vblank_start(void)
{
    status |= 0x80;
    if (ctrl & 0x80) nes_nmi_pending = 1;
}
void ppu_prerender_start(void) { status &= (uint8_t)~0xE0; }
void ppu_prerender_end(void)   { if (mask & 0x18) v = t; }

void ppu_scanline_end(void)
{
    if (!(mask & 0x18)) return;
    if ((v & 0x7000) != 0x7000) v += 0x1000;
    else {
        int cy = (v & 0x03E0) >> 5;
        v &= (uint16_t)~0x7000;
        if (cy == 29)      { cy = 0; v ^= 0x0800; }
        else if (cy == 31) cy = 0;
        else               cy++;
        v = (uint16_t)((v & ~0x03E0) | (cy << 5));
    }
    v = (uint16_t)((v & ~0x041F) | (t & 0x041F));
}

void ppu_render_scanline(int y, uint16_t *row)
{
    uint8_t bgpix[256];       /* 0..3 pattern value, for priority + sprite-0 hit */
    uint8_t col[256];         /* palette index 0..31 */
    int x;

    memset(bgpix, 0, sizeof bgpix);
    memset(col, 0, sizeof col);

    if (mask & 0x08) {
        uint16_t vv = v;
        int fy = (vv >> 12) & 7;
        unsigned pbase = (ctrl & 0x10) ? 0x1000 : 0;
        int out = -(int)fine_x, tile;

        for (tile = 0; tile < 33; tile++) {
            uint8_t id  = vram[nt_index((uint16_t)(0x2000 | (vv & 0x0FFF)))];
            uint8_t at  = vram[nt_index((uint16_t)(0x23C0 | (vv & 0x0C00) | ((vv >> 4) & 0x38) | ((vv >> 2) & 0x07)))];
            int     pl  = (at >> (((vv >> 4) & 4) | (vv & 2))) & 3;
            uint8_t lo  = nes_chr[pbase + id * 16 + fy];
            uint8_t hi  = nes_chr[pbase + id * 16 + fy + 8];
            int bit;
            for (bit = 0; bit < 8; bit++) {
                int px = out + bit, sh = 7 - bit, p;
                if (px < 0 || px > 255) continue;
                p = ((lo >> sh) & 1) | (((hi >> sh) & 1) << 1);
                bgpix[px] = (uint8_t)p;
                col[px]   = p ? (uint8_t)((pl << 2) | p) : 0;
            }
            if ((vv & 0x1F) == 31) { vv &= (uint16_t)~0x1F; vv ^= 0x0400; } else vv++;
            out += 8;
        }
        if (!(mask & 0x02)) for (x = 0; x < 8; x++) { bgpix[x] = 0; col[x] = 0; }
    }

    if (mask & 0x10) {
        int h = (ctrl & 0x20) ? 16 : 8, n = 0, i;
        uint8_t done[256];
        memset(done, 0, sizeof done);
        for (i = 0; i < 64; i++) {
            const uint8_t *o = &oam[i * 4];
            int r = y - (o[0] + 1), px;
            uint8_t attr, lo, hi;
            unsigned a;
            if (r < 0 || r >= h) continue;
            if (++n > 8) { status |= 0x20; break; }
            attr = o[2];
            if (attr & 0x80) r = h - 1 - r;
            if (h == 8) a = ((ctrl & 0x08) ? 0x1000 : 0) + o[1] * 16 + r;
            else        a = ((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16 + ((r & 8) ? 16 : 0) + (r & 7);
            lo = nes_chr[a];
            hi = nes_chr[a + 8];
            for (px = 0; px < 8; px++) {
                int sx = o[3] + px, bit, p;
                if (sx > 255) break;
                bit = (attr & 0x40) ? px : 7 - px;
                p = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!p || done[sx]) continue;
                done[sx] = 1;
                if (i == 0 && bgpix[sx] && sx != 255 && !(sx < 8 && !((mask & 0x02) && (mask & 0x04))))
                    status |= 0x40;
                if (sx < 8 && !(mask & 0x04)) continue;
                if ((attr & 0x20) && bgpix[sx]) continue;
                col[sx] = (uint8_t)(0x10 | ((attr & 3) << 2) | p);
            }
        }
    }

    {
        uint8_t gray = (mask & 1) ? 0x30 : 0x3F;
        for (x = 0; x < 256; x++) row[x] = rgb565[pal[col[x]] & gray];
    }
}
