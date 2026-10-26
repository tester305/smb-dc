/*
 * Super Mario Bros. for the Sega Dreamcast (KallistiOS).
 *
 * Runs the ROM built from the SMB disassembly (smb.nes in the romdisk) on the
 * portable NES core in this folder. Video: the PPU's 256x240 output is uploaded
 * as a PVR texture and drawn as a quad (see INTEGER_SCALE). Audio: AICA stream
 * whose callback pulls samples straight from the software APU.
 *
 * Controls: D-pad / stick = move; A = jump; X or B = run / fireball
 *           Start = Start; Y = Select
 *           A + B + X + Y + Start = exit to the BIOS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>

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
#include <dc/vmu_pkg.h>
#include <dc/fs_vmu.h>

#include "nes.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define TXR_BYTES        (256 * 256 * 2)
#define AUDIO_BUF_BYTES  16384
#define STICK_DEADZONE   40

/* 1 = sharp 2x scale (512x480); 0 = full-screen stretch. */
#define INTEGER_SCALE    1

static uint16_t fb[256 * 256] __attribute__((aligned(32)));
static int16_t abuf[AUDIO_BUF_BYTES / 2] __attribute__((aligned(32)));

static void menu_init_font(void);

/* KOS expects byte counts for this stream callback. */
static void *audio_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes)
{
    int n = req_bytes / 2;
    (void)hnd;
    if (n > AUDIO_BUF_BYTES / 2) n = AUDIO_BUF_BYTES / 2;
    if (n < 0) n = 0;
    nes_audio_render(abuf, n);
    *got_bytes = n * 2;
    return abuf;
}

/* Robustly load a file; *size is always initialized. */
static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f;
    uint8_t *buf;
    long end;

    if (size) *size = 0;
    if (!path || !size) return NULL;

    f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    end = ftell(f);
    if (end < 16 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (uint8_t *)malloc((size_t)end);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)end, f) != (size_t)end) {
        free(buf);
        fclose(f);
        return NULL;
    }
    if (fclose(f) != 0) {
        free(buf);
        return NULL;
    }

    *size = (size_t)end;
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

/* Returns NES pad bits; sets *quit when the exit combo is held. */
static uint8_t read_pad(int *quit)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    uint8_t pad = 0;
    const uint32_t combo = CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START;

    if (!st) return 0;
    if (quit && (st->buttons & combo) == combo) *quit = 1;

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

static void set_vertex(pvr_vertex_t *v, float x, float y, float u, float t,
                       uint32_t flags)
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

static void menu_init_font(void)
{
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    bfont_set_foreground_color(0xFFFF);
    bfont_set_background_color(0x0000);
}

static void menu_clear(void)
{
    vid_clear(0, 0, 0);
}

static void menu_present(void)
{
    vid_waitvbl();
    if (vid_mode && (vid_mode->flags & VID_INTERLACE))
        vid_waitvbl();
}

static void menu_draw_str(int y, const char *str, int highlight)
{
    int pitch = menu_pitch();
    int len = (int)strlen(str);
    int x = (pitch - len * 12) / 2;
    if (x < 8) x = 8;

    if (highlight && x >= 16)
        bfont_draw_str(vram_s + y * pitch + x - 16, pitch, true, ">");
    bfont_draw_str(vram_s + y * pitch + x, pitch, true, str);
}

/* Wait until A/Start has been released and pressed again, or quit combo. */
static void wait_button(void)
{
    int quit_flag = 0;
    uint8_t prev = 0xFF;
    int released = 0;

    for (;;) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return;
        if (!(pad & (PAD_A | PAD_START))) released = 1;
        if (released && (pad & (PAD_A | PAD_START)) &&
            !(prev & (PAD_A | PAD_START)))
            return;
        prev = pad;
        vid_waitvbl();
    }
}

/* Yes/No prompt. Returns 1 = Yes, 0 = No. */
static int confirm_yes_no(const char *line1, const char *line2)
{
    int sel = 0;
    int quit_flag = 0;
    uint8_t prev = 0;
    int debounce = 0;
    int armed = 0;
    int dirty = 1;

    menu_init_font();
    for (;;) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return 0;

        if (!armed) {
            if (!(pad & (PAD_A | PAD_START | PAD_B))) {
                armed = 1;
                dirty = 1;
            }
            prev = pad;
        } else if (debounce > 0) {
            debounce--;
        } else {
            if ((pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) &&
                !(prev & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
                sel ^= 1;
                debounce = 12;
                dirty = 1;
            }
            if ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START)))
                return sel;
            if ((pad & PAD_B) && !(prev & PAD_B))
                return 0;
        }
        prev = pad;

        if (dirty) {
            vid_waitvbl();
            menu_clear();
            if (line1) menu_draw_str(140, line1, 0);
            if (line2) menu_draw_str(170, line2, 0);
            menu_draw_str(240, "Yes", sel == 1);
            menu_draw_str(280, "No", sel == 0);
            menu_draw_str(340, "A/Start = OK   B = Cancel", 0);
            menu_present();
            dirty = 0;
        } else {
            vid_waitvbl();
        }
    }
}

