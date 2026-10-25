/*
 * core.c -- the C64 itself: VICE's x64sc as a libretro core (vice_x64sc_libretro.so, built from libretro/vice-libretro),
 * with this file as its frontend. Worked out in ../spike first (2026-10-01).
 *
 * A libretro core runs one frame per retro_run() and hands back its picture, sound and questions through callbacks:
 *   video     a 384x272 PAL frame, borders included, in whatever pixel format the core asked for; kept here as RGB24
 *   audio     S16 stereo at the core's own rate, kept in a ring for the sound side to take
 *   input     the joystick (RetroPad port 0, which the core puts in the C64's port 2 unless told otherwise) is polled;
 *             keys are pushed, through the keyboard callback the core registers
 *   options   every option is answered with the core's own declared default unless set below. 🚨 Answering "no value"
 *             instead left the core on internal fallbacks and every frame came out black (spike, 2026-10-01).
 *   disks     the disk-control interface swaps images from the .m3u list the core was started with
 */

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "libretro.h"

static void *g_lib;
static const char *g_sysdir;
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t g_rgb[CORE_H * CORE_W * 3];
static unsigned g_pad;
static retro_keyboard_event_t g_key_cb;
static struct retro_disk_control_ext_callback g_disk;
static bool g_have_disk;
static struct retro_system_av_info g_av;
static char g_joyport[2] = "2";
static bool g_options_changed;
static bool g_drive_led;                /* the 1541's red light, as the core reports it */

/* The core's functions */
static void (*p_set_environment)(retro_environment_t);
static void (*p_set_video_refresh)(retro_video_refresh_t);
static void (*p_set_audio_sample)(retro_audio_sample_t);
static void (*p_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*p_set_input_poll)(retro_input_poll_t);
static void (*p_set_input_state)(retro_input_state_t);
static void (*p_set_controller_port_device)(unsigned, unsigned);
static void (*p_init)(void);
static void (*p_deinit)(void);
static bool (*p_load_game)(const struct retro_game_info *);
static void (*p_unload_game)(void);
static void (*p_get_system_av_info)(struct retro_system_av_info *);
static void (*p_run)(void);
static void (*p_reset)(void);

/* ---- options ---- */

#define MAX_OPTS 512
static const char *g_opt_key[MAX_OPTS], *g_opt_def[MAX_OPTS];
static int g_nopts;

static const char *door_option(const char *key)
{
    /* What the door needs, whatever the core's defaults say */
    static const char *const opts[][2] = {
        { "vice_c64_model", "C64 PAL" },
        { "vice_drive_true_emulation", "enabled" },     /* the real 1541: fastloaders and copy protection work */
        { "vice_autostart", "enabled" },
        { "vice_crop", "disabled" },                    /* the door crops, so it always gets the same 384x272 */
        { "vice_sid_engine", "ReSID" },
        { "vice_drive_sound_emulation", "disabled" },
        { "vice_keyboard_keymap", "symbolic" },         /* typing " gives " (suits BASIC) */
        { "vice_statusbar_startup", "disabled" },
        { "vice_read_vicerc", "disabled" },             /* no config files from anywhere */
        { "vice_work_disk", "disabled" },
        { "vice_easyflash_write_protection", "enabled" },  /* else the core rewrites an EasyFlash .crt on the way out,
                                                         * as a headerless .bin the door can't boot again */
        { "vice_jiffydos", "disabled" },                /* loading is sped up by warping instead (play.c), and some
                                                         * games' own fast loaders hang with its drive ROM */
    };
    if (strcmp(key, "vice_joyport") == 0)
        return g_joyport;
    if (strcmp(key, "vice_autostart") == 0 && getenv("C64DOOR_NO_AUTOSTART") != NULL)
        return "disabled";                              /* the caller's own disk: they LOAD"$",8 and choose */
    for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++)
        if (strcmp(opts[i][0], key) == 0)
            return opts[i][1];
    return NULL;
}

static void add_default(const char *key, const char *def)
{
    if (key && g_nopts < MAX_OPTS)
    {
        g_opt_key[g_nopts] = key;
        g_opt_def[g_nopts++] = def;
    }
}

