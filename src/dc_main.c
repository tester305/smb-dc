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
#include <kos/thread.h>
#include <dc/video.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>
#include <dc/flashrom.h>
#include <dc/syscalls.h>
#include <dc/wdt.h>

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

static void menu_init_font(void);   /* defined with menu helpers below */

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
    int pitch = (vid_mode && vid_mode->width) ? (int)vid_mode->width : 640;
    menu_init_font();
    vid_clear(0, 0, 0);
    bfont_draw_str(vram_s + 24 * pitch + 24, pitch, true, msg);
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

static int menu_pitch(void)
{
    return (vid_mode && vid_mode->width) ? (int)vid_mode->width : 640;
}

/* Reset bfont to clean white-on-black (BIOS may leave garbage colors).
 * bfont_set_32bit_mode() is gone in current KOS; we always use PM_RGB565. */
static void menu_init_font(void)
{
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    /* RGB565: white foreground, black background */
    bfont_set_foreground_color(0xFFFF);
    bfont_set_background_color(0x0000);
}

static void menu_clear(void)
{
    vid_clear(0, 0, 0);
}

static void menu_draw_str(int y, const char *str, int highlight)
{
    int pitch = menu_pitch();
    int len = (int)strlen(str);
    int x = (pitch - len * 12) / 2;
    if (x < 8) x = 8;

    if (highlight)
        bfont_draw_str(vram_s + y * pitch + (x - 16), pitch, true, ">");
    bfont_draw_str(vram_s + y * pitch + x, pitch, true, str);
}

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

static int confirm_yes_no(const char *line1, const char *line2)
{
    int sel = 0;
    int quit_flag = 0;
    uint8_t prev = 0;
    int debounce = 0;
    int armed = 0;

    menu_init_font();

    while (1) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return 0;

        if (!armed) {
            if (!(pad & (PAD_A | PAD_START | PAD_B)))
                armed = 1;
            prev = pad;
            menu_clear();
            if (line1) menu_draw_str(140, line1, 0);
            if (line2) menu_draw_str(170, line2, 0);
            menu_draw_str(240, "Yes", sel == 1);
            menu_draw_str(280, "No",  sel == 0);
            menu_draw_str(340, "A/Start = OK   B = Cancel", 0);
            vid_waitvbl();
            continue;
        }

        if (debounce > 0) {
            debounce--;
        } else {
            if ((pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) &&
                !(prev & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
                sel ^= 1;
                debounce = 12;
            }
            if ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START))) {
                return sel;
            }
            if ((pad & PAD_B) && !(prev & PAD_B)) {
                return 0;
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
    int x, y, pitch = menu_pitch();
    uint16_t *v = vram_s;
    uint16_t colors[8] = {
        RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255), RGB565(255, 255, 0),
        RGB565(255, 0, 255), RGB565(0, 255, 255), RGB565(255, 255, 255), RGB565(0, 0, 0)
    };

    menu_init_font();
    menu_clear();
    menu_draw_str(40, "Testing graphics...", 0);
    vid_waitvbl();

    for (y = 80; y < 200; y++) {
        for (x = 0; x < pitch; x++) {
            v[y * pitch + x] = colors[(x * 8) / pitch];
        }
    }
    for (y = 0; y < 30; y++) vid_waitvbl();

    for (y = 210; y < 340; y++) {
        for (x = 0; x < pitch; x++) {
            int c = ((x / 16) ^ (y / 16)) & 1;
            v[y * pitch + x] = c ? RGB565(200, 200, 200) : RGB565(40, 40, 40);
        }
    }
    for (y = 0; y < 30; y++) vid_waitvbl();

    v[100 * pitch + 100] = RGB565(123, 45, 67);
    v[200 * pitch + 300] = RGB565(10, 20, 30);
    if (v[100 * pitch + 100] != RGB565(123, 45, 67) ||
        v[200 * pitch + 300] != RGB565(10, 20, 30)) {
        snprintf(err, errsz, "Graphics: framebuffer read-back failed");
        return 0;
    }

    menu_clear();
    menu_draw_str(200, "Graphics OK", 0);
    for (y = 0; y < 40; y++) vid_waitvbl();
    return 1;
}

static volatile int wdt_fired;

static void wdt_test_cb(void *user)
{
    (void)user;
    wdt_fired = 1;
}

