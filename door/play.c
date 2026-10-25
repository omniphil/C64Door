/*
 * play.c -- a C64 session on a CTerm terminal (SyncTERM): the C64 runs here, the caller sees and hears it.
 *
 * Built on the Doom door's JPEG XL mode (Doom/door/pix_play.c) and spike A (../spike, 2026-10-01):
 *
 *   Pictures   Only what changed is sent: 32x8 tiles, the whole picture when more than half changed, otherwise each
 *              run of changed tiles merged downwards into rectangles. Always LOSSLESS: on C64 art lossy JPEG XL is
 *              bigger as well as blurrier (spike A: 137 KB/s lossless at 50 fps on the heaviest demo, 525 at distance 1).
 *              On SyncTERM's usual 640x400 screen the 320x200 screen goes at 2x and the borders are cut; a screen big
 *              enough for 384x272 at 2x gets the borders too.
 *   Pacing     As Doom: a cursor-position request after each frame, as many frames on their way as the link's speed
 *              times its round trip allows. A frame that finds no room is skipped, never queued; the C64 itself always
 *              runs at its own 50 frames a second.
 *   Sound      SID sound can't be uploaded ahead like Doom's effects: it only exists as the game makes it. So it goes
 *              in 200 ms pieces of 8-bit mono WAV at 22050 Hz (about 30 KB/s with the base64), each cached, loaded
 *              into a slot and queued on one channel behind the last. UNPROVEN: whether CTerm joins pieces this short
 *              without gaps, and how the two clocks drift. The log (setup->log) has the numbers to debug it with.
 *   Keys       Real presses and releases (CSI = 1 h) where the terminal reports them, mapped to the C64's keyboard,
 *              with the arrows and fire keys as the joystick while joystick mode is on. Otherwise typed characters
 *              are turned into key taps.
 *   F12        (or Ctrl-Q, for keyboards whose F12 needs Fn) pauses the C64 and shows the session menu: load a disk
 *              from the library, upload one by ZMODEM (kept for the caller's next visit), their kept disks, next
 *              disk, joystick port, sound, reset, back to the BBS.
 *              Loading a disk or resetting ends this session with PLAY_RELOAD, and the door starts
 *              the C64 again (a reset with autostart off, so it stops at the BASIC prompt).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "apc.h"
#include "core.h"
#include "jxl_enc.h"
#include "libretro.h"
#include "play.h"
#include "session.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define TW 32
#define TH 8
#define MAX_TX (CORE_W / TW)
#define MAX_TY (CORE_H / TH)

#define MAX_IN_FLIGHT    16
#define MIN_WINDOW       4
#define QUEUE_ALLOWED_MS 50
#define STALL_MS         3000
#define FULL_FRACTION    0.5

#define SND_RATE         22050
#define SND_PIECE        (SND_RATE / 5)         /* 200 ms */
#define SND_PREBUFFER    2                      /* pieces held back before the first is queued */
#define SND_CH           2
#define SND_SLOT         250
#define SND_FILE         "c64door/snd.wav"

#define TAP_DOWN_FRAMES  3                      /* a typed key is held this long, then let go this long */
#define TAP_UP_FRAMES    2
#define TYPED_HOLD_MS    180                    /* a typed arrow holds the joystick this long after its last repeat */