/* ---- self-test suite -------------------------------------------------- */

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static void menu_hold(int frames)
{
    int i;
    for (i = 0; i < frames; i++) vid_waitvbl();
}

static int test_graphics(char *err, size_t errsz)
{
    int x, y, pitch = menu_pitch();
    uint16_t *v = vram_s;
    uint16_t colors[8] = {
        RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255),
        RGB565(255, 255, 0), RGB565(255, 0, 255), RGB565(0, 255, 255),
        RGB565(255, 255, 255), RGB565(0, 0, 0)
    };

    menu_init_font();
    menu_clear();
    menu_draw_str(40, "Testing graphics...", 0);
    menu_draw_str(80, "Press A / Start to step", 0);
    menu_hold(90);

    for (y = 80; y < 200; y++)
        for (x = 0; x < pitch; x++)
            v[y * pitch + x] = colors[(x * 8) / pitch];
    menu_draw_str(360, "Color bars  (A = next)", 0);
    wait_button();

    for (y = 210; y < 340; y++)
        for (x = 0; x < pitch; x++) {
            int c = ((x / 16) ^ (y / 16)) & 1;
            v[y * pitch + x] = c ? RGB565(200, 200, 200) : RGB565(40, 40, 40);
        }
    menu_draw_str(360, "Checkerboard  (A = next)", 0);
    wait_button();

    v[100 * pitch + 100] = RGB565(123, 45, 67);
    v[200 * pitch + 300] = RGB565(10, 20, 30);
    if (v[100 * pitch + 100] != RGB565(123, 45, 67) ||
        v[200 * pitch + 300] != RGB565(10, 20, 30)) {
        snprintf(err, errsz, "Graphics: framebuffer read-back failed");
        return 0;
    }

    menu_clear();
    menu_draw_str(200, "Graphics OK", 0);
    menu_draw_str(260, "Press A / Start", 0);
    wait_button();
    return 1;
}

static volatile int wdt_fired;

static void wdt_test_cb(void *user)
{
    (void)user;
    wdt_fired = 1;
}

/*
 * BIOS asset and hardware test. WDT support can vary by emulator/KOS build,
 * so failure is reported as a diagnostic rather than proof of emulation.
 */
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
    menu_draw_str(120, "Press A / Start to step", 0);
    menu_hold(90);

    if (syscall_font_lock() != 0) {
        snprintf(err, errsz, "BIOS font lock failed");
        return 0;
    }
    font = syscall_font_address();
    if (!font) {
        syscall_font_unlock();
        snprintf(err, errsz, "No BIOS ROM font");
        return 0;
    }
    for (i = 0; i < 16; i++) probe[i] = font[i];
    syscall_font_unlock();

    {
        int all0 = 1, allf = 1;
        for (i = 0; i < 16; i++) {
            if (probe[i] != 0x00) all0 = 0;
            if (probe[i] != 0xFF) allf = 0;
        }
        if (all0 || allf) {
            snprintf(err, errsz, "BIOS font data appears empty");
            return 0;
        }
    }

    menu_clear();
    menu_draw_str(100, "BIOS ROM font OK", 0);
    menu_draw_str(140, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0);
    menu_draw_str(180, "0123456789 !@#$%%", 0);
    menu_draw_str(260, "Press A / Start", 0);
    wait_button();

    region = flashrom_get_region();
    if (region < 0) {
        snprintf(err, errsz, "Flashrom region failed");
        return 0;
    }
    if (flashrom_get_syscfg(&syscfg) < 0) {
        snprintf(err, errsz, "Flashrom syscfg failed");
        return 0;
    }

    menu_clear();
    menu_draw_str(100, "Flashrom / BIOS config OK", 0);
    {
        char line[64];
        snprintf(line, sizeof(line), "Region code: %d", region);
        menu_draw_str(140, line, 0);
    }
    menu_draw_str(260, "Press A / Start", 0);
    wait_button();

    menu_clear();
    menu_draw_str(100, "Checking AICA sound path...", 0);
    menu_hold(60);
    if (snd_init() != 0) {
        snprintf(err, errsz, "snd_init() failed");
        return 0;
    }
    mem = snd_mem_available();
    if (mem == 0) {
        snd_shutdown();
        snprintf(err, errsz, "No AICA RAM reported");
        return 0;
    }
    if (snd_stream_init() != 0) {
        snd_shutdown();
        snprintf(err, errsz, "Sound stream init failed");
        return 0;
    }
    snd_stream_shutdown();
    snd_shutdown();

    menu_clear();
    menu_draw_str(100, "Checking WDT...", 0);
    menu_hold(60);
    wdt_fired = 0;
    wdt_enable_timer(0, 1000, 15, wdt_test_cb, NULL);
    thd_sleep(5);
    wdt_disable();

    if (!wdt_fired) {
        snprintf(err, errsz, "WDT callback did not fire");
        return 0;
    }

    menu_clear();
    menu_draw_str(200, "BIOS + hardware checks OK", 0);
    menu_draw_str(260, "Press A / Start", 0);
    wait_button();
    return 1;
}

