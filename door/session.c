/*
 * session.c -- a caller's disks and the sysop's library, kept as files in the session folder. See session.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apc.h"
#include "session.h"

static char g_session[512], g_door[512], g_mine[512];

void session_set_dirs(const char *session_dir, const char *door_dir)
{
    snprintf(g_session, sizeof(g_session), "%s", session_dir);
    snprintf(g_door, sizeof(g_door), "%s", door_dir);
}

void session_set_mine(const char *dir)
{
    snprintf(g_mine, sizeof(g_mine), "%s", dir ? dir : "");
}

bool mine_available(void)
{
    return g_mine[0] != '\0';
}

static bool read_file(const char *path, unsigned char **data, size_t *size, size_t max)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL)
        return false;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > max || (*data = malloc((size_t)n)) == NULL)
    {
        fclose(f);
        return false;
    }
    *size = fread(*data, 1, (size_t)n, f);
    fclose(f);
    return *size == (size_t)n;
}

static bool write_file(const char *path, const unsigned char *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    bool ok;
    if (f == NULL)
        return false;
    ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

void file_md5(const char *path, char md5[33])
{
    unsigned char *data;
    size_t size;
    md5[0] = '\0';
    if (read_file(path, &data, &size, 4 * 1024 * 1024))
    {
        md5_hex(data, size, md5);
        free(data);
    }
}

/* What kind of image this is, by its size and header; NULL when it isn't one the C64 can use */
static const char *image_kind(const unsigned char *data, size_t size)
{
    if (size == 174848 || size == 175531 || size == 196608 || size == 197376)
        return "d64";
    if (size == 819200 || size == 822400)               /* a 1581's 3.5" disk; the core makes drive 8 a 1581 for it */
        return "d81";
    if (size > 12 && size <= 1024 * 1024 && memcmp(data, "GCR-1541", 8) == 0)
        return "g64";
    if (size > 64 && memcmp(data, "C64 CARTRIDGE   ", 16) == 0)
        return "crt";                                   /* a cartridge: in the port, not the drive, and running at once */
    if (size > 2 && size <= 65536)
        return "prg";
    return NULL;
}

/* A display name: the original file name, plain characters only, no '|' (the files' separator) */
static void clean_name(char *out, size_t size, const char *name)
{
    size_t n = 0;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (const char *p = base; *p != '\0' && n < size - 1; p++)
        out[n++] = (*p >= 32 && *p < 127 && *p != '|') ? *p : '_';
    out[n] = '\0';
}