static const char *default_of(const char *key)
{
    for (int i = 0; i < g_nopts; i++)
        if (strcmp(g_opt_key[i], key) == 0)
            return g_opt_def[i];
    return NULL;
}

/* ---- callbacks ---- */

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    const char *path = getenv("C64DOOR_CORE_LOG");
    FILE *f;

    if (level < RETRO_LOG_WARN || path == NULL || (f = fopen(path, "a")) == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static void led_cb(int led, int state)
{
    if (led == 1)                       /* 0 power, 1 floppy, 2 tape */
        g_drive_led = state != 0;
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_sysdir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        /* the core keeps temporary files here: the session's own folder (C64DOOR_SAVE_DIR), not the read-only door */
        *(const char **)data = getenv("C64DOOR_SAVE_DIR") != NULL ? getenv("C64DOOR_SAVE_DIR") : g_sysdir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_fmt = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    {
        const struct retro_core_options_v2 *o = cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 ? data
            : ((const struct retro_core_options_v2_intl *)data)->us;
        for (const struct retro_core_option_v2_definition *d = o->definitions; d && d->key; d++)
            add_default(d->key, d->default_value ? d->default_value : d->values[0].value);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        for (const struct retro_core_option_definition *d = data; d && d->key; d++)
            add_default(d->key, d->default_value ? d->default_value : d->values[0].value);
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *v = data;
        v->value = door_option(v->key);
        if (v->value == NULL)
            v->value = default_of(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = g_options_changed;
        g_options_changed = false;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        g_key_cb = ((const struct retro_keyboard_callback *)data)->callback;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        g_disk = *(const struct retro_disk_control_ext_callback *)data;
        g_have_disk = true;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        memset(&g_disk, 0, sizeof(g_disk));
        memcpy(&g_disk, data, sizeof(struct retro_disk_control_callback));
        g_have_disk = true;
        return true;
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        ((struct retro_led_interface *)data)->set_led_state = led_cb;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        g_av = *(const struct retro_system_av_info *)data;
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (data == NULL)
        return;                                 /* the same frame again */
    if (w > CORE_W)
        w = CORE_W;
    if (h > CORE_H)
        h = CORE_H;
    for (unsigned y = 0; y < h; y++)
    {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        uint8_t *d = g_rgb + y * CORE_W * 3;
        for (unsigned x = 0; x < w; x++, d += 3)
        {
            if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888)
            {
                uint32_t p = ((const uint32_t *)row)[x];
                d[0] = (uint8_t)(p >> 16);
                d[1] = (uint8_t)(p >> 8);
                d[2] = (uint8_t)p;
            }
            else
            {
                uint16_t p = ((const uint16_t *)row)[x];
                if (g_fmt == RETRO_PIXEL_FORMAT_RGB565)
                {
                    d[0] = (uint8_t)(((p >> 11) & 31) << 3);
                    d[1] = (uint8_t)(((p >> 5) & 63) << 2);
                }
                else
                {
                    d[0] = (uint8_t)(((p >> 10) & 31) << 3);
                    d[1] = (uint8_t)(((p >> 5) & 31) << 3);
                }
                d[2] = (uint8_t)((p & 31) << 3);
            }
        }
    }
}

/* Sound: a ring of stereo frames, about 1.3 s, far more than one frame's worth ever waits */
#define RING 65536
static int16_t g_ring[RING * 2];
static unsigned g_ring_w, g_ring_r;
static int g_frame_peak;                        /* the loudest sample of the last frame */

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames; i++)
    {
        if (g_ring_w - g_ring_r >= RING)
            g_ring_r++;                         /* nobody is taking it: the oldest goes */
        int l = abs(data[i * 2]), r = abs(data[i * 2 + 1]);
        if (l > g_frame_peak) g_frame_peak = l;
        if (r > g_frame_peak) g_frame_peak = r;
        g_ring[(g_ring_w % RING) * 2] = data[i * 2];
        g_ring[(g_ring_w % RING) * 2 + 1] = data[i * 2 + 1];
        g_ring_w++;
    }
    return frames;
}

static void audio_cb(int16_t l, int16_t r)
{
    int16_t f[2] = { l, r };
    audio_batch_cb(f, 1);
}