static int16_t test_tone_buf[1024];

static void *test_apu_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes)
{
    int n = req_bytes / 2;
    (void)hnd;
    if (n > 1024) n = 1024;
    if (n < 0) n = 0;
    apu_generate(test_tone_buf, n);
    *got_bytes = n * 2;
    return test_tone_buf;
}

static int test_apu(char *err, size_t errsz)
{
    int16_t samples[2048];
    int i, nonzero = 0, clipped = 0, maxabs = 0;
    snd_stream_hnd_t h = -1;

    menu_clear();
    menu_draw_str(40, "Testing APU...", 0);
    menu_draw_str(80, "You may hear short tones", 0);
    menu_draw_str(120, "Press A / Start when ready", 0);
    wait_button();

    apu_reset();
    apu_write(0x4015, 0x01);
    apu_write(0x4000, 0xBF);
    apu_write(0x4001, 0x08);
    apu_write(0x4002, 0xFD);
    apu_write(0x4003, 0x08);
    apu_generate(samples, 2048);

    for (i = 0; i < 2048; i++) {
        int a = samples[i] < 0 ? -(int)samples[i] : (int)samples[i];
        if (a > 50) nonzero++;
        if (a >= 30000) clipped++;
        if (a > maxabs) maxabs = a;
    }
    if (nonzero < 100) {
        snprintf(err, errsz, "APU output was silent");
        apu_reset();
        return 0;
    }
    if (clipped > 1500) {
        snprintf(err, errsz, "APU output heavily clipped");
        apu_reset();
        return 0;
    }
    if (maxabs < 100) {
        snprintf(err, errsz, "APU output level too low");
        apu_reset();
        return 0;
    }

    if (snd_init() == 0) {
        if (snd_stream_init() == 0) {
            h = snd_stream_alloc(test_apu_cb, 4096);
            if (h >= 0) {
                menu_clear();
                menu_draw_str(200, "Playing pulse tone...", 0);
                snd_stream_start(h, NES_AUDIO_RATE, 0);
                for (i = 0; i < 90; i++) {
                    snd_stream_poll(h);
                    vid_waitvbl();
                }
                apu_reset();
                apu_write(0x4015, 0x08);
                apu_write(0x400C, 0x3F);
                apu_write(0x400E, 0x04);
                apu_write(0x400F, 0x18);
                menu_clear();
                menu_draw_str(200, "Playing noise...", 0);
                for (i = 0; i < 60; i++) {
                    snd_stream_poll(h);
                    vid_waitvbl();
                }
                snd_stream_stop(h);
                snd_stream_destroy(h);
            }
            snd_stream_shutdown();
        }
        snd_shutdown();
    }
    apu_reset();

    menu_clear();
    menu_draw_str(200, "APU OK", 0);
    menu_draw_str(260, "Press A / Start", 0);
    wait_button();
    return 1;
}

