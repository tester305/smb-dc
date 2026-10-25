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

/* ---- menu helpers ----------------------------------------------------- */

static void menu_clear(void)
{
    memset(vram_s, 0, 640 * 480 * 2);
}

static void menu_draw_str(int y, const char *str, int highlight)
{
    int len = (int)strlen(str);
    int x = (640 - len * 12) / 2;
    if (x < 8) x = 8;

    if (highlight)
        bfont_draw_str(vram_s + y * 640 + (x - 16), 640, true, ">");
    bfont_draw_str(vram_s + y * 640 + x, 640, true, str);
}

static void menu_draw_str_left(int y, int x, const char *str)
{
    bfont_draw_str(vram_s + y * 640 + x, 640, true, str);
}

/* Wait until A/Start is released then pressed again (or quit combo). */
static void wait_button(void)
{
    int quit_flag = 0;
    uint8_t prev = 0xFF;
    int released = 0;

    while (1) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return;
        if (!(pad & (PAD_A | PAD_START)))
            released = 1;
        if (released && (pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START)))
            return;
        prev = pad;
        vid_waitvbl();
    }
}

/* Yes/No prompt. Returns 1 = Yes, 0 = No. */
static int confirm_yes_no(const char *line1, const char *line2)
{
    int sel = 0;   /* 0 = No (safer default), 1 = Yes */
    int quit_flag = 0;
    uint8_t prev = 0;
    int debounce = 0;

    bfont_set_encoding(BFONT_CODE_ISO8859_1);

    while (1) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return 0;

        if (debounce > 0) {
            debounce--;
        } else {
            if ((pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) &&
                !(prev & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
                sel ^= 1;
                debounce = 12;
            }
            if ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START))) {
                return sel;   /* 1 = Yes, 0 = No */
            }
            if ((pad & PAD_B) && !(prev & PAD_B)) {
                return 0;     /* B = cancel / No */
            }
        }
        prev = pad;

        menu_clear();
        if (line1) menu_draw_str(140, line1, 0);
        if (line2) menu_draw_str(170, line2, 0);
        menu_draw_str(240, "Yes", sel == 1);
        menu_draw_str(280, "No",  sel == 0);
        menu_draw_str(340, "A/Start = OK   B = Cancel", 0);
        vid_waitvbl();
    }
}

/* ---- self-test suite -------------------------------------------------- */

#define RGB565(r, g, b) (uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3))

static int test_graphics(char *err, size_t errsz)
{
    int x, y;
    uint16_t *v = vram_s;
    uint16_t colors[8] = {
        RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255), RGB565(255, 255, 0),
        RGB565(255, 0, 255), RGB565(0, 255, 255), RGB565(255, 255, 255), RGB565(0, 0, 0)
    };

    menu_clear();
    menu_draw_str(40, "Testing graphics...", 0);
    vid_waitvbl();

    /* Color bars */
    for (y = 80; y < 200; y++) {
        for (x = 0; x < 640; x++) {
            v[y * 640 + x] = colors[(x * 8) / 640];
        }
    }
    for (y = 0; y < 30; y++) vid_waitvbl();

    /* Checkerboard */
    for (y = 210; y < 340; y++) {
        for (x = 0; x < 640; x++) {
            int c = ((x / 16) ^ (y / 16)) & 1;
            v[y * 640 + x] = c ? RGB565(200, 200, 200) : RGB565(40, 40, 40);
        }
    }
    for (y = 0; y < 30; y++) vid_waitvbl();

    /* Write/read-back check on a few pixels */
    v[100 * 640 + 100] = RGB565(123, 45, 67);
    v[200 * 640 + 300] = RGB565(10, 20, 30);
    if (v[100 * 640 + 100] != RGB565(123, 45, 67) ||
        v[200 * 640 + 300] != RGB565(10, 20, 30)) {
        snprintf(err, errsz, "Graphics: framebuffer read-back failed");
        return 0;
    }

    /* bfont still works after fills */
    menu_clear();
    menu_draw_str(200, "Graphics OK", 0);
    for (y = 0; y < 40; y++) vid_waitvbl();
    return 1;
}

static int test_sound_system(char *err, size_t errsz)
{
    int i;
    uint32_t mem;

    menu_clear();
    menu_draw_str(40, "Testing sound system...", 0);
    menu_draw_str(80, "(BIOS / AICA init)", 0);
    vid_waitvbl();

    if (snd_init() != 0) {
        snprintf(err, errsz, "Sound: snd_init() failed");
        return 0;
    }

    mem = snd_mem_available();
    if (mem == 0) {
        snd_shutdown();
        snprintf(err, errsz, "Sound: no AICA RAM available");
        return 0;
    }

    /* Exercise stream layer briefly (alloc + destroy) */
    if (snd_stream_init() != 0) {
        snd_shutdown();
        snprintf(err, errsz, "Sound: snd_stream_init() failed");
        return 0;
    }
    snd_stream_shutdown();
    snd_shutdown();

    menu_clear();
    menu_draw_str(200, "Sound system OK", 0);
    for (i = 0; i < 40; i++) vid_waitvbl();
    return 1;
}

