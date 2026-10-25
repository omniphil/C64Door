/*
 * session.h -- a caller's disks and the sysop's library, shared by the door (outside the sandbox) and the C64 session
 * (inside it). Everything lives as files in the session folder, so both sides see the same thing:
 *
 *   drive.txt    what the C64 boots with: "autostart 0|1", then one "path|name" a line, the first in drive 8
 *   ledger.txt   every disk ever added this visit: "path|name|md5 as added", for the "download the changed disk?"
 *                offer on the way out (a disk can leave the drive and still have a saved game on it)
 *
 * The caller's kept disks are in their own folder beside the door, players/<handle>-<user number>/, bound writable into
 * the sandbox. Uploads go there and the drive uses them in place, so a game's saves are kept too.
 *
 * See session.c.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>

#define SESSION_MAX_DISKS 8
#define MINE_MAX          20    /* kept disks a caller may have */

typedef struct
{
    char path[512];             /* the copy in the session folder */
    char name[64];              /* what the caller sees */
    char md5[33];               /* as it went in */
} disk_t;

typedef struct
{
    bool   autostart;           /* the C64 loads and runs the first disk by itself */
    int    count;
    disk_t disk[SESSION_MAX_DISKS];
} drive_t;

typedef struct
{
    char title[48];
    char about[72];
    char files[256];            /* comma-separated, in library/ */
} title_t;

/* The session folder and the door's own folder, for every call below */
void session_set_dirs(const char *session_dir, const char *door_dir);

/* What the C64 boots with */
void drive_load(drive_t *d);
bool drive_save(const drive_t *d);

/* The caller's kept disks: their folder, "" for none (then uploads last this visit only) */
void session_set_mine(const char *dir);
bool mine_available(void);

/* The kept disks' file names, sorted. Returns how many (up to max). */
int mine_list(char names[][64], int max);

/* An upload into the kept disks (replacing one of the same name), and into the drive list as it is. False with a
 * reason, "full" included. */
bool mine_add(drive_t *d, const char *source, const char *display, const char **why);

/* Kept disk number index into drive 8, the ones after it next in the list (for D). False if it's gone. */
bool mine_use(int index, drive_t *d);

/* Deletes a kept disk by its file name */
bool mine_delete(const char *name);

/* Adds a disk image to the drive list as a copy in the session folder, and to the ledger. False with a reason. */
bool disk_add(drive_t *d, const char *source, const char *display, const char **why);

/* Every disk added this visit, for the way out. Returns how many (up to max). */
int ledger_load(disk_t *out, int max);

/* An .m3u of the drive list, for the core. False if the list is empty. */
bool drive_write_m3u(const drive_t *d, char *path, size_t size);

/* The library: library/library.txt beside the door, "title | description | disk1.d64,disk2.d64" a line */
int library_load(title_t *titles, int max);

/* A library title's disks, replacing the drive list, set to start by itself. False with a reason. */
bool library_use(const title_t *t, drive_t *d, const char **why);

/* MD5 of a file, "" if unreadable */
void file_md5(const char *path, char md5[33]);

#endif