/* Rotating cube through PVR proves the 3D command path is alive. */
static int test_3d_graphics(char *err, size_t errsz)
{
    int i, frames = 180;
    float angle = 0.0f;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    int quit_flag = 0;

    menu_init_font();
    menu_clear();
    menu_draw_str(40, "Testing 3D (PVR)...", 0);
    menu_draw_str(80, "Rotating cube - A skips", 0);
    menu_hold(60);

    if (pvr_init_defaults() != 0) {
        snprintf(err, errsz, "pvr_init_defaults failed");
        return 0;
    }

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr, &cxt);

    for (i = 0; i < frames; i++) {
        float c = cosf(angle), s = sinf(angle);
        float c2 = cosf(angle * 0.7f), s2 = sinf(angle * 0.7f);
        float pts[8][3];
        static const float base[8][3] = {
            {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
            {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}
        };
        static const int faces[6][4] = {
            {0,1,2,3}, {5,4,7,6}, {4,0,3,7},
            {1,5,6,2}, {3,2,6,7}, {4,5,1,0}
        };
        static const uint32_t fcol[6] = {
            0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
            0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF
        };
        int v, f;
        pvr_vertex_t vert;

        for (v = 0; v < 8; v++) {
            float x = base[v][0], y = base[v][1], z = base[v][2];
            float x1 = x * c + z * s;
            float z1 = -x * s + z * c;
            float y2 = y * c2 - z1 * s2;
            float z2 = y * s2 + z1 * c2;
            pts[v][0] = x1 * 80.0f + 320.0f;
            pts[v][1] = y2 * 80.0f + 240.0f;
            pts[v][2] = z2 + 5.0f;
        }

        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&hdr, sizeof(hdr));

        for (f = 0; f < 6; f++) {
            int a = faces[f][0], b = faces[f][1];
            int ci = faces[f][2], d = faces[f][3];

            vert.flags = PVR_CMD_VERTEX;
            vert.x = pts[a][0]; vert.y = pts[a][1]; vert.z = pts[a][2];
            vert.u = 0; vert.v = 0; vert.argb = fcol[f]; vert.oargb = 0;
            pvr_prim(&vert, sizeof(vert));
            vert.x = pts[b][0]; vert.y = pts[b][1]; vert.z = pts[b][2];
            pvr_prim(&vert, sizeof(vert));
            vert.flags = PVR_CMD_VERTEX_EOL;
            vert.x = pts[ci][0]; vert.y = pts[ci][1]; vert.z = pts[ci][2];
            pvr_prim(&vert, sizeof(vert));

            vert.flags = PVR_CMD_VERTEX;
            vert.x = pts[a][0]; vert.y = pts[a][1]; vert.z = pts[a][2];
            pvr_prim(&vert, sizeof(vert));
            vert.x = pts[ci][0]; vert.y = pts[ci][1]; vert.z = pts[ci][2];
            pvr_prim(&vert, sizeof(vert));
            vert.flags = PVR_CMD_VERTEX_EOL;
            vert.x = pts[d][0]; vert.y = pts[d][1]; vert.z = pts[d][2];
            pvr_prim(&vert, sizeof(vert));
        }

        pvr_list_finish();
        pvr_scene_finish();
        angle += 0.04f;
        if (angle > 6.28318f) angle -= 6.28318f;

        read_pad(&quit_flag);
        if (quit_flag) break;
        {
            maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
            cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
            if (st && (st->buttons & (CONT_A | CONT_START))) {
                while (st && (st->buttons & (CONT_A | CONT_START))) {
                    vid_waitvbl();
                    st = (cont_state_t *)maple_dev_status(dev);
                }
                break;
            }
        }
    }

    pvr_shutdown();
    {
        int cable = vid_check_cable();
        int dm = (cable == CT_VGA) ? DM_640x480_VGA : DM_640x480_NTSC_IL;
        vid_set_mode(dm, PM_RGB565);
        vid_clear(0, 0, 0);
        menu_init_font();
        menu_draw_str(200, "3D test complete", 0);
        menu_draw_str(240, "Returning to menu in 5 seconds...", 0);
        menu_present();
        menu_hold(300);
    }
    return 1;
}

