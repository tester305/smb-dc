/*
 * Super Mario Bros. for the Sega Dreamcast (KallistiOS).
 *
 * Runs the ROM built from the SMB disassembly (smb.nes in the romdisk) on the
 * portable NES core in this folder.  Video: the PPU's 256x240 output is uploaded
 * as a PVR texture and drawn as a quad (see INTEGER_SCALE).  Audio: AICA stream
 * whose callback pulls samples straight from the software APU.
 *
 * Controls:  D-pad / stick = move      A = jump      X or B = run / fireball
 *            Start = Start             Y = Select
 *            A + B + X + Y + Start     = exit to the BIOS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kos/init.h>
#include <dc/video.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>

#include "nes.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define TXR_BYTES        (256 * 256 * 2)
#define AUDIO_BUF_BYTES  16384            /* AICA stream buffer: \~185 ms at 44.1 kHz mono */
#define STICK_DEADZONE   40

/* 1 = sharp pixel-perfect 2x (512x480, black bars left/right, nearest filter)
 * 0 = stretched to the full 4:3 screen (bilinear, softer) */
#define INTEGER_SCALE    1

static uint16_t fb[256 * 256] __attribute__((aligned(32)));      /* PPU output, rows 0..239 used */
static int16_t  abuf[AUDIO_BUF_BYTES / 2] __attribute__((aligned(32)));

/* KOS passes/expects BYTES here, even though the parameters are named smp_*. */
static void *audio_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes)
{
    int n = req_bytes / 2;
    (void)hnd;
    if (n > AUDIO_BUF_BYTES / 2) n = AUDIO_BUF_BYTES / 2;
    nes_audio_render(abuf, n);
    *got_bytes = n * 2;
    return abuf;
}

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    *size = buf ? (size_t)sz : 0;
    return buf;
}

static void fatal(const char *msg)
{
    int i;
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    bfont_draw_str(vram_s + 24 * 640 + 24, 640, true, msg);
    for (i = 0; i < 60 * 8; i++) vid_waitvbl();
}

/* returns NES pad bits; sets *quit when the exit combo is held */
static uint8_t read_pad(int *quit)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    uint8_t pad = 0;
    const uint32_t combo = CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START;

    if (!st) return 0;
    if ((st->buttons & combo) == combo) *quit = 1;

    if (st->buttons & CONT_A)               pad |= PAD_A;
    if (st->buttons & (CONT_X | CONT_B))    pad |= PAD_B;
    if (st->buttons & CONT_Y)               pad |= PAD_SELECT;
    if (st->buttons & CONT_START)           pad |= PAD_START;
    if (st->buttons & CONT_DPAD_UP)         pad |= PAD_UP;
    if (st->buttons & CONT_DPAD_DOWN)       pad |= PAD_DOWN;
    if (st->buttons & CONT_DPAD_LEFT)       pad |= PAD_LEFT;
    if (st->buttons & CONT_DPAD_RIGHT)      pad |= PAD_RIGHT;
    if (st->joyx >  STICK_DEADZONE)         pad |= PAD_RIGHT;
    if (st->joyx < -STICK_DEADZONE)         pad |= PAD_LEFT;
    if (st->joyy >  STICK_DEADZONE)         pad |= PAD_DOWN;
    if (st->joyy < -STICK_DEADZONE)         pad |= PAD_UP;
    return pad;
}

static void set_vertex(pvr_vertex_t *v, float x, float y, float u, float t, uint32_t flags)
{
    v->flags = flags;
    v->x = x; v->y = y; v->z = 1.0f;
    v->u = u; v->v = t;
    v->argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    v->oargb = 0;
}

/* ---- simple main menu ------------------------------------------------- */

#define MENU_ITEMS 2
static const char *menu_labels[MENU_ITEMS] = {
    "Start Game",
    "Quit"
};

/* Clear the 640x480 RGB565 framebuffer to black */
static void menu_clear(void)
{
    memset(vram_s, 0, 640 * 480 * 2);
}

/* Draw a centered-ish string on a given scanline */
static void menu_draw_str(int y, const char *str, int highlight)
{
    /* bfont is \~12 px wide per character; centre roughly for 640 width */
    int len = (int)strlen(str);
    int x = (640 - len * 12) / 2;
    if (x < 8) x = 8;

    if (highlight) {
        /* simple prefix cursor */
        bfont_draw_str(vram_s + y * 640 + (x - 16), 640, true, ">");
    }
    bfont_draw_str(vram_s + y * 640 + x, 640, true, str);
}

