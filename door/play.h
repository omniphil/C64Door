/*
 * play.h -- a C64 session on a CTerm terminal (SyncTERM): the C64 runs here and its picture goes out as JPEG XL, its
 * sound as short WAV pieces. See play.c.
 */

#ifndef PLAY_H
#define PLAY_H

#include <stdbool.h>
#include <time.h>

typedef struct
{
    bool blob;              /* pictures can travel inside the command (CTerm 1.329+), not only through the cache */
    bool zoom;              /* the terminal scales pictures up itself (ZX/ZY, CTerm 1.332+) */
    bool keys;              /* the terminal reports key presses and releases (CSI = 1 h) */
    bool sound;             /* it plays WAV files */
    int  px_w, px_h;        /* the screen in pixels */
    int  rows;              /* the screen in text rows (0: not known); the last one holds the controls */
} play_caps_t;

typedef struct
{
    const char *core;       /* vice_x64sc_libretro.so */
    const char *system;     /* the core's system folder */
    const char *m3u;        /* the session's disks, the first in drive 8; NULL boots to BASIC */
    const char *session;    /* the caller's session folder (drive.txt, uploads) */
    const char *log;        /* where the session's numbers go, or NULL */
    time_t deadline;        /* when the caller's time on the BBS runs out */
    int link_kbps, link_ping;   /* a modelled slower link for testing (0: none) */
} play_setup_t;

typedef enum
{
    PLAY_QUIT,              /* the caller chose to leave the C64 */
    PLAY_HANGUP,            /* the connection went */
    PLAY_TIMEOUT,           /* their time ran out */
    PLAY_FAILED,            /* the C64 couldn't start */
    PLAY_RELOAD             /* new disks were chosen: start the C64 again with drive.txt */
} play_result_t;

play_result_t play(const play_caps_t *caps, const play_setup_t *setup);

#endif