static int test_bios_assets(char *err, size_t errsz)
{
    int i, region;
    uint8_t *font;
    uint8_t probe[16];
    flashrom_syscfg_t syscfg;
    uint32_t mem;

    menu_clear();
    menu_draw_str(40, "Testing BIOS assets...", 0);
    menu_draw_str(80, "(ROM font / flashrom / WDT)", 0);
    vid_waitvbl();

    if (syscall_font_lock() != 0) {
        snprintf(err, errsz, "Emulator?: BIOS font lock failed");
        return 0;
    }
    font = syscall_font_address();
    if (!font) {
        syscall_font_unlock();
        snprintf(err, errsz, "Emulator?: no BIOS ROM font");
        return 0;
    }
    for (i = 0; i < 16; i++)
        probe[i] = font[i];
    syscall_font_unlock();

    {
        int all0 = 1, allf = 1;
        for (i = 0; i < 16; i++) {
            if (probe[i] != 0x00) all0 = 0;
            if (probe[i] != 0xFF) allf = 0;
        }
        if (all0 || allf) {
            snprintf(err, errsz, "Emulator?: BIOS font data empty");
            return 0;
        }
    }

    menu_clear();
    menu_draw_str(100, "BIOS ROM font OK", 0);
    menu_draw_str(140, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0);
    menu_draw_str(180, "0123456789 !@#$%", 0);
    for (i = 0; i < 45; i++) vid_waitvbl();

    region = flashrom_get_region();
    if (region < 0) {
        snprintf(err, errsz, "Emulator?: flashrom region failed");
        return 0;
    }
    if (flashrom_get_syscfg(&syscfg) < 0) {
        snprintf(err, errsz, "Emulator?: flashrom syscfg failed");
        return 0;
    }

    menu_clear();
    menu_draw_str(100, "Flashrom / BIOS config OK", 0);
    {
        char line[64];
        snprintf(line, sizeof(line), "Region code: %d", region);
        menu_draw_str(140, line, 0);
    }
    for (i = 0; i < 40; i++) vid_waitvbl();

    if (snd_init() != 0) {
        snprintf(err, errsz, "Sound: snd_init() failed");
        return 0;
    }
    mem = snd_mem_available();
    if (mem == 0) {
        snd_shutdown();
        snprintf(err, errsz, "Sound: no AICA RAM (emu?)");
        return 0;
    }
    if (snd_stream_init() != 0) {
        snd_shutdown();
        snprintf(err, errsz, "Sound: stream init failed");
        return 0;
    }
    snd_stream_shutdown();
    snd_shutdown();

    wdt_fired = 0;
    wdt_enable_timer(0, 1000, 15, wdt_test_cb, NULL);
    thd_sleep(5);
    wdt_disable();

    if (!wdt_fired) {
        snprintf(err, errsz, "Emulator detected (WDT IRQ missing)");
        return 0;
    }

    menu_clear();
    menu_draw_str(200, "BIOS + hardware OK", 0);
    for (i = 0; i < 40; i++) vid_waitvbl();
    return 1;
}

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

    apu_write(0x4015, 0x01);
    apu_write(0x4000, 0xBF);
    apu_write(0x4001, 0x08);
    apu_write(0x4002, 0xFD);
    apu_write(0x4003, 0x08);

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

    if (snd_init() == 0) {
        snd_stream_init();
        h = snd_stream_alloc(test_apu_cb, 4096);
        if (h >= 0) {
            snd_stream_start(h, NES_AUDIO_RATE, 0);
            for (i = 0; i < 45; i++) {
                snd_stream_poll(h);
                vid_waitvbl();
            }
            apu_reset();
            apu_write(0x4015, 0x08);
            apu_write(0x400C, 0x3F);
            apu_write(0x400E, 0x04);
            apu_write(0x400F, 0x18);
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
    if (!test_bios_assets(err, sizeof(err))) {
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
        menu_draw_str(140, "Abnormality detected:", 0);
        menu_draw_str(180, fail_msg ? fail_msg : "Unknown error", 0);
        if (fail_msg && (strstr(fail_msg, "Emulator") || strstr(fail_msg, "BIOS")))
            menu_draw_str(240, "Assuming emulator / no real BIOS", 0);
    }
    menu_draw_str(320, "Press A / Start to continue", 0);
    wait_button();
}

/* ---- main menu -------------------------------------------------------- */

#define MENU_ITEMS 3
static const char *menu_labels[MENU_ITEMS] = {
    "Start Game",
    "Test",
    "Quit"
};

static int run_main_menu(void)
{
    int sel = 0;
    int quit_flag = 0;
    uint8_t prev = 0;
    int debounce = 0;

    menu_init_font();

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
                    return 1;
                if (sel == 2)
                    return 0;
                if (confirm_yes_no(
                        "Are you sure you wanna test?",
                        "It might look or sound creepy.")) {
                    run_diagnostics();
                }
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

    {
        int i;
        int dm = (vid_check_cable() == CT_VGA) ? DM_640x480_VGA : DM_640x480_NTSC_IL;
        vid_set_mode(dm, PM_RGB565);
        vid_empty();
        vid_set_mode(dm, PM_RGB565);
        vid_clear(0, 0, 0);
        menu_init_font();
        for (i = 0; i < 4; i++)
            vid_waitvbl();
    }

    if (!run_main_menu()) {
        return 0;
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

        pvr_wait_render_done();
        pvr_txr_load(fb, txr, TXR_BYTES);

        pvr_wait_ready();
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