static void run_diagnostics(void)
{
    char err[128];
    int ok = 1;
    const char *fail_msg = NULL;

    err[0] = '\0';
    if (!test_graphics(err, sizeof(err))) {
        ok = 0; fail_msg = err; goto done;
    }
    if (!test_3d_graphics(err, sizeof(err))) {
        ok = 0; fail_msg = err; goto done;
    }
    if (!test_bios_assets(err, sizeof(err))) {
        ok = 0; fail_msg = err; goto done;
    }
    if (!test_apu(err, sizeof(err))) {
        ok = 0; fail_msg = err; goto done;
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

/* ---- cheats and main menu --------------------------------------------- */

#define MAX_CHEATS       32
#define MAX_CHEAT_LENGTH 8
#define MENU_ITEMS       4
#define CHEAT_FILE       "SMBCHEAT.VMS"
#define CHEAT_BLOCK      512            /* one VMU block: enough for all 32 codes */
#define CHEAT_MAGIC      "SMBCHEATS1\n"

static char cheat_vmu_path[40];         /* "/vmu/a1/SMBCHEAT.VMS" once a VMU is known */
static int  cheat_load_status = -1;     /* -1 not tried, 0 VMU but no file, 1 loaded, 2 file empty, 3 no VMU */
static int  cheat_save_status = -1;     /* -1 never saved, 0 failed, 1 ok */
static char saved_cheats[MAX_CHEATS][MAX_CHEAT_LENGTH + 1];
static int  saved_cheat_count;
/* Alphabetical order of the 16 valid Game Genie characters. */
static const char cheat_alphabet[] = "AEGIKLNOPSTUVXYZ";
static const char *menu_labels[MENU_ITEMS] = {
    "Start Game", "Cheats", "Test", "Quit"
};

static int cheat_code_ok(const char *s, size_t n)
{
    size_t i;
    if (n != 6 && n != 8) return 0;
    for (i = 0; i < n; ++i)
        if (!s[i] || !strchr("APZLGITYEOXUKSVN", s[i])) return 0;
    return 1;
}

/* Path of the n-th memory card, e.g. "/vmu/a1/SMBCHEAT.VMS".  Returns 0 when there is no n-th card.
   Port/unit come straight from the maple device, so any slot on any controller works. */
static int vmu_slot_path(size_t n, char *path, size_t sz)
{
    maple_device_t *dev = maple_enum_type(n, MAPLE_FUNC_MEMCARD);
    if (!dev) return 0;
    snprintf(path, sz, "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, CHEAT_FILE);
    return 1;
}

static void vmu_label(char *out, size_t sz)
{
    if (strlen(cheat_vmu_path) > 7)
        snprintf(out, sz, "VMU %c%c", cheat_vmu_path[5] - 'a' + 'A', cheat_vmu_path[6]);
    else
        snprintf(out, sz, "VMU");
}

/* Read the cheat file.  Returns the number of codes added, or -1 if the file is missing/unreadable.
   The KOS VMU filesystem strips the BIOS header for us, so we simply read the first block. */
static int load_cheats_from_path(const char *path)
{
    char buf[CHEAT_BLOCK + 1];
    const size_t mlen = sizeof(CHEAT_MAGIC) - 1;
    size_t pos;
    ssize_t got;
    file_t fd;
    int added = 0;

    fd = fs_open(path, O_RDONLY);
    if (fd == FILEHND_INVALID) return -1;
    memset(buf, 0, sizeof(buf));
    got = fs_read(fd, buf, CHEAT_BLOCK);
    fs_close(fd);
    if (got < (ssize_t)mlen || memcmp(buf, CHEAT_MAGIC, mlen) != 0) return -1;

    pos = mlen;
    while (pos < CHEAT_BLOCK && buf[pos] && saved_cheat_count < MAX_CHEATS) {
        char code[MAX_CHEAT_LENGTH + 1];
        size_t n = 0;
        while (pos < CHEAT_BLOCK && buf[pos] && buf[pos] != '\n') {
            if (n < MAX_CHEAT_LENGTH) code[n] = buf[pos];
            n++; pos++;
        }
        if (pos < CHEAT_BLOCK && buf[pos] == '\n') pos++;
        if (n <= MAX_CHEAT_LENGTH) {            /* the last code is added too: it is followed by zero padding */
            code[n] = '\0';
            if (cheat_code_ok(code, n)) {
                memcpy(saved_cheats[saved_cheat_count], code, n + 1);
                saved_cheat_count++;
                added++;
            }
        }
    }
    return added;
}

/* Look for a cheat file on every memory card.  The maple bus enumerates VMUs in the background after boot,
   so wait for that scan first - reading too early is what made loading look like it did nothing. */
static void load_cheats_from_vmu(void)
{
    char path[40], first[40] = "", empty[40] = "";
    size_t slot;

    maple_wait_scan();
    saved_cheat_count = 0;
    cheat_vmu_path[0] = '\0';
    cheat_load_status = 3;
    for (slot = 0; vmu_slot_path(slot, path, sizeof(path)); ++slot) {
        int r;
        if (!first[0]) strcpy(first, path);
        r = load_cheats_from_path(path);
        if (r > 0) {
            strcpy(cheat_vmu_path, path);
            cheat_load_status = 1;
            return;
        }
        if (r == 0 && !empty[0]) strcpy(empty, path);
    }
    if (empty[0]) {
        strcpy(cheat_vmu_path, empty);
        cheat_load_status = 2;
    } else if (first[0]) {
        strcpy(cheat_vmu_path, first);          /* first VMU found, no cheat file yet: save here */
        cheat_load_status = 0;
    }
}

/* Write the list to the VMU.  The BIOS header must be attached to the SAME open file before it is closed;
   the filesystem then writes header + data when the file is closed. */
static int save_cheats_to_vmu(void)
{
    char payload[CHEAT_BLOCK];
    size_t used = sizeof(CHEAT_MAGIC) - 1;
    vmu_pkg_t pkg;
    file_t fd;
    int i, ok;

    if (!cheat_vmu_path[0] &&
        !vmu_slot_path(0, cheat_vmu_path, sizeof(cheat_vmu_path)))
        return 0;                               /* no memory card in any slot */

    memset(payload, 0, sizeof(payload));
    memcpy(payload, CHEAT_MAGIC, used);
    for (i = 0; i < saved_cheat_count; ++i) {
        size_t n = strlen(saved_cheats[i]);
        if (used + n + 1 > sizeof(payload)) return 0;
        memcpy(payload + used, saved_cheats[i], n);
        used += n;
        payload[used++] = '\n';
    }

    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "SMB Cheats", sizeof(pkg.desc_short) - 1);
    strncpy(pkg.desc_long, "Super Mario Bros Game Genie Codes", sizeof(pkg.desc_long) - 1);
    strncpy(pkg.app_id, "SMB-DREAMCAST", sizeof(pkg.app_id) - 1);
    pkg.icon_cnt = 0;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;

    fd = fs_open(cheat_vmu_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd == FILEHND_INVALID) return 0;
    ok = (fs_vmu_set_header(fd, &pkg) == 0);
    if (ok && fs_write(fd, payload, sizeof(payload)) != (ssize_t)sizeof(payload)) ok = 0;
    if (fs_close(fd) != 0) ok = 0;
    return ok;
}

/* Activate only after nes_load() and nes_reset(), which initialize the core. */
static void apply_saved_cheats(void)
{
    int i;
    nes_game_genie_clear();
    for (i = 0; i < saved_cheat_count; ++i)
        (void)nes_game_genie_add(saved_cheats[i]);   /* a rejected code stays in the saved list */
}

/* Swallow buttons still held from the previous screen, so the press that opened a menu
   is not read again as a press inside it. */
static void wait_all_released(void)
{
    int quit_flag = 0, i;
    for (i = 0; i < 180 && read_pad(&quit_flag); i++)
        vid_waitvbl();
}

static void show_message(const char *line1, const char *line2)
{
    menu_init_font();
    menu_clear();
    menu_draw_str(150, line1, 0);
    if (line2) menu_draw_str(200, line2, 0);
    menu_draw_str(300, "Press A / Start", 0);
    menu_present();
    wait_button();
}

static void add_cheat_code(void)
{
    char code[MAX_CHEAT_LENGTH + 1], msg[64] = "";
    int len = 0, letter = 0, quit_flag = 0, dirty = 1, debounce = 0;
    uint8_t prev = 0;
    const int alphabet_len = (int)strlen(cheat_alphabet);

    code[0] = '\0';
    menu_init_font();
    wait_all_released();
    for (;;) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return;
        if (debounce > 0) {
            debounce--;
        } else {
            if (((pad & PAD_UP) && !(prev & PAD_UP)) || ((pad & PAD_LEFT) && !(prev & PAD_LEFT))) {
                letter = (letter + alphabet_len - 1) % alphabet_len;
                debounce = 7; dirty = 1; msg[0] = '\0';
            } else if (((pad & PAD_DOWN) && !(prev & PAD_DOWN)) || ((pad & PAD_RIGHT) && !(prev & PAD_RIGHT))) {
                letter = (letter + 1) % alphabet_len;
                debounce = 7; dirty = 1; msg[0] = '\0';
            } else if ((pad & PAD_A) && !(prev & PAD_A)) {
                if (len < MAX_CHEAT_LENGTH) {
                    code[len++] = cheat_alphabet[letter];
                    code[len] = '\0';
                    msg[0] = '\0';
                } else {
                    snprintf(msg, sizeof(msg), "Codes are 8 letters at most");
                }
                debounce = 7; dirty = 1;
            } else if ((pad & PAD_B) && !(prev & PAD_B)) {
                if (len > 0) { code[--len] = '\0'; msg[0] = '\0'; debounce = 7; dirty = 1; }
                else return;
            } else if ((pad & PAD_START) && !(prev & PAD_START)) {
                int i, dup = 0;
                for (i = 0; i < saved_cheat_count; ++i)
                    if (strcmp(saved_cheats[i], code) == 0) dup = 1;
                if (!cheat_code_ok(code, (size_t)len)) {
                    snprintf(msg, sizeof(msg), "Need 6 or 8 letters (you have %d)", len);
                } else if (dup) {
                    snprintf(msg, sizeof(msg), "That code is already saved");
                } else if (saved_cheat_count >= MAX_CHEATS) {
                    snprintf(msg, sizeof(msg), "Cheat list is full (%d codes)", MAX_CHEATS);
                } else {
                    char where[16], line[64];
                    memcpy(saved_cheats[saved_cheat_count], code, (size_t)len + 1);
                    saved_cheat_count++;
                    cheat_save_status = save_cheats_to_vmu();
                    vmu_label(where, sizeof(where));
                    if (cheat_save_status) {
                        snprintf(line, sizeof(line), "Saved to %s", where);
                        show_message(line, "Active the next time you Start Game");
                    } else {
                        show_message("VMU save FAILED - is a VMU inserted?",
                                     "Code kept until you power off");
                    }
                    return;
                }
                debounce = 7; dirty = 1;
            }
        }
        prev = pad;
        if (dirty) {
            char line[64];
            menu_clear();
            menu_draw_str(70, "Feeling a little evil, eh?", 0);
            menu_draw_str(130, "Add Game Genie Code", 0);
            snprintf(line, sizeof(line), "Code: %-8s", code);
            menu_draw_str(190, line, 0);
            snprintf(line, sizeof(line), "Letter: [%c]", cheat_alphabet[letter]);
            menu_draw_str(240, line, 1);
            menu_draw_str(300, "D-pad = choose letter", 0);
            menu_draw_str(330, "A = add letter   B = delete / back", 0);
            menu_draw_str(360, "Start = save (6 or 8 letters)", 0);
            if (msg[0]) menu_draw_str(410, msg, 0);
            menu_present();
            dirty = 0;
        } else {
            vid_waitvbl();
        }
    }
}

static void list_cheat_codes(void)
{
    int top = 0, i, quit_flag = 0, debounce = 0;
    uint8_t prev = 0;
    char where[16], line[64];

    wait_all_released();
    vmu_label(where, sizeof(where));
    for (;;) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return;
        if (((pad & PAD_B) && !(prev & PAD_B)) ||
            ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START))))
            return;
        if (debounce > 0) debounce--;
        else if ((pad & PAD_UP) && !(prev & PAD_UP)) {
            if (top > 0) top--;
            debounce = 8;
        } else if ((pad & PAD_DOWN) && !(prev & PAD_DOWN)) {
            if (top + 6 < saved_cheat_count) top++;
            debounce = 8;
        }
        prev = pad;
        menu_clear();
        menu_draw_str(60, "Saved Game Genie Codes", 0);
        if (saved_cheat_count == 0) {
            menu_draw_str(150, "No codes saved yet", 0);
        } else {
            for (i = 0; i < 6 && top + i < saved_cheat_count; i++) {
                snprintf(line, sizeof(line), "%02d: %s", top + i + 1, saved_cheats[top + i]);
                menu_draw_str(120 + i * 35, line, 0);
            }
        }
        switch (cheat_load_status) {
        case 1:  snprintf(line, sizeof(line), "Loaded %d code(s) from %s", saved_cheat_count, where); break;
        case 2:  snprintf(line, sizeof(line), "Cheat file on %s is empty", where); break;
        case 0:  snprintf(line, sizeof(line), "No cheat file on %s yet", where); break;
        case 3:  snprintf(line, sizeof(line), "No VMU found"); break;
        default: snprintf(line, sizeof(line), "VMU not checked"); break;
        }
        menu_draw_str(330, line, 0);
        menu_draw_str(355, cheat_save_status == 1 ? "Last save succeeded" :
                           (cheat_save_status == 0 ? "Last VMU save FAILED" : "Nothing saved this session"), 0);
        menu_draw_str(385, "A / B = return", 0);
        menu_present();
    }
}