/* Tiny stream callback used only during APU self-test */
static int16_t test_tone_buf[1024];
static void *test_apu_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes)
{
    int n = req_bytes / 2;
    (void)hnd;
    if (n > 1024) n = 1024;
    apu_generate(test_tone_buf, n);
    *got_bytes = n * 2;
    return test_tone_buf;
}

/*
 * APU self-test: program a short pulse tone, generate samples, check they
 * are neither silent nor stuck at full scale. Also plays them briefly.
 */
static int test_apu(char *err, size_t errsz)
{
    int16_t samples[2048];
    int i, nonzero = 0, clipped = 0;
    int maxabs = 0;
    snd_stream_hnd_t h;

    menu_clear();
    menu_draw_str(40, "Testing APU...", 0);
    menu_draw_str(80, "You may hear short tones", 0);
    vid_waitvbl();

    apu_reset();

    /* Enable pulse 1, constant volume, duty 50%, period for \~440 Hz-ish */
    apu_write(0x4015, 0x01);          /* enable pulse 1 */
    apu_write(0x4000, 0xBF);          /* duty 2, const vol, vol=15 */
    apu_write(0x4001, 0x08);          /* sweep off */
    apu_write(0x4002, 0xFD);          /* timer low  (approx A4) */
    apu_write(0x4003, 0x00);          /* timer high + length */

    apu_generate(samples, 2048);

    for (i = 0; i < 2048; i++) {
        int a = samples[i] < 0 ? -samples[i] : samples[i];
        if (a > 50) nonzero++;
        if (a >= 30000) clipped++;
        if (a > maxabs) maxabs = a;
    }

    if (nonzero < 100) {
        snprintf(err, errsz, "APU: output was silent (no tone)");
        apu_reset();
        return 0;
    }
    if (clipped > 1500) {
        snprintf(err, errsz, "APU: output heavily clipped");
        apu_reset();
        return 0;
    }
    if (maxabs < 100) {
        snprintf(err, errsz, "APU: output level too low");
        apu_reset();
        return 0;
    }

    /* Play the tone through AICA so it is audible */
    if (snd_init() == 0) {
        snd_stream_init();
        h = snd_stream_alloc(test_apu_cb, 4096);
        if (h >= 0) {
            snd_stream_start(h, NES_AUDIO_RATE, 0);
            for (i = 0; i < 45; i++) {   /* \~0.75 s */
                snd_stream_poll(h);
                vid_waitvbl();
            }
            /* Switch to noise for a moment */
            apu_reset();
            apu_write(0x4015, 0x08);
            apu_write(0x400C, 0x3F);
            apu_write(0x400E, 0x04);
            apu_write(0x400F, 0x08);
            for (i = 0; i < 30; i++) {
                snd_stream_poll(h);
                vid_waitvbl();
            }
            snd_stream_stop(h);
            snd_stream_destroy(h);
        }
        snd_stream_shutdown();
        snd_shutdown();
    }

    apu_reset();

    menu_clear();
    menu_draw_str(200, "APU OK", 0);
    for (i = 0; i < 40; i++) vid_waitvbl();
    return 1;
}

static void run_diagnostics(void)
{
    char err[128];
    int ok = 1;
    const char *fail_msg = NULL;

    err[0] = '\0';

    if (!test_graphics(err, sizeof(err))) {
        ok = 0;
        fail_msg = err;
        goto done;
    }
    if (!test_sound_system(err, sizeof(err))) {
        ok = 0;
        fail_msg = err;
        goto done;
    }
    if (!test_apu(err, sizeof(err))) {
        ok = 0;
        fail_msg = err;
        goto done;
    }

done:
    menu_clear();
    if (ok) {
        menu_draw_str(200, "All done, it's fine.", 0);
    } else {
        menu_draw_str(160, "Abnormality detected:", 0);
        menu_draw_str(200, fail_msg ? fail_msg : "Unknown error", 0);
    }
    menu_draw_str(300, "Press A / Start to continue", 0);
    wait_button();
}

/* ---- main menu -------------------------------------------------------- */

#define MENU_ITEMS 3
static const char *menu_labels[MENU_ITEMS] = {
    "Start Game",
    "Test",
    "Quit"
};

/*
 * Blocks until Start Game or Quit.
 * Returns 1 = Start Game, 0 = Quit.
 * Test is handled entirely inside and returns to the menu.
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
            return 0;

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
                if (sel == 0)
                    return 1;   /* Start Game */
                if (sel == 2)
                    return 0;   /* Quit */
                /* sel == 1 → Test */
                if (confirm_yes_no(
                        "Are you sure you wanna test?",
                        "It might look or sound creepy.")) {
                    run_diagnostics();
                }
                /* after test (or No) fall back to menu */
                prev = 0;
                debounce = 12;
            }
        }
        prev = pad;

        menu_clear();
        menu_draw_str(120, "Welcome to SMB for Dreamcast", 0);
        menu_draw_str(160, "A / Start = Select", 0);
        menu_draw_str(240, menu_labels[0], sel == 0);
        menu_draw_str(280, menu_labels[1], sel == 1);
        menu_draw_str(320, menu_labels[2], sel == 2);
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