static void poll_cb(void)
{
}

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)g_pad;
    return (int16_t)((g_pad >> id) & 1);
}

/* ---- the door's side ---- */

#define SYM(p, name) do { if (!(p = (__typeof__(p))dlsym(g_lib, name))) { *why = "the C64 core is incomplete"; return false; } } while (0)

bool core_start(const char *core_path, const char *system_dir, const char *m3u, const char **why)
{
    struct retro_game_info game = { 0 };

    g_sysdir = system_dir;
    if ((g_lib = dlopen(core_path, RTLD_NOW | RTLD_LOCAL)) == NULL)
    {
        *why = "the C64 core (vice_x64sc_libretro.so) wouldn't load";
        return false;
    }
    SYM(p_set_environment, "retro_set_environment");
    SYM(p_set_video_refresh, "retro_set_video_refresh");
    SYM(p_set_audio_sample, "retro_set_audio_sample");
    SYM(p_set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(p_set_input_poll, "retro_set_input_poll");
    SYM(p_set_input_state, "retro_set_input_state");
    SYM(p_set_controller_port_device, "retro_set_controller_port_device");
    SYM(p_init, "retro_init");
    SYM(p_deinit, "retro_deinit");
    SYM(p_load_game, "retro_load_game");
    SYM(p_unload_game, "retro_unload_game");
    SYM(p_get_system_av_info, "retro_get_system_av_info");
    SYM(p_run, "retro_run");
    SYM(p_reset, "retro_reset");

    p_set_environment(env_cb);
    p_init();
    p_set_video_refresh(video_cb);
    p_set_audio_sample(audio_cb);
    p_set_audio_sample_batch(audio_batch_cb);
    p_set_input_poll(poll_cb);
    p_set_input_state(input_cb);
    game.path = m3u;
    if (!p_load_game(m3u != NULL ? &game : NULL))
    {
        *why = "the C64 wouldn't start with that disk";
        return false;
    }
    p_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    p_get_system_av_info(&g_av);
    return true;
}

void core_run_frame(void)
{
    g_frame_peak = 0;
    p_run();
}

bool core_frame_silent(void)
{
    return g_frame_peak < 64;
}

void core_audio_drop(void)
{
    g_ring_r = g_ring_w;
}

const uint8_t *core_rgb(void)
{
    return g_rgb;
}

double core_fps(void)
{
    return g_av.timing.fps > 1 ? g_av.timing.fps : 50.0;
}

double core_sample_rate(void)
{
    return g_av.timing.sample_rate > 1000 ? g_av.timing.sample_rate : 48000.0;
}

int core_audio(int16_t *out, int max)
{
    int n = 0;
    while (n < max && g_ring_r != g_ring_w)
    {
        out[n * 2] = g_ring[(g_ring_r % RING) * 2];
        out[n * 2 + 1] = g_ring[(g_ring_r % RING) * 2 + 1];
        g_ring_r++;
        n++;
    }
    return n;
}

void core_key(unsigned retrok, bool down)
{
    if (g_key_cb != NULL)
        g_key_cb(down, retrok, 0, 0);
}

void core_joystick(unsigned bits)
{
    g_pad = bits;
}

void core_joystick_port(int port)
{
    g_joyport[0] = port == 1 ? '1' : '2';
    g_options_changed = true;
}

int core_disk_count(void)
{
    return g_have_disk && g_disk.get_num_images ? (int)g_disk.get_num_images() : 0;
}

int core_disk_index(void)
{
    return g_have_disk && g_disk.get_image_index ? (int)g_disk.get_image_index() : 0;
}

void core_disk_insert(int index)
{
    if (!g_have_disk || index < 0 || index >= core_disk_count())
        return;
    g_disk.set_eject_state(true);
    g_disk.set_image_index((unsigned)index);
    g_disk.set_eject_state(false);
}

bool core_drive_led(void)
{
    return g_drive_led;
}

void core_reset(void)
{
    p_reset();
}

void core_stop(void)
{
    if (g_lib == NULL)
        return;
    p_unload_game();
    p_deinit();
    /* Not dlclose'd: the core's threads and atexit handlers may still point into it, and the process ends soon after */
}