static void path_in(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

void drive_load(drive_t *d)
{
    char path[600], line[700];
    FILE *f;

    memset(d, 0, sizeof(*d));
    path_in(path, sizeof(path), g_session, "drive.txt");
    if ((f = fopen(path, "r")) == NULL)
        return;
    if (fgets(line, sizeof(line), f) != NULL)
        d->autostart = strncmp(line, "autostart 1", 11) == 0;
    while (fgets(line, sizeof(line), f) != NULL && d->count < SESSION_MAX_DISKS)
    {
        char *bar = strchr(line, '|'), *e;
        if (bar == NULL)
            continue;
        *bar = '\0';
        for (e = bar + 1 + strlen(bar + 1); e > bar + 1 && (e[-1] == '\n' || e[-1] == '\r'); )
            *--e = '\0';
        snprintf(d->disk[d->count].path, sizeof(d->disk[0].path), "%s", line);
        snprintf(d->disk[d->count].name, sizeof(d->disk[0].name), "%s", bar + 1);
        d->count++;
    }
    fclose(f);
}

bool drive_save(const drive_t *d)
{
    char path[600];
    FILE *f;

    path_in(path, sizeof(path), g_session, "drive.txt");
    if ((f = fopen(path, "w")) == NULL)
        return false;
    fprintf(f, "autostart %d\n", d->autostart ? 1 : 0);
    for (int i = 0; i < d->count; i++)
        fprintf(f, "%s|%s\n", d->disk[i].path, d->disk[i].name);
    return fclose(f) == 0;
}

int ledger_load(disk_t *out, int max)
{
    char path[600], line[700];
    FILE *f;
    int n = 0;

    path_in(path, sizeof(path), g_session, "ledger.txt");
    if ((f = fopen(path, "r")) == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL && n < max)
    {
        char *a = strchr(line, '|'), *b;
        if (a == NULL || (b = strchr(a + 1, '|')) == NULL)
            continue;
        *a = *b = '\0';
        snprintf(out[n].path, sizeof(out[n].path), "%s", line);
        snprintf(out[n].name, sizeof(out[n].name), "%s", a + 1);
        snprintf(out[n].md5, sizeof(out[n].md5), "%.32s", b + 1);
        n++;
    }
    fclose(f);
    return n;
}

/* A disk image from a file: its bytes and kind. False with a reason. */
static bool load_image(const char *source, unsigned char **data, size_t *size, const char **kind, const char **why)
{
    if (!read_file(source, data, size, 2 * 1024 * 1024))
    {
        *why = "it couldn't be read, or it's too big for a C64 disk or cartridge";
        return false;
    }
    if ((*kind = image_kind(*data, *size)) == NULL)
    {
        free(*data);
        *why = "it isn't a .D64, .G64, .D81, .CRT or .PRG";
        return false;
    }
    return true;
}

static bool has_image_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".d64") == 0 || strcmp(name + n - 4, ".g64") == 0 ||
                     strcmp(name + n - 4, ".d81") == 0 || strcmp(name + n - 4, ".crt") == 0 ||
                     strcmp(name + n - 4, ".prg") == 0);
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int mine_list(char names[][64], int max)
{
    DIR *dir;
    struct dirent *e;
    int n = 0;

    if (!mine_available() || (dir = opendir(g_mine)) == NULL)
        return 0;
    while ((e = readdir(dir)) != NULL && n < max)
        if (e->d_name[0] != '.' && strlen(e->d_name) < 64 && has_image_ext(e->d_name))
            snprintf(names[n++], 64, "%s", e->d_name);
    closedir(dir);
    qsort(names, (size_t)n, 64, by_name);
    return n;
}

bool mine_add(drive_t *d, const char *source, const char *display, const char **why)
{
    unsigned char *data;
    size_t size;
    const char *kind;
    char base[64], name[64], path[600], have[MINE_MAX][64];
    struct stat st;
    disk_t *k;
    size_t n;

    if (!mine_available())
    {
        *why = "there's nowhere to keep it";
        return false;
    }
    if (d->count == SESSION_MAX_DISKS)
    {
        *why = "the drive list already has 8 disks";
        return false;
    }
    if (!load_image(source, &data, &size, &kind, why))
        return false;
    /* Its name with the extension the core goes by (it picks the drive from it) */
    clean_name(base, sizeof(base), display);
    n = strlen(base);
    if (n > 4 && base[n - 4] == '.')
        base[n - 4] = '\0';
    snprintf(name, sizeof(name), "%.55s.%s", base[0] != '\0' ? base : "disk", kind);
    snprintf(path, sizeof(path), "%s/%s", g_mine, name);
    if (stat(path, &st) != 0 && mine_list(have, MINE_MAX) >= MINE_MAX)
    {
        free(data);
        *why = "you keep 20 disks already (F12, M to delete one)";
        return false;
    }
    if (!write_file(path, data, size))
    {
        free(data);
        *why = "the BBS couldn't store it";
        return false;
    }
    k = &d->disk[d->count++];
    snprintf(k->path, sizeof(k->path), "%s", path);
    snprintf(k->name, sizeof(k->name), "%s", name);
    md5_hex(data, size, k->md5);
    free(data);
    return true;
}

bool mine_use(int index, drive_t *d)
{
    static char names[MINE_MAX][64];
    int n = mine_list(names, MINE_MAX);
    drive_t fresh = { 0 };

    if (index < 0 || index >= n)
        return false;
    for (int i = 0; i < n && fresh.count < SESSION_MAX_DISKS; i++)
    {
        disk_t *k = &fresh.disk[fresh.count++];
        const char *name = names[(index + i) % n];
        snprintf(k->path, sizeof(k->path), "%s/%s", g_mine, name);
        snprintf(k->name, sizeof(k->name), "%s", name);
    }
    *d = fresh;
    return true;
}