static void run_cheats_menu(void)
{
    int sel = 0, quit_flag = 0, debounce = 0, dirty = 1;
    uint8_t prev = 0;
    const char *labels[] = { "Add Cheat Code", "List Cheat Codes", "Back" };

    menu_init_font();
    wait_all_released();
    /* A VMU may have been inserted since boot; only re-scan if nothing is loaded and nothing is pending. */
    if (cheat_load_status != 1 && saved_cheat_count == 0)
        load_cheats_from_vmu();
    for (;;) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return;
        if (debounce > 0) debounce--;
        else {
            if ((pad & PAD_UP) && !(prev & PAD_UP)) {
                sel = (sel + 2) % 3; debounce = 10; dirty = 1;
            } else if ((pad & PAD_DOWN) && !(prev & PAD_DOWN)) {
                sel = (sel + 1) % 3; debounce = 10; dirty = 1;
            } else if ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START))) {
                if (sel == 0) add_cheat_code();
                else if (sel == 1) list_cheat_codes();
                else return;
                wait_all_released();
                prev = 0; debounce = 10; dirty = 1;
                continue;
            } else if ((pad & PAD_B) && !(prev & PAD_B)) {
                return;
            }
        }
        prev = pad;
        if (dirty) {
            menu_clear();
            menu_draw_str(90, "Feeling a little evil, eh?", 0);
            menu_draw_str(170, labels[0], sel == 0);
            menu_draw_str(220, labels[1], sel == 1);
            menu_draw_str(270, labels[2], sel == 2);
            menu_draw_str(350, "A / Start = Select   B = Back", 0);
            menu_present();
            dirty = 0;
        } else {
            vid_waitvbl();
        }
    }
}