/*
 * Blocks until the player chooses an item.
 * Returns 1 = Start Game, 0 = Quit.
 */
static int run_main_menu(void)
{
    int sel = 0;
    int quit_flag = 0;
    uint8_t prev = 0;
    int debounce = 0;

    bfont_set_encoding(BFONT_CODE_ISO8859_1);

    while (1) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag)
            return 0;   /* force quit via button combo */

        /* rising-edge detection with a short debounce */
        if (debounce > 0) {
            debounce--;
        } else {
            if ((pad & PAD_UP) && !(prev & PAD_UP)) {
                sel = (sel + MENU_ITEMS - 1) % MENU_ITEMS;
                debounce = 12;
            }
            if ((pad & PAD_DOWN) && !(prev & PAD_DOWN)) {
                sel = (sel + 1) % MENU_ITEMS;
                debounce = 12;
            }
            if ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START))) {
                return (sel == 0) ? 1 : 0;   /* 1 = start, 0 = quit */
            }
        }
        prev = pad;

        menu_clear();

        /* Title */
        menu_draw_str(140, "Welcome to SMB for Dreamcast", 0);

        /* Hint */
        menu_draw_str(180, "A / Start = Select", 0);

        /* Menu items */
        menu_draw_str(260, menu_labels[0], sel == 0);
        menu_draw_str(300, menu_labels[1], sel == 1);

        vid_waitvbl();
    }
}

int main(int argc, char **argv)
{
    uint8_t *rom;
    size_t rom_size = 0;
    pvr_ptr_t txr;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert[4];
    snd_stream_hnd_t stream;
    int quit = 0;
    (void)argc; (void)argv;

    /* 60 Hz output on every cable (PAL consoles use NTSC-60 interlaced on composite/RGB). */
    vid_set_mode(vid_check_cable() == CT_VGA ? DM_640x480_VGA : DM_640x480_NTSC_IL, PM_RGB565);

    /* ---- Main menu (before ROM / PVR / audio) ---- */
    if (!run_main_menu()) {
        return 0;   /* player chose Quit */
    }

    rom = load_file("/rd/smb.nes", &rom_size);
    if (!rom)                                         { fatal("smb.nes missing from romdisk"); return 1; }
    if (nes_load(rom, rom_size) != 0)                 { fatal("smb.nes is not a valid NROM image"); return 1; }
    nes_reset();

    pvr_init_defaults();
    txr = pvr_mem_malloc(TXR_BYTES);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     256, 256, txr, INTEGER_SCALE ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
    pvr_poly_compile(&hdr, &cxt);

    /* quad; the texture is 256x256 but only the top 240 rows hold picture */
    {
        float x0 = INTEGER_SCALE ?  64.0f :   0.0f;
        float x1 = INTEGER_SCALE ? 576.0f : 640.0f;
        set_vertex(&vert[0], x0,   0.0f, 0.0f, 0.0f,          PVR_CMD_VERTEX);
        set_vertex(&vert[1], x1,   0.0f, 1.0f, 0.0f,          PVR_CMD_VERTEX);
        set_vertex(&vert[2], x0, 480.0f, 0.0f, 240.0f/256.0f, PVR_CMD_VERTEX);
        set_vertex(&vert[3], x1, 480.0f, 1.0f, 240.0f/256.0f, PVR_CMD_VERTEX_EOL);
    }

    snd_stream_init();
    stream = snd_stream_alloc(audio_cb, AUDIO_BUF_BYTES);
    snd_stream_start(stream, NES_AUDIO_RATE, 0);

    while (!quit) {
        nes_set_pad(read_pad(&quit));
        nes_run_frame(fb);
        snd_stream_poll(stream);

        pvr_wait_render_done();                  /* don't overwrite the texture mid-render */
        pvr_txr_load(fb, txr, TXR_BYTES);

        pvr_wait_ready();                        /* paces the loop to the display refresh */
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&hdr, sizeof(hdr));
        pvr_prim(&vert[0], sizeof(pvr_vertex_t));
        pvr_prim(&vert[1], sizeof(pvr_vertex_t));
        pvr_prim(&vert[2], sizeof(pvr_vertex_t));
        pvr_prim(&vert[3], sizeof(pvr_vertex_t));
        pvr_list_finish();
        pvr_scene_finish();
    }

    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    snd_stream_shutdown();
    pvr_mem_free(txr);
    pvr_shutdown();
    free(rom);
    return 0;
}