bool mine_delete(const char *name)
{
    char path[600];
    if (!mine_available() || strchr(name, '/') != NULL || name[0] == '.')
        return false;
    snprintf(path, sizeof(path), "%s/%s", g_mine, name);
    return remove(path) == 0;
}

bool disk_add(drive_t *d, const char *source, const char *display, const char **why)
{
    unsigned char *data;
    size_t size;
    const char *kind;
    disk_t *k;
    disk_t ledger[64];
    char path[600];
    FILE *f;
    int number = ledger_load(ledger, 64) + 1;

    if (d->count == SESSION_MAX_DISKS)
    {
        *why = "the drive list already has 8 disks";
        return false;
    }
    if (number > 64)
    {
        *why = "that's enough disks for one visit";
        return false;
    }
    if (!load_image(source, &data, &size, &kind, why))
        return false;
    k = &d->disk[d->count];
    snprintf(k->path, sizeof(k->path), "%s/disk%d.%s", g_session, number, kind);
    clean_name(k->name, sizeof(k->name), display);
    if (!write_file(k->path, data, size))
    {
        free(data);
        *why = "the BBS couldn't store it";
        return false;
    }
    md5_hex(data, size, k->md5);
    free(data);
    d->count++;

    path_in(path, sizeof(path), g_session, "ledger.txt");
    if ((f = fopen(path, "a")) != NULL)
    {
        fprintf(f, "%s|%s|%s\n", k->path, k->name, k->md5);
        fclose(f);
    }
    return true;
}

bool drive_write_m3u(const drive_t *d, char *path, size_t size)
{
    FILE *f;
    if (d->count == 0)
        return false;
    path_in(path, size, g_session, "disks.m3u");
    if ((f = fopen(path, "w")) == NULL)
        return false;
    for (int i = 0; i < d->count; i++)
        fprintf(f, "%s\n", d->disk[i].path);
    return fclose(f) == 0;
}

static void trim(char *s)
{
    char *e;
    while (*s == ' ')
        memmove(s, s + 1, strlen(s));
    for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r'); )
        *--e = '\0';
}

int library_load(title_t *titles, int max)
{
    char path[600], line[512];
    FILE *f;
    int n = 0;

    path_in(path, sizeof(path), g_door, "library/library.txt");
    if ((f = fopen(path, "r")) == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL && n < max)
    {
        char *a, *b;
        if (line[0] == '#' || (a = strchr(line, '|')) == NULL || (b = strchr(a + 1, '|')) == NULL)
            continue;
        *a = *b = '\0';
        snprintf(titles[n].title, sizeof(titles[n].title), "%s", line);
        snprintf(titles[n].about, sizeof(titles[n].about), "%s", a + 1);
        snprintf(titles[n].files, sizeof(titles[n].files), "%s", b + 1);
        trim(titles[n].title);
        trim(titles[n].about);
        trim(titles[n].files);
        n++;
    }
    fclose(f);
    return n;
}

bool library_use(const title_t *t, drive_t *d, const char **why)
{
    char list[256], *save = NULL;
    drive_t fresh = { 0 };

    snprintf(list, sizeof(list), "%s", t->files);
    for (char *f = strtok_r(list, ",", &save); f != NULL; f = strtok_r(NULL, ",", &save))
    {
        char path[600], rel[300];
        trim(f);
        if (*f == '\0' || strstr(f, "..") != NULL || strchr(f, '/') != NULL)
            continue;                                   /* library entries are plain file names */
        snprintf(rel, sizeof(rel), "library/%s", f);
        path_in(path, sizeof(path), g_door, rel);
        if (!disk_add(&fresh, path, f, why))
            return false;
    }
    if (fresh.count == 0)
    {
        *why = "its entry names no disk the door can find";
        return false;
    }
    fresh.autostart = true;
    *d = fresh;
    return true;
}