static const play_caps_t *g_caps;
static const char *g_session_dir;      /* the caller's session folder: drive.txt, uploads */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool write_all(const char *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
            {
                struct timeval tv = { 0, 10000 };
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(STDOUT_FILENO, &fds);
                select(STDOUT_FILENO + 1, NULL, &fds, NULL, &tv);
                continue;
            }
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* ---- what's on its way (as pix_play.c) ---- */

static struct
{
    long   sent[MAX_IN_FLIGHT];
    size_t bytes[MAX_IN_FLIGHT];
    size_t total;
    int    head, count;
} g_fly;

static long   g_rtt_sum, g_rtt_n;
static long   g_rtt_floor[10];
static long   g_rtt_floor_sec = -1;
static size_t g_acked;
static double g_rate;                   /* the link's speed as measured, bytes a millisecond */
static double g_frame_avg = 4000;
static int    g_orphans;

static void fly_push(long now, size_t bytes)
{
    int i = (g_fly.head + g_fly.count) % MAX_IN_FLIGHT;
    g_fly.sent[i] = now;
    g_fly.bytes[i] = bytes;
    g_fly.total += bytes;
    g_fly.count++;
}

static void fly_reset(void)
{
    g_orphans += g_fly.count;
    g_fly.count = 0;
    g_fly.total = 0;
}

static long rtt_min(void)
{
    long m = 0;
    for (int i = 0; i < 10; i++)
        if (g_rtt_floor[i] > 0 && (m == 0 || g_rtt_floor[i] < m))
            m = g_rtt_floor[i];
    return m;
}

static void fly_ack(long now)
{
    long rtt, sec = now / 1000;
    if (g_orphans > 0)
    {
        g_orphans--;
        return;
    }
    if (g_fly.count == 0)
        return;
    rtt = now - g_fly.sent[g_fly.head];
    g_acked += g_fly.bytes[g_fly.head];
    g_fly.total -= g_fly.bytes[g_fly.head];
    g_fly.head = (g_fly.head + 1) % MAX_IN_FLIGHT;
    g_fly.count--;
    g_rtt_sum += rtt;
    g_rtt_n++;
    if (sec != g_rtt_floor_sec)
    {
        g_rtt_floor_sec = sec;
        g_rtt_floor[sec % 10] = rtt;
    }
    else if (rtt < g_rtt_floor[sec % 10])
        g_rtt_floor[sec % 10] = rtt;
}

static bool window_open(void)
{
    double window = g_rate * (double)(rtt_min() + QUEUE_ALLOWED_MS);
    if (window < MIN_WINDOW * g_frame_avg)
        window = MIN_WINDOW * g_frame_avg;
    return g_fly.count < MAX_IN_FLIGHT - 1 && (double)g_fly.total < window;
}

/* ---- the picture ---- */

static int     g_vx, g_vy, g_vw, g_vh;  /* the part of the C64's frame that is shown */
static int     g_tx, g_ty;              /* its tiles */
static uint8_t g_view[CORE_H * CORE_W * 3];
static uint8_t g_ref[CORE_H * CORE_W * 3];
static bool    g_have_ref;
static uint8_t g_scaled[CORE_W * 2 * CORE_H * 2 * 3];
static int     g_zoom, g_ox, g_oy;
/* Stretched to fill the screen above the controls row by any ratio up to 2x (pre-scaled here), when a whole-number
 * zoom would leave it small: an 80x24 or 80x25 SyncTERM (sysop 2026-10-05: "the C64 take up the whole screen except
 * a text menu at the bottom"). g_dw x g_dh is its size on screen. */
static bool    g_fill;
static int     g_dw, g_dh;
static int     g_bar_row, g_cell_h; /* the controls' text row (1-based) and a row's height in pixels */

static void choose_view(void)
{
    int zf = 0, aw, ah;

    /* The last text row holds the controls; the picture gets the rest */
    g_bar_row = g_caps->rows > 0 && g_caps->px_h / g_caps->rows >= 6 && g_caps->px_h / g_caps->rows <= 40
                ? g_caps->rows : g_caps->px_h / 16;     /* a row count that makes no sense: 16-pixel rows */
    g_cell_h = g_bar_row > 0 ? g_caps->px_h / g_bar_row : 16;
    aw = g_caps->px_w;
    ah = g_caps->px_h - g_cell_h;

    /* The whole frame when there's room for it at 2x or more, otherwise the 320x200 screen */
    if (aw >= CORE_W * 2 && ah >= CORE_H * 2)
    {
        g_vx = 0, g_vy = 0, g_vw = CORE_W, g_vh = CORE_H;
        zf = aw / CORE_W < ah / CORE_H ? aw / CORE_W : ah / CORE_H;
    }
    else
    {
        g_vx = CORE_SCREEN_X, g_vy = CORE_SCREEN_Y, g_vw = 320, g_vh = 200;
        zf = aw / 320 < ah / 200 ? aw / 320 : ah / 200;
    }
    g_tx = g_vw / TW;
    g_ty = g_vh / TH;
    g_fill = zf < 2;
    if (g_fill)
    {
        g_zoom = 1;
        g_dw = aw < g_vw * 2 ? aw : g_vw * 2;
        g_dh = ah < g_vh * 2 ? ah : g_vh * 2;
    }
    else
    {
        g_zoom = zf;
        if (g_zoom > 2 && !g_caps->zoom)
            g_zoom = 2;                 /* sending them pre-scaled beyond 2x costs too much */
        g_dw = g_vw * g_zoom;
        g_dh = g_vh * g_zoom;
    }
    g_ox = (g_caps->px_w - g_dw) / 2;
    g_oy = (ah - g_dh) / 2;
    if (g_ox < 0) g_ox = 0;
    if (g_oy < 0) g_oy = 0;
}

/* The controls, on the screen's last row under the picture: drawn after every clear */
static void draw_bar(outbuf_t *o)
{
    char line[256];
    const char *fire = g_caps->keys ? "R-Ctrl/Alt" : "Space";   /* the line stays within 74 columns */
    int n;

    if (g_bar_row < 1)
        return;
    n = snprintf(line, sizeof(line),
                 "\033[%d;1H\033[0;44m \033[1;37mF12\033[0;36;44m/\033[1;37mCtrl-Q\033[0;36;44m Menu & leave "
                 "\xb3 \033[1;37mArrows\033[0;36;44m Joystick \xb3 \033[1;37m%s\033[0;36;44m Fire "
                 "\xb3 \033[1;37mEsc\033[0;36;44m RUN/STOP\033[K\033[0m",
                 g_bar_row, fire);
    if (n > 0)
        out_bytes(o, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

#define WARP_MS  14                     /* of each 20 ms frame, the most spent on extra frames while loading */
#define WARP_MAX 50
static bool g_warp = true;              /* C64DOOR_NO_WARP=1 for real-speed loading (comparing, debugging) */
static long g_led_until;                /* the drive light shows until then (ms): it flickers, the picture shouldn't */

static void fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
        {
            uint8_t *p = g_view + (yy * g_vw + xx) * 3;
            p[0] = r, p[1] = g, p[2] = b;
        }
}

/* While the drive is busy, a 1541's front in the bottom right corner: the green power light and the red drive light,
 * so a caller watching a quiet screen knows a game is loading */
static void draw_drive_light(void)
{
    int w = 22, h = 7, x = g_vw - w - 3, y = g_vh - h - 3;
    fill(x, y, w, h, 0x8a, 0x84, 0x70);                 /* the 1541's beige, its dark edge */
    fill(x + 1, y + 1, w - 2, h - 2, 0xd8, 0xd0, 0xb0);
    fill(x + 3, y + 3, 4, 2, 0x20, 0xc0, 0x40);         /* power */
    fill(x + w - 9, y + 3, 6, 2, 0xff, 0x20, 0x10);     /* drive */
}

/* For the first seconds of every C64 start, how to get to the menu, in the bottom left corner. A 5x7 font of just
 * the letters it needs. */
static long g_hint_until;

static const uint8_t *hint_glyph(char c)
{
    static const struct { char c; uint8_t rows[7]; } font[] = {
        { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
        { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
        { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
        { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
        { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
        { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
        { 'e', { 0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e } },
        { 'n', { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 } },
        { 'u', { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d } },
    };
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].rows;
    return NULL;                                        /* a space */
}

static void draw_hint(void)
{
    static const char text[] = "CTRL Q Menu";
    int n = (int)sizeof(text) - 1, w = n * 6 + 5, h = 11, x = 3, y = g_vh - h - 3;
    fill(x, y, w, h, 0x70, 0x70, 0xe0);                 /* the C64's light blue edge, dark blue inside */
    fill(x + 1, y + 1, w - 2, h - 2, 0x20, 0x20, 0x60);
    for (int i = 0; i < n; i++)
    {
        const uint8_t *g = hint_glyph(text[i]);
        for (int r = 0; g != NULL && r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (0x10 >> b))
                    fill(x + 3 + i * 6 + b, y + 2 + r, 1, 1, 0xff, 0xff, 0xff);
    }
}

static void grab_view(long now)
{
    const uint8_t *rgb = core_rgb();
    for (int y = 0; y < g_vh; y++)
        memcpy(g_view + y * g_vw * 3, rgb + ((g_vy + y) * CORE_W + g_vx) * 3, (size_t)g_vw * 3);
    if (now < g_led_until)
        draw_drive_light();
    if (now < g_hint_until)
        draw_hint();
}

static bool tile_changed(int tx, int ty)
{
    for (int y = ty * TH; y < (ty + 1) * TH; y++)
        if (memcmp(g_view + (y * g_vw + tx * TW) * 3, g_ref + (y * g_vw + tx * TW) * 3, TW * 3) != 0)
            return true;
    return false;
}

static void draw_rect(outbuf_t *o, int x, int y, int w, int h)
{
    const uint8_t *src = g_view + (y * g_vw + x) * 3;
    int stride = g_vw * 3, zoom = g_zoom;
    const uint8_t *jxl;
    size_t n;
    char head[96];

    if (g_fill)
    {
        /* Stretched: screen column d shows the C64's column d * vw / dw, so the tile's own columns are
         * ceil(x * dw / vw) up to ceil((x + w) * dw / vw), and tiles meet with no gap or overlap */
        int d0 = (x * g_dw + g_vw - 1) / g_vw, d1 = ((x + w) * g_dw + g_vw - 1) / g_vw;
        int e0 = (y * g_dh + g_vh - 1) / g_vh, e1 = ((y + h) * g_dh + g_vh - 1) / g_vh;
        int sw = d1 - d0, sh = e1 - e0;
        if (sw <= 0 || sh <= 0)
            return;
        for (int yy = 0; yy < sh; yy++)
        {
            int sy = (e0 + yy) * g_vh / g_dh - y;
            for (int xx = 0; xx < sw; xx++)
                memcpy(g_scaled + (yy * sw + xx) * 3, src + sy * stride + ((d0 + xx) * g_vw / g_dw - x) * 3, 3);
        }
        src = g_scaled;
        stride = sw * 3;
        w = sw, h = sh, x = d0, y = e0;
        zoom = 1;
    }
    /* A terminal that can't scale pictures itself gets them at the size they're shown */
    else if (!g_caps->zoom && g_zoom > 1)
    {
        int sw = w * g_zoom;
        for (int yy = 0; yy < h * g_zoom; yy++)
            for (int xx = 0; xx < sw; xx++)
                memcpy(g_scaled + (yy * sw + xx) * 3, src + (yy / g_zoom) * stride + (xx / g_zoom) * 3, 3);
        src = g_scaled;
        stride = sw * 3;
        w *= g_zoom;
        h *= g_zoom;
        x *= g_zoom;
        y *= g_zoom;
        zoom = 1;
    }
    else
    {
        x *= g_zoom;
        y *= g_zoom;
    }

    n = jxl_enc_rgb(src, stride, w, h, 0.0f, &jxl);
    if (n == 0)
        return;
    if (zoom > 1)
        snprintf(head, sizeof(head), "DX=%d;DY=%d;ZX=%d;ZY=%d;", g_ox + x, g_oy + y, zoom, zoom);
    else
        snprintf(head, sizeof(head), "DX=%d;DY=%d;", g_ox + x, g_oy + y);

    if (g_caps->blob)
    {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "C;DrawJXLBlob;%s", head);
        apc_blob(o, cmd, jxl, n);
    }
    else
    {
        apc_blob(o, "C;S;c64door/frame.jxl;", jxl, n);
        apc_cmd(o, "C;DrawJXL;%sc64door/frame.jxl", head);
    }
}

/* What changed since the caller's copy. False if nothing did. */
static bool draw_changes(outbuf_t *o)
{
    static bool tiles[MAX_TY][MAX_TX];
    int count = 0;

    for (int ty = 0; ty < g_ty; ty++)
        for (int tx = 0; tx < g_tx; tx++)
        {
            tiles[ty][tx] = !g_have_ref || tile_changed(tx, ty);
            count += tiles[ty][tx];
        }
    memcpy(g_ref, g_view, (size_t)g_vw * g_vh * 3);
    g_have_ref = true;
    if (count == 0)
        return false;

    if (count > FULL_FRACTION * g_tx * g_ty)
    {
        draw_rect(o, 0, 0, g_vw, g_vh);
        return true;
    }
    for (int ty = 0; ty < g_ty; ty++)
        for (int tx = 0; tx < g_tx; )
        {
            int x0, x1, ty1;
            if (!tiles[ty][tx])
            {
                tx++;
                continue;
            }
            x0 = tx;
            while (tx < g_tx && tiles[ty][tx])
                tx++;
            x1 = tx;
            for (ty1 = ty + 1; ty1 < g_ty; ty1++)
            {
                bool same = (x0 == 0 || !tiles[ty1][x0 - 1]) && (x1 == g_tx || !tiles[ty1][x1]);
                for (int k = x0; k < x1 && same; k++)
                    same = tiles[ty1][k];
                if (!same)
                    break;
                for (int k = x0; k < x1; k++)
                    tiles[ty1][k] = false;
            }
            draw_rect(o, x0 * TW, ty * TH, (x1 - x0) * TW, (ty1 - ty) * TH);
        }
    return true;
}

/* ---- sound ---- */

static bool    g_sound_on;
static double  g_snd_pos, g_snd_step;   /* resampling: where the next output sample falls between the last input
                                         * sample (0) and the newest (1), and how far each output sample moves on */
static int16_t g_snd_last;
static uint8_t g_piece[SND_PREBUFFER + 1][44 + SND_PIECE];
static int     g_piece_fill, g_pieces_held;
static bool    g_snd_started;
static long    g_snd_pieces;

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void wav_header(uint8_t *h)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + SND_PIECE, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);                   /* PCM */
    put_le(h + 22, 1, 2);                   /* mono */
    put_le(h + 24, SND_RATE, 4);
    put_le(h + 28, SND_RATE, 4);            /* bytes a second */
    put_le(h + 32, 1, 2);
    put_le(h + 34, 8, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, SND_PIECE, 4);
}

static void sound_send(outbuf_t *o, const uint8_t *wav)
{
    apc_blob(o, "C;S;" SND_FILE ";", wav, 44 + SND_PIECE);
    apc_cmd(o, "A;Load;S=%d;" SND_FILE, SND_SLOT);
    apc_cmd(o, "A;Queue;C=%d;S=%d", SND_CH, SND_SLOT);
    g_snd_pieces++;
}

/* The core's newest sound into pieces; a finished piece goes out (after the first few are held back as a cushion).
 * Returns the bytes added, 0 if none. */
static size_t sound_update(outbuf_t *o)
{
    int16_t buf[2048 * 2];
    int n;
    size_t before = o->len;

    while ((n = core_audio(buf, 2048)) > 0)
    {
        if (!g_sound_on)
            continue;
        for (int i = 0; i < n; i++)
        {
            int16_t mono = (int16_t)((buf[i * 2] + buf[i * 2 + 1]) / 2);
            /* the input moves on one sample; every output sample that falls between the last and this one is made */
            while (g_snd_pos <= 1.0)
            {
                double s = g_snd_last + (mono - g_snd_last) * g_snd_pos;
                uint8_t *piece = g_piece[g_pieces_held];
                piece[44 + g_piece_fill++] = (uint8_t)((int)lrint(s) / 256 + 128);
                g_snd_pos += g_snd_step;
                if (g_piece_fill == SND_PIECE)
                {
                    wav_header(piece);
                    g_piece_fill = 0;
                    if (!g_snd_started && ++g_pieces_held < SND_PREBUFFER + 1)
                        continue;
                    if (!g_snd_started)
                    {
                        for (int k = 0; k <= SND_PREBUFFER; k++)
                            sound_send(o, g_piece[k]);
                        g_pieces_held = 0;
                        g_snd_started = true;
                    }
                    else
                        sound_send(o, piece);
                }
            }
            g_snd_pos -= 1.0;
            g_snd_last = mono;
        }
    }
    return o->len - before;
}

static void sound_restart(outbuf_t *o)
{
    apc_cmd(o, "A;Flush;C=%d", SND_CH);
    g_piece_fill = g_pieces_held = 0;
    g_snd_started = false;
    g_snd_pos = 0;
}

/* ---- keys ---- */

static bool g_held[256];                /* by Linux key code, as CSI = 1 h reports them */
static bool g_joy_mode = true;          /* the arrows and fire keys are the joystick */
static int  g_joy_port = 2;
static bool g_menu_asked, g_quit_asked;
static unsigned g_typed_joy;            /* the joystick as typed keys hold it, without key reports */
static long g_typed_joy_until;

/* A Linux key code as a libretro key */
static unsigned evdev_to_retrok(int code)
{
    static const unsigned main_block[89] = {
        0, RETROK_ESCAPE, RETROK_1, RETROK_2, RETROK_3, RETROK_4, RETROK_5, RETROK_6, RETROK_7, RETROK_8, RETROK_9,
        RETROK_0, RETROK_MINUS, RETROK_EQUALS, RETROK_BACKSPACE, RETROK_TAB, RETROK_q, RETROK_w, RETROK_e, RETROK_r,
        RETROK_t, RETROK_y, RETROK_u, RETROK_i, RETROK_o, RETROK_p, RETROK_LEFTBRACKET, RETROK_RIGHTBRACKET,
        RETROK_RETURN, RETROK_LCTRL, RETROK_a, RETROK_s, RETROK_d, RETROK_f, RETROK_g, RETROK_h, RETROK_j, RETROK_k,
        RETROK_l, RETROK_SEMICOLON, RETROK_QUOTE, RETROK_BACKQUOTE, RETROK_LSHIFT, RETROK_BACKSLASH, RETROK_z,
        RETROK_x, RETROK_c, RETROK_v, RETROK_b, RETROK_n, RETROK_m, RETROK_COMMA, RETROK_PERIOD, RETROK_SLASH,
        RETROK_RSHIFT, RETROK_KP_MULTIPLY, RETROK_LALT, RETROK_SPACE, RETROK_CAPSLOCK, RETROK_F1, RETROK_F2,
        RETROK_F3, RETROK_F4, RETROK_F5, RETROK_F6, RETROK_F7, RETROK_F8, RETROK_F9, RETROK_F10, RETROK_NUMLOCK,
        RETROK_SCROLLOCK, RETROK_KP7, RETROK_KP8, RETROK_KP9, RETROK_KP_MINUS, RETROK_KP4, RETROK_KP5, RETROK_KP6,
        RETROK_KP_PLUS, RETROK_KP1, RETROK_KP2, RETROK_KP3, RETROK_KP0, RETROK_KP_PERIOD, 0, 0, RETROK_LESS,
        RETROK_F11, RETROK_F12,
    };
    if (code >= 0 && code <= 88)
        return main_block[code];
    switch (code)
    {
    case 96:  return RETROK_KP_ENTER;
    case 97:  return RETROK_RCTRL;
    case 98:  return RETROK_KP_DIVIDE;
    case 100: return RETROK_RALT;
    case 102: return RETROK_HOME;
    case 103: return RETROK_UP;
    case 104: return RETROK_PAGEUP;
    case 105: return RETROK_LEFT;
    case 106: return RETROK_RIGHT;
    case 107: return RETROK_END;
    case 108: return RETROK_DOWN;
    case 109: return RETROK_PAGEDOWN;
    case 110: return RETROK_INSERT;
    case 111: return RETROK_DELETE;
    default:  return 0;
    }
}

/* Which joystick bits a Linux key code drives in joystick mode (0: none) */
static unsigned joy_bits(int code)
{
    switch (code)
    {
    case 103: case 72: return JOY_UP;               /* Up, keypad 8 */
    case 108: case 80: return JOY_DOWN;
    case 105: case 75: return JOY_LEFT;
    case 106: case 77: return JOY_RIGHT;
    case 71:  return JOY_UP | JOY_LEFT;             /* keypad 7, 9, 1, 3 */
    case 73:  return JOY_UP | JOY_RIGHT;
    case 79:  return JOY_DOWN | JOY_LEFT;
    case 81:  return JOY_DOWN | JOY_RIGHT;
    case 97: case 100: case 56: case 82: return JOY_FIRE;  /* right Ctrl, right Alt, left Alt, keypad 0 */
    default:  return 0;
    }
}

static void sync_joystick(long now)
{
    unsigned bits = 0;
    if (g_joy_mode)
    {
        for (int c = 0; c < 256; c++)
            if (g_held[c])
                bits |= joy_bits(c);
        if (now < g_typed_joy_until)
            bits |= g_typed_joy;
    }
    core_joystick(bits);
}

static void key_report(const char *params, bool down, long now)
{
    /* CSI = a;b;c K (down) or k (up): usually one code, several only when the terminal lets go of everything */
    for (const char *p = params; *p != '\0'; )
    {
        int code = atoi(p);
        if (code > 0 && code < 256 && g_held[code] != down)
        {
            g_held[code] = down;
            if (code == 88 || (code == 16 && (g_held[29] || g_held[97])))
            {
                /* F12, or Ctrl-Q for keyboards whose F12 needs Fn: the session menu, never the C64's */
                if (down)
                    g_menu_asked = true;
            }
            else if (g_joy_mode && joy_bits(code) != 0)
                sync_joystick(now);
            else if (evdev_to_retrok(code) != 0)
                core_key(evdev_to_retrok(code), down);
        }
        p = strchr(p, ';');
        if (p == NULL)
            break;
        p++;
    }
}

static void release_all(long now)
{
    for (int c = 1; c < 256; c++)
        if (g_held[c])
        {
            g_held[c] = false;
            if (evdev_to_retrok(c) != 0)
                core_key(evdev_to_retrok(c), false);
        }
    g_typed_joy_until = 0;
    sync_joystick(now);
}

/* Without key reports: typed characters become key taps, one at a time */
#define TAPS 256
static struct { unsigned key; bool shift; } g_taps[TAPS];
static int g_tap_head, g_tap_count, g_tap_phase;

static void tap(unsigned key, bool shift)
{
    if (g_tap_count < TAPS)
    {
        int i = (g_tap_head + g_tap_count++) % TAPS;
        g_taps[i].key = key;
        g_taps[i].shift = shift;
    }
}

/* A typed character as the US-layout key (and Shift) that makes it */
static void tap_char(unsigned char c)
{
    static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    static const char base[]    = "`1234567890-=[]\\;',./";
    const char *s;

    if (c >= 'a' && c <= 'z')
        tap(RETROK_a + (c - 'a'), false);
    else if (c >= 'A' && c <= 'Z')
        tap(RETROK_a + (c - 'A'), true);
    else if (c >= '0' && c <= '9')
        tap(RETROK_0 + (c - '0'), false);
    else if (c == ' ')
        tap(RETROK_SPACE, false);
    else if (c == '\r' || c == '\n')
        tap(RETROK_RETURN, false);
    else if (c == 8 || c == 127)
        tap(RETROK_BACKSPACE, false);
    else if (c != 0 && (s = strchr(shifted, c)) != NULL)
        tap((unsigned)base[s - shifted], true);
    else if (c != 0 && strchr(base, c) != NULL)
        tap(c, false);                                  /* RETROK_ for these is the character itself */
}

/* One step a frame: press, hold, let go, pause */
static void tap_step(void)
{
    if (g_tap_count == 0)
        return;
    if (g_tap_phase == 0)
    {
        if (g_taps[g_tap_head].shift)
            core_key(RETROK_LSHIFT, true);
        core_key(g_taps[g_tap_head].key, true);
    }
    else if (g_tap_phase == TAP_DOWN_FRAMES)
    {
        core_key(g_taps[g_tap_head].key, false);
        if (g_taps[g_tap_head].shift)
            core_key(RETROK_LSHIFT, false);
    }
    if (++g_tap_phase == TAP_DOWN_FRAMES + TAP_UP_FRAMES)
    {
        g_tap_phase = 0;
        g_tap_head = (g_tap_head + 1) % TAPS;
        g_tap_count--;
    }
}

/* A typed escape sequence (no key reports): arrows, F1-F8, F12 */
static void typed_sequence(const char *params, unsigned char final, long now)
{
    unsigned dir = final == 'A' ? JOY_UP : final == 'B' ? JOY_DOWN : final == 'C' ? JOY_RIGHT : final == 'D' ? JOY_LEFT : 0;

    if (dir != 0 && params[0] == '\0')
    {
        if (g_joy_mode)
        {
            g_typed_joy = (now < g_typed_joy_until ? g_typed_joy : 0) | dir;
            g_typed_joy_until = now + TYPED_HOLD_MS;
            sync_joystick(now);
        }
        else
            tap(final == 'A' ? RETROK_UP : final == 'B' ? RETROK_DOWN : final == 'C' ? RETROK_RIGHT : RETROK_LEFT,
                false);
        return;
    }
    if (final == '~')
    {
        static const struct { int n; unsigned key; } f[] = { { 11, RETROK_F1 }, { 12, RETROK_F2 }, { 13, RETROK_F3 },
            { 14, RETROK_F4 }, { 15, RETROK_F5 }, { 17, RETROK_F6 }, { 18, RETROK_F7 }, { 19, RETROK_F8 },
            { 1, RETROK_HOME }, { 7, RETROK_HOME }, { 3, RETROK_DELETE }, { 5, RETROK_PAGEUP } };
        int n = atoi(params);
        if (n == 24)
        {
            g_menu_asked = true;
            return;
        }
        for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
            if (f[i].n == n)
                tap(f[i].key, false);
    }
    else if (final == 'H')
        tap(RETROK_HOME, false);
}

/*
 * The caller's bytes: answers to the door's own questions (cursor position, key reports, sound notices) are taken
 * out here; anything else is typing. An escape sequence can be split across reads, so it is gathered until complete.
 */
static unsigned char g_seq[64];
static int  g_seq_len;
static long g_seq_at;

static void finish_sequence(long now)
{
    unsigned char final = g_seq[g_seq_len - 1];
    char params[64];
    int n = g_seq_len - 3;
    bool ss3 = g_seq[1] == 'O';

    memcpy(params, g_seq + 2, (size_t)n);
    params[n] = '\0';
    g_seq_len = 0;

    if (ss3)
    {
        /* ESC O P..S: F1-F4 */
        if (!g_caps->keys && final >= 'P' && final <= 'S')
            tap(RETROK_F1 + (final - 'P'), false);
        return;
    }
    if (final == 'R' && params[0] >= '0' && params[0] <= '9')
        fly_ack(now);                                   /* ESC [ row ; col R: a frame has arrived */
    else if (params[0] == '=' && (final == 'K' || final == 'k'))
        key_report(params + 1, final == 'K', now);
    else if (params[0] == '=' || params[0] == '<' || params[0] == '?')
        ;                                               /* sound notices and other answers */
    else if (!g_caps->keys)
        typed_sequence(params, final, now);
}

static void take_input(const unsigned char *data, int len, long now)
{
    for (int i = 0; i < len; i++)
    {
        unsigned char c = data[i];

        if (g_seq_len == 1)
        {
            if (c == '[' || c == 'O')
            {
                g_seq[g_seq_len++] = c;
                continue;
            }
            if (!g_caps->keys)
                tap(RETROK_ESCAPE, false);              /* ESC then something else: RUN/STOP, then that */
            g_seq_len = 0;
        }
        else if (g_seq_len >= 2)
        {
            if (g_seq_len < (int)sizeof(g_seq))
                g_seq[g_seq_len++] = c;
            if (c >= 0x40 && c <= 0x7E)
                finish_sequence(now);
            else if (g_seq_len == (int)sizeof(g_seq))
                g_seq_len = 0;
            continue;
        }

        if (c == 27)
        {
            g_seq[0] = c;
            g_seq_len = 1;
            g_seq_at = now;
            continue;
        }
        if (g_caps->keys)
            continue;                                   /* keys come as reports; this is just the typed copy */
        if (c == 17)
            g_menu_asked = true;                        /* Ctrl-Q: the session menu, as F12 */
        else if (c == ' ' && g_joy_mode)
        {
            g_typed_joy = (now < g_typed_joy_until ? g_typed_joy : 0) | JOY_FIRE;
            g_typed_joy_until = now + TYPED_HOLD_MS;
        }
        else
            tap_char(c);
    }
    if (g_seq_len == 1 && now - g_seq_at > 50)
    {
        if (!g_caps->keys)
            tap(RETROK_ESCAPE, false);                  /* a lone Escape is RUN/STOP */
        g_seq_len = 0;
    }
}

/* ---- a slower link, for testing (as pix_play.c) ---- */

#define SHAPE_CHUNKS 4096
#define SHAPE_IN     256

static bool     g_shape;
static double   g_shape_rate, g_shape_free;
static long     g_shape_half_ping;
static outbuf_t g_shape_out;
static size_t   g_shape_sent;
static struct { long due; size_t end; } g_shape_chunks[SHAPE_CHUNKS];
static int      g_shape_head, g_shape_count;
static struct { long due; int len; unsigned char data[512]; } g_shape_in[SHAPE_IN];
static int      g_in_head, g_in_count;

static void shape_out(const char *data, size_t len, long now)
{
    int i;
    if (g_shape_count == SHAPE_CHUNKS)
        return;
    if (g_shape_free < now)
        g_shape_free = now;
    g_shape_free += (double)len / g_shape_rate;
    out_bytes(&g_shape_out, data, len);
    i = (g_shape_head + g_shape_count) % SHAPE_CHUNKS;
    g_shape_chunks[i].due = (long)g_shape_free + g_shape_half_ping;
    g_shape_chunks[i].end = g_shape_out.len;
    g_shape_count++;
}

static bool shape_flush(long now, bool all)
{
    while (g_shape_count > 0 && (all || g_shape_chunks[g_shape_head].due <= now))
    {
        size_t end = g_shape_chunks[g_shape_head].end;
        if (!write_all(g_shape_out.data + g_shape_sent, end - g_shape_sent))
            return false;
        g_shape_sent = end;
        g_shape_head = (g_shape_head + 1) % SHAPE_CHUNKS;
        g_shape_count--;
    }
    if (g_shape_count == 0)
        g_shape_out.len = g_shape_sent = 0;
    return true;
}

static void shape_in(const unsigned char *data, int len, long now)
{
    int i;
    if (g_in_count == SHAPE_IN)
        return;
    i = (g_in_head + g_in_count) % SHAPE_IN;
    g_shape_in[i].due = now + g_shape_half_ping;
    g_shape_in[i].len = len > 512 ? 512 : len;
    memcpy(g_shape_in[i].data, data, (size_t)g_shape_in[i].len);
    g_in_count++;
}

static void shape_in_release(long now)
{
    while (g_in_count > 0 && g_shape_in[g_in_head].due <= now)
    {
        take_input(g_shape_in[g_in_head].data, g_shape_in[g_in_head].len, now);
        g_in_head = (g_in_head + 1) % SHAPE_IN;
        g_in_count--;
    }
}

static bool send_out(const outbuf_t *o, long now)
{
    if (o->len == 0)
        return true;
    if (g_shape)
    {
        shape_out(o->data, o->len, now);
        return true;
    }
    return write_all(o->data, o->len);
}

/* ---- if the door itself falls over: put the terminal back first (as pix_play.c) ---- */

static const char CRASH_RESET[] = "\033[=2l\033[=1l" APC_PREFIX "A;Flush;C=2" APC_END "\033[0m\033[2J\033[H\033[?25h";
static const int CRASH_SIGNALS[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

static void crash_reset(int sig)
{
    ssize_t n = write(STDOUT_FILENO, CRASH_RESET, sizeof(CRASH_RESET) - 1);
    (void)n;
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_guard(bool on)
{
    for (size_t i = 0; i < sizeof(CRASH_SIGNALS) / sizeof(CRASH_SIGNALS[0]); i++)
        signal(CRASH_SIGNALS[i], on ? crash_reset : SIG_DFL);
}

/* ---- the session menu (F12) ---- */

/* One typed key, waiting up to timeout_ms; -1 if none, -2 if the caller has gone */
static int read_key(int timeout_ms)
{
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    fd_set fds;
    unsigned char c;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return -1;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return -2;
    return c;
}

/* Drops whatever the terminal still has to say (answers to frames in flight, key reports) */
static void drain_input(int quiet_ms)
{
    while (read_key(quiet_ms) >= 0)
        ;
}

static void menu_line(outbuf_t *o, const char *key, const char *text, const char *state)
{
    out_fmt(o, "\033[1;37m  [%s] \033[0;37m%-34s\033[1;36m%s\033[0m\r\n", key, text, state ? state : "");
}

static void menu_title(outbuf_t *o, const char *text)
{
    out_str(o, "\033[0m\033[2J\033[H\033[?25l");
    out_fmt(o, "\033[1;34m  ===============================================\r\n"
               "\033[1;37m    %s\r\n"
               "\033[1;34m  ===============================================\r\n\r\n", text);
}

static void show(outbuf_t *o)
{
    write_all(o->data, o->len);
    o->len = 0;
}

typedef enum { MENU_RESUME, MENU_QUIT, MENU_RELOAD, MENU_HANGUP } menu_result_t;

/* The library, a page at a time. MENU_RELOAD when a title was picked (drive.txt now holds it). */
static menu_result_t menu_library(outbuf_t *o)
{
    static title_t titles[200];
    int n = library_load(titles, 200), page = 0, pages = (n + 8) / 9;

    for (;;)
    {
        int c;
        menu_title(o, "The library");
        if (n == 0)
        {
            out_str(o, "\033[1;33m  The sysop hasn't put any disks in the library yet.\r\n\r\n"
                       "\033[0;37m  Any key goes back.\033[0m");
            show(o);
            return read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
        }
        for (int i = 0; i < 9 && page * 9 + i < n; i++)
        {
            const title_t *t = &titles[page * 9 + i];
            out_fmt(o, "\033[1;37m  [%d] \033[1;36m%s\r\n\033[0;37m      %s\r\n", i + 1, t->title, t->about);
        }
        out_fmt(o, "\r\n\033[0;37m  1-9 = load it and start%s   any other key = back\033[0m",
                pages > 1 ? "   N/P = next/previous page" : "");
        show(o);
        c = read_key(600000);
        if (c == -2)
            return MENU_HANGUP;
        if ((c == 'n' || c == 'N') && page + 1 < pages)
            page++;
        else if ((c == 'p' || c == 'P') && page > 0)
            page--;
        else if (c >= '1' && c <= '9' && page * 9 + (c - '1') < n)
        {
            drive_t d;
            const char *why = NULL;
            if (library_use(&titles[page * 9 + (c - '1')], &d, &why) && drive_save(&d))
                return MENU_RELOAD;
            menu_title(o, "The library");
            out_fmt(o, "\033[1;33m  That title couldn't be loaded: %s\r\n\r\n\033[0;37m  Any key goes back.\033[0m",
                    why ? why : "the BBS couldn't store it");
            show(o);
            return read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
        }
        else
        {
            if (c == 27)
                drain_input(50);
            return MENU_RESUME;
        }
    }
}

static menu_result_t ask_start(outbuf_t *o, drive_t *dp);

/* The caller's own disks by ZMODEM. MENU_RELOAD when some arrived and they chose to boot them. */
static menu_result_t menu_upload(outbuf_t *o)
{
    char incoming[600], *const argv[] = { "rz", "-b", "-E", "-R", NULL };
    drive_t d = { 0 };
    DIR *dir;
    struct dirent *e;
    pid_t pid;

    snprintf(incoming, sizeof(incoming), "%s/incoming", g_session_dir);
    mkdir(incoming, 0700);
    menu_title(o, "Send your disk");
    out_str(o, "\033[0;37m  Start a ZMODEM upload in your terminal now (in SyncTERM: Alt-U).\r\n"
               "  .D64, .G64, .D81, .CRT (cartridges) and .PRG files work, several at\r\n"
               "  once for a multi-disk game.\r\n"
               "  Cancel the transfer (Ctrl-X a few times) to come back without one.\r\n\r\n\033[0m");
    show(o);
    if ((pid = fork()) == 0)
    {
        if (chdir(incoming) != 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
    drain_input(300);                                   /* the transfer's last packets aren't menu choices */

    menu_title(o, "Send your disk");
    if ((dir = opendir(incoming)) != NULL)
    {
        while ((e = readdir(dir)) != NULL)
        {
            char path[900];
            const char *why = NULL, *not_kept;
            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s", incoming, e->d_name);
            if (mine_add(&d, path, e->d_name, &why))
                out_fmt(o, "\033[1;32m  Arrived and kept: %.50s\r\n", d.disk[d.count - 1].name);
            else if (strncmp(why, "it ", 3) != 0 && (not_kept = why, disk_add(&d, path, e->d_name, &why)))
                out_fmt(o, "\033[1;33m  Arrived, for this visit only: %.40s\r\n\033[0;37m    (not kept: %s)\r\n",
                        e->d_name, not_kept);           /* the image is fine, keeping it isn't: a disk for now */
            else
                out_fmt(o, "\033[1;33m  Not used: %.40s (%s)\r\n", e->d_name, why);
            remove(path);
        }
        closedir(dir);
    }
    if (d.count == 0)
    {
        out_str(o, "\033[0;37m  No disk arrived.\r\n\r\n  Any key goes back.\033[0m");
        show(o);
        return read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
    }
    return ask_start(o, &d);
}

/* After disks were chosen: start the first by itself, or go to BASIC with it in the drive. MENU_RELOAD if either. */
static menu_result_t ask_start(outbuf_t *o, drive_t *dp)
{
    drive_t d = *dp;
    size_t len = strlen(d.disk[0].path);
    int c;

    if (len > 4 && strcmp(d.disk[0].path + len - 4, ".crt") == 0)
    {
        out_str(o, "\r\n\033[0;37m  A cartridge: the C64 restarts with it plugged in, and it starts at once.\r\n\r\n"
                   "  C = go, any other key keeps what's in the C64 now.\033[0m");
        show(o);
        c = read_key(600000);
        if (c == -2)
            return MENU_HANGUP;
        if (c != 'c' && c != 'C')
            return MENU_RESUME;
        d.autostart = true;
        return drive_save(&d) ? MENU_RELOAD : MENU_RESUME;
    }
    out_str(o, "\r\n\033[0;37m  The C64 restarts with it in drive 8.\r\n\r\n");
    menu_line(o, "A", "Start it by itself", "LOAD\"*\",8,1 and RUN");
    menu_line(o, "B", "Go to BASIC", "LOAD\"$\",8 to look first");
    out_str(o, "\r\n\033[0;37m  Any other key keeps what's in the drive now.\033[0m");
    show(o);
    c = read_key(600000);
    if (c == -2)
        return MENU_HANGUP;
    if (c != 'a' && c != 'A' && c != 'b' && c != 'B')
        return MENU_RESUME;
    d.autostart = c == 'a' || c == 'A';
    return drive_save(&d) ? MENU_RELOAD : MENU_RESUME;
}

/* The caller's kept disks, a page at a time: boot one, or delete one. MENU_RELOAD when one was booted. */
static menu_result_t menu_mine(outbuf_t *o)
{
    static char names[MINE_MAX][64];
    int page = 0;
    bool deleting = false;

    for (;;)
    {
        int n = mine_list(names, MINE_MAX), pages = (n + 8) / 9, c;
        if (page >= pages && page > 0)
            page = pages - 1;
        menu_title(o, deleting ? "Delete a kept disk" : "Your kept disks");
        if (n == 0)
        {
            out_str(o, "\033[0;37m  You haven't kept any disks yet. The ones you upload (F12, U) are kept\r\n"
                       "  here for your next visit, saved games and all.\r\n\r\n  Any key goes back.\033[0m");
            show(o);
            return read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
        }
        for (int i = 0; i < 9 && page * 9 + i < n; i++)
            out_fmt(o, "\033[1;37m  [%d] \033[1;36m%s\r\n", i + 1, names[page * 9 + i]);
        if (deleting)
            out_str(o, "\r\n\033[1;33m  1-9 = delete that disk for good\033[0;37m   any other key = don't\033[0m");
        else
            out_fmt(o, "\r\n\033[0;37m  %d of %d kept.  1-9 = put it in drive 8 (the ones after it follow, for D)\r\n"
                       "  X = delete one%s   any other key = back\033[0m",
                    n, MINE_MAX, pages > 1 ? "   N/P = next/previous page" : "");
        show(o);
        c = read_key(600000);
        if (c == -2)
            return MENU_HANGUP;
        if (c >= '1' && c <= '9' && page * 9 + (c - '1') < n)
        {
            int index = page * 9 + (c - '1');
            drive_t d;
            if (deleting)
            {
                mine_delete(names[index]);
                deleting = false;
                continue;
            }
            if (!mine_use(index, &d))
                continue;
            menu_title(o, "Your kept disks");
            out_fmt(o, "\033[1;36m  %s\r\n", d.disk[0].name);
            return ask_start(o, &d);
        }
        if (deleting)
            deleting = false;
        else if (c == 'x' || c == 'X')
            deleting = true;
        else if ((c == 'n' || c == 'N') && page + 1 < pages)
            page++;
        else if ((c == 'p' || c == 'P') && page > 0)
            page--;
        else
        {
            if (c == 27)
                drain_input(50);
            return MENU_RESUME;
        }
    }
}

/* Reset to the BASIC prompt with the disk left in the drive. The core's own reset always autostarts the disk it was
 * started with, so this restarts the C64 (MENU_RELOAD) with autostart off and the disk in the drive now first. */
static menu_result_t reset_to_basic(drive_t *d, int in)
{
    drive_t r = { 0 };

    if (d->count == 0)
    {
        core_reset();                                   /* nothing to autostart: straight to BASIC */
        return MENU_RESUME;
    }
    if (in < 0 || in >= d->count)
        in = 0;
    for (int i = 0; i < d->count; i++)
        r.disk[r.count++] = d->disk[(in + i) % d->count];
    r.autostart = false;
    if (drive_save(&r))
        return MENU_RELOAD;
    core_reset();
    return MENU_RESUME;
}

static menu_result_t session_menu(void)
{
    outbuf_t o = { 0 };
    menu_result_t result = MENU_RESUME;
    long now = now_ms();

    release_all(now);
    if (g_caps->keys)
        out_str(&o, "\033[=2l\033[=1l");             /* plain typing while the menu is up */
    show(&o);
    drain_input(150);
    /* The answers for frames in flight were just thrown away with the rest, so none are owed any more */
    g_fly.count = 0;
    g_fly.total = 0;
    g_orphans = 0;

    for (bool done = false; !done; )
    {
        int disks = core_disk_count(), in = core_disk_index(), c;
        drive_t d;
        char state[96];

        drive_load(&d);
        menu_title(&o, "C64 paused");
        if (d.count == 0)
            out_str(&o, "\033[0;37m  Drive 8: \033[1;36mempty\r\n\r\n");
        else
            out_fmt(&o, "\033[0;37m  Drive 8: \033[1;36m%s\033[0;37m%s\r\n\r\n",
                    d.disk[in < d.count ? in : 0].name, disks > 1 ? " (F12, D for the next disk)" : "");
        menu_line(&o, "L", "Load a disk from the library", NULL);
        menu_line(&o, "U", "Upload your own disk (ZMODEM)", mine_available() ? "kept for next time" : NULL);
        if (mine_available())
        {
            char kept[MINE_MAX][64];
            snprintf(state, sizeof(state), "%d of %d", mine_list(kept, MINE_MAX), MINE_MAX);
            menu_line(&o, "M", "Your kept disks", state);
        }
        if (disks > 1)
        {
            snprintf(state, sizeof(state), "disk %d of %d", in + 1, disks);
            menu_line(&o, "D", "Next disk into drive 8", state);
        }
        snprintf(state, sizeof(state), "port %d", g_joy_port);
        menu_line(&o, "J", "Joystick port", state);
        menu_line(&o, "K", "Arrow keys + Ctrl/Alt", g_joy_mode ? "joystick" : "C64 cursor keys");
        if (g_caps->sound)
            menu_line(&o, "S", "Sound", g_sound_on ? "on" : "off");
        menu_line(&o, "R", "Reset the C64", NULL);
        menu_line(&o, "Q", "Leave (back to the BBS)", NULL);
        out_str(&o, "\r\n\033[0;37m  Any other key goes back to the C64.\033[0m");
        show(&o);

        c = read_key(600000);
        if (c == -2)
        {
            result = MENU_HANGUP;
            break;
        }
        switch (c)
        {
        case 'l': case 'L':
            result = menu_library(&o);
            done = result != MENU_RESUME;
            break;
        case 'u': case 'U':
            result = menu_upload(&o);
            done = result != MENU_RESUME;
            break;
        case 'm': case 'M':
            if (mine_available())
            {
                result = menu_mine(&o);
                done = result != MENU_RESUME;
            }
            break;
        case 'd': case 'D':
            if (disks > 1)
                core_disk_insert((in + 1) % disks);
            break;
        case 'j': case 'J':
            g_joy_port = g_joy_port == 2 ? 1 : 2;
            core_joystick_port(g_joy_port);
            break;
        case 'k': case 'K':
            g_joy_mode = !g_joy_mode;
            break;
        case 's': case 'S':
            if (g_caps->sound)
                g_sound_on = !g_sound_on;
            break;
        case 'r': case 'R':
            result = reset_to_basic(&d, in);
            done = true;
            break;
        case 'q': case 'Q':
            result = MENU_QUIT;
            done = true;
            break;
        case 27:
            drain_input(50);                            /* an arrow or F-key's sequence: just go back */
            done = true;
            break;
        default:
            done = true;
            break;
        }
    }

    o.len = 0;
    out_str(&o, "\033[0m\033[2J\033[H\033[?25l");
    draw_bar(&o);
    if (g_caps->keys && result == MENU_RESUME)
        out_str(&o, "\033[=1h\033[=2h");
    if (g_caps->sound)
        sound_restart(&o);
    show(&o);
    free(o.data);
    g_have_ref = false;                                 /* the whole picture again */
    g_menu_asked = false;
    return result;
}

/* ---- the session ---- */

play_result_t play(const play_caps_t *caps, const play_setup_t *setup)
{
    play_result_t result = PLAY_QUIT;
    outbuf_t out = { 0 };
    FILE *log = setup->log != NULL ? fopen(setup->log, "w") : NULL;
    const char *why = NULL;
    double frame_ms;
    long next_emu, stats_at, control_at;
    long frames_sent = 0, frames_skipped = 0, bytes = 0, emu_frames = 0, emu_late = 0, drive_frames = 0, warp_frames = 0;
    bool frame_waiting = false;
    const char *dump_dir = getenv("C64DOOR_DUMP");
    long dump_n = 0;

    g_caps = caps;
    g_session_dir = setup->session;
    g_sound_on = caps->sound;
    g_warp = getenv("C64DOOR_NO_WARP") == NULL;
    choose_view();
    if (setup->link_kbps > 0)
    {
        g_shape = true;
        g_shape_rate = setup->link_kbps * 1024.0 / 1000.0;
        g_shape_half_ping = setup->link_ping / 2;
    }
    if (log != NULL)
        fprintf(log, "c64: screen %dx%d, %d rows; showing %dx%d as %dx%d (%s), key reports %s, sound %s, pictures %s%s\n",
                caps->px_w, caps->px_h, g_bar_row, g_vw, g_vh, g_dw, g_dh,
                g_fill ? "stretched here" : caps->zoom ? "scaled by the terminal" : "pre-scaled here",
                caps->keys ? "on" : "off", caps->sound ? "on" : "off", caps->blob ? "in the command" : "through the cache",
                g_shape ? ", LINK TEST" : "");

    if (!core_start(setup->core, setup->system, setup->m3u, &why))
    {
        if (log != NULL)
            fprintf(log, "c64: %s\n", why);
        if (log != NULL)
            fclose(log);
        return PLAY_FAILED;
    }
    frame_ms = 1000.0 / core_fps();
    g_snd_step = core_sample_rate() / SND_RATE;         /* input samples per output sample */
    if (log != NULL)
        fprintf(log, "c64: core %.3f fps, sound %.0f Hz\n", core_fps(), core_sample_rate());

    /* Colours reset, screen cleared, cursor hidden; key reports on (and the typed copies of keys off) */
    out_str(&out, "\033[0m\033[2J\033[H\033[?25l");
    draw_bar(&out);
    if (caps->keys)
        out_str(&out, "\033[=1h\033[=2h");
    if (caps->sound)
        sound_restart(&out);
    write_all(out.data, out.len);
    crash_guard(true);

    next_emu = stats_at = control_at = now_ms();
    if (g_bar_row < 1)
        g_hint_until = next_emu + 5000;   /* the controls row says it; without one, a few seconds on the picture */
    for (;;)
    {
        long now = now_ms();
        long wait = next_emu - now;

        /* Input until the C64's next frame is due */
        {
            struct timeval tv = { 0, wait > 0 ? wait * 1000 : 0 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
            {
                unsigned char buf[512];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
                {
                    result = PLAY_HANGUP;
                    break;
                }
                if (n > 0 && g_shape)
                    shape_in(buf, (int)n, now_ms());
                else if (n > 0)
                    take_input(buf, (int)n, now_ms());
            }
            else
                take_input(NULL, 0, now_ms());
        }
        now = now_ms();
        if (g_shape)
        {
            shape_in_release(now);
            if (!shape_flush(now, false))
            {
                result = PLAY_HANGUP;
                break;
            }
        }
        if (g_quit_asked)
            break;
        if (time(NULL) >= setup->deadline)
        {
            result = PLAY_TIMEOUT;
            break;
        }
        if (g_menu_asked)
        {
            menu_result_t m = session_menu();
            if (m == MENU_QUIT)
                break;
            if (m == MENU_RELOAD)
            {
                result = PLAY_RELOAD;                   /* the door starts the C64 again with the new disks */
                break;
            }
            if (m == MENU_HANGUP)
            {
                result = PLAY_HANGUP;
                break;
            }
            next_emu = now_ms();
            continue;
        }

        if (g_fly.count > 0 && now - g_fly.sent[g_fly.head] > STALL_MS)
            fly_reset();

        if (now < next_emu)
            continue;

        /* One frame of C64 time; a door that fell far behind catches up by skipping time, never by racing */
        if (now - next_emu > 200)
        {
            next_emu = now;
            emu_late++;
        }
        next_emu += (long)lround(frame_ms);
        if (!caps->keys)
        {
            tap_step();
            if (g_typed_joy_until != 0 && now >= g_typed_joy_until)
            {
                g_typed_joy_until = 0;
                sync_joystick(now);
            }
        }
        core_run_frame();
        emu_frames++;
        if (core_drive_led())
        {
            /* Warp while a disk loads: the drive busy and the C64 silent (as VICE's own load warp judges it), as many
             * more frames as fit in most of this frame's time. Their silence is dropped, so the sound stays in time. */
            long until = now_ms() + WARP_MS;
            int extra = 0;
            while (g_warp && core_drive_led() && core_frame_silent() && extra < WARP_MAX && now_ms() < until)
            {
                core_run_frame();
                extra++;
            }
            if (extra > 0 && core_frame_silent())
                core_audio_drop();
            warp_frames += extra;
            g_led_until = now + 600;
            drive_frames++;
        }
        if (dump_dir != NULL && ++dump_n % 250 == 0)
        {
            /* C64DOOR_DUMP=<dir>: a snapshot every 5 s, for debugging what the C64 shows */
            char path[600];
            FILE *f;
            snprintf(path, sizeof(path), "%s/f%05ld.ppm", dump_dir, dump_n);
            if ((f = fopen(path, "wb")) != NULL)
            {
                fprintf(f, "P6\n%d %d\n255\n", CORE_W, CORE_H);
                fwrite(core_rgb(), 1, CORE_W * CORE_H * 3, f);
                fclose(f);
            }
        }
        frame_waiting = true;

        out.len = 0;
        if (caps->sound)
        {
            size_t n = sound_update(&out);
            if (n > 0)
            {
                out_str(&out, "\033[6n");
                fly_push(now, out.len);
            }
        }
        if (frame_waiting && window_open())
        {
            size_t before = out.len;
            grab_view(now);
            if (draw_changes(&out))
            {
                out_str(&out, "\033[6n");
                g_frame_avg = g_frame_avg * 0.9 + (double)(out.len - before) * 0.1;
                fly_push(now, out.len - before);
                frames_sent++;
            }
            frame_waiting = false;
        }
        else if (frame_waiting)
            frames_skipped++;
        if (!send_out(&out, now))
        {
            result = PLAY_HANGUP;
            break;
        }
        bytes += (long)out.len;

        /* The link's speed: what was answered for, trusted fully when more than before, slowly when less */
        if (now - control_at >= 250)
        {
            double sample = (double)g_acked / (double)(now - control_at);
            g_rate = sample > g_rate ? sample : g_rate * 0.95 + sample * 0.05;
            g_acked = 0;
            g_rtt_sum = g_rtt_n = 0;
            control_at = now;
        }

        if (log != NULL && now - stats_at >= 5000)
        {
            long ms = now - stats_at;
            fprintf(log, "c64: %ld frames run, %ld sent (%ld fps), %ld skipped, %ld KB/s, rtt floor %ld ms, link %.0f KB/s, "
                    "sound pieces %ld, late %ld, drive busy %ld frames (+%ld warp)\n", emu_frames, frames_sent,
                    frames_sent * 1000 / ms, frames_skipped, bytes * 1000 / ms / 1024, rtt_min(), g_rate * 1000 / 1024,
                    g_snd_pieces, emu_late, drive_frames, warp_frames);
            fflush(log);
            frames_sent = frames_skipped = bytes = emu_frames = emu_late = drive_frames = warp_frames = 0;
            g_snd_pieces = 0;
            stats_at = now;
        }
    }

    release_all(now_ms());
    if (g_shape)
        shape_flush(now_ms(), true);
    out.len = 0;
    if (caps->sound)
        apc_cmd(&out, "A;Flush;C=%d", SND_CH);
    if (caps->keys)
        out_str(&out, "\033[=2l\033[=1l");
    out_str(&out, "\033[0m\033[2J\033[H\033[?25h");
    write_all(out.data, out.len);
    core_stop();
    crash_guard(false);
    free(out.data);
    drain_input(200);
    if (log != NULL)
        fclose(log);
    return result;
}
