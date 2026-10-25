/*
 * core.h -- the C64 itself: VICE's x64sc as a libretro core, loaded from vice_x64sc_libretro.so beside the door.
 * See core.c.
 */

#ifndef C64_CORE_H
#define C64_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define CORE_W 384                  /* the core's PAL frame, borders included */
#define CORE_H 272
#define CORE_SCREEN_X 32            /* where the 320x200 screen sits inside it */
#define CORE_SCREEN_Y 35

/* Joystick bits, as the RetroPad has them */
#define JOY_UP    (1u << 4)
#define JOY_DOWN  (1u << 5)
#define JOY_LEFT  (1u << 6)
#define JOY_RIGHT (1u << 7)
#define JOY_FIRE  (1u << 0)

/* Loads the core and starts it with the disks listed in m3u (an .m3u file of image paths, the first in drive 8), or
 * with no disk when m3u is NULL. system_dir is the core's system folder. False with a reason in *why on failure. */
bool core_start(const char *core_path, const char *system_dir, const char *m3u, const char **why);

/* One frame of C64 time (1/50 s). Afterwards the newest picture is in core_rgb() and its sound in core_audio(). */
void core_run_frame(void);

/* Whether the last frame's sound was silence (as a disk loads) */
bool core_frame_silent(void);

/* Throws away the sound made so far (frames run at warp speed) */
void core_audio_drop(void);

/* The newest picture, RGB24, CORE_W x CORE_H. */
const uint8_t *core_rgb(void);

/* The frame rate and the sound's sample rate the core runs at. */
double core_fps(void);
double core_sample_rate(void);

/* Takes up to max stereo frames of the sound made so far (S16 interleaved). Returns how many. */
int core_audio(int16_t *out, int max);

/* Keys, as libretro key codes (RETROK_*), pressed or let go. */
void core_key(unsigned retrok, bool down);

/* The joystick, as JOY_* bits. */
void core_joystick(unsigned bits);

/* Which of the C64's two joystick ports the joystick is in (1 or 2; 2 at the start, as most games want). */
void core_joystick_port(int port);

/* The disks: how many, which is in the drive, and swapping to another (eject, change, insert, as a caller would). */
int core_disk_count(void);
int core_disk_index(void);
void core_disk_insert(int index);

/* The drive's red light: on while it reads or writes */
bool core_drive_led(void);

/* A cold reset of the C64, the disks staying as they are. */
void core_reset(void);

/* Flushes the drive's writes to the image files and unloads the core. */
void core_stop(void);

#endif