static int run_main_menu(void)
{
    int sel = 0;
    int quit_flag = 0;
    uint8_t prev = 0;
    int debounce = 0;
    int dirty = 1;

    menu_init_font();
    for (;;) {
        uint8_t pad = read_pad(&quit_flag);
        if (quit_flag) return 0;

        if (debounce > 0) {
            debounce--;
        } else {
            if ((pad & PAD_UP) && !(prev & PAD_UP)) {
                sel = (sel + MENU_ITEMS - 1) % MENU_ITEMS;
                debounce = 12; dirty = 1;
            }
            if ((pad & PAD_DOWN) && !(prev & PAD_DOWN)) {
                sel = (sel + 1) % MENU_ITEMS;
                debounce = 12; dirty = 1;
            }
            if ((pad & (PAD_A | PAD_START)) && !(prev & (PAD_A | PAD_START))) {
                if (sel == 0) return 1;
                if (sel == 1) run_cheats_menu();
                if (sel == 3) return 0;
                if (sel == 2 && confirm_yes_no("Are you sure you wanna test?",
                                               "It might look or sound creepy."))
                    run_diagnostics();
                prev = 0;
                debounce = 12;
                dirty = 1;
            }
        }
        prev = pad;

        if (dirty) {
            vid_waitvbl();
            menu_clear();
            menu_draw_str(100, "Welcome to SMB for Dreamcast", 0);
            menu_draw_str(140, "A / Start = Select", 0);
            menu_draw_str(210, menu_labels[0], sel == 0);
            menu_draw_str(250, menu_labels[1], sel == 1);
            menu_draw_str(290, menu_labels[2], sel == 2);
            menu_draw_str(330, menu_labels[3], sel == 3);
            menu_present();
            dirty = 0;
        } else {
            vid_waitvbl();
        }
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

    (void)argc;
    (void)argv;

    /* Reset video state after returning from the official BIOS menu. */
    {
        int i;
        int cable = vid_check_cable();
        int dm = (cable == CT_VGA) ? DM_640x480_VGA : DM_640x480_NTSC_IL;
        vid_set_mode(dm, PM_RGB565);
        vid_empty();
        vid_set_mode(dm, PM_RGB565);
        vid_border_color(0, 0, 0);
        vid_clear(0, 0, 0);
        menu_init_font();
        for (i = 0; i < 8; i++) vid_waitvbl();
    }

    /* Load saved Game Genie codes before the menu/game starts. */
    load_cheats_from_vmu();
    if (!run_main_menu()) return 0;

    rom = load_file("/rd/smb.nes", &rom_size);
    if (!rom) {
        fatal("smb.nes missing from romdisk");
        return 1;
    }
    if (nes_load(rom, rom_size) != 0) {
        free(rom);
        fatal("smb.nes is not a valid NROM image");
        return 1;
    }
    nes_reset();
    apply_saved_cheats();

    if (pvr_init_defaults() != 0) {
        free(rom);
        fatal("PVR initialization failed");
        return 1;
    }
    txr = pvr_mem_malloc(TXR_BYTES);
    if (!txr) {
        pvr_shutdown();
        free(rom);
        fatal("PVR texture allocation failed");
        return 1;
    }

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     256, 256, txr,
                     INTEGER_SCALE ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
    pvr_poly_compile(&hdr, &cxt);

    {
        float x0 = INTEGER_SCALE ? 64.0f : 0.0f;
        float x1 = INTEGER_SCALE ? 576.0f : 640.0f;
        set_vertex(&vert[0], x0,   0.0f, 0.0f, 0.0f,          PVR_CMD_VERTEX);
        set_vertex(&vert[1], x1,   0.0f, 1.0f, 0.0f,          PVR_CMD_VERTEX);
        set_vertex(&vert[2], x0, 480.0f, 0.0f, 240.0f/256.0f, PVR_CMD_VERTEX);
        set_vertex(&vert[3], x1, 480.0f, 1.0f, 240.0f/256.0f, PVR_CMD_VERTEX_EOL);
    }

    if (snd_stream_init() != 0) {
        pvr_mem_free(txr);
        pvr_shutdown();
        free(rom);
        fatal("Audio stream initialization failed");
        return 1;
    }
    stream = snd_stream_alloc(audio_cb, AUDIO_BUF_BYTES);
    if (stream < 0) {
        snd_stream_shutdown();
        pvr_mem_free(txr);
        pvr_shutdown();
        free(rom);
        fatal("Audio stream allocation failed");
        return 1;
    }
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

/* I'M TIRED OF THIS GRANDPA! WELL THAT'S TOO DAMN BAD!!*/
