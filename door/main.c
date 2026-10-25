/*
 * main.c -- a Commodore 64 as a BBS door.
 *
 * The C64 is VICE's x64sc (a libretro core beside the door, core.c) with a real 1541 in drive 8. It switches on at the
 * BASIC prompt as soon as the caller has agreed to the ROM disclaimer; everything else is in its F12 menu (play.c):
 * a disk from the sysop's library, their own .D64 sent up by ZMODEM, the next disk, and the way back to the BBS.
 * Choosing a disk ends that C64 session with PLAY_RELOAD and this door starts the next one at once.
 *
 * For now it plays on terminals that draw JPEG XL through the CTerm APC commands (SyncTERM 1.2+): the C64 runs here
 * and its picture and sound go out (play.c). The TRACE path, where the C64 runs on the caller's own machine inside
 * TERMinator, is the next step and isn't built yet.
 *
 * Each session gets its own folder in /tmp: the disks it uses are copies, so a game that writes to its disk never
 * changes the library, and the folder is deleted when the door ends. Uploads are kept in players/<handle>-<number>/
 * beside the door instead, used in place, so they and the games saved on them are there next visit. The C64 itself runs in a child process, inside
 * bubblewrap when the box has it: it sees the door's folder read-only, its session folder, and nothing of /home, /tmp
 * or the network.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "apc.h"
#include "door.h"
#include "jxl_enc.h"
#include "play.h"
#include "session.h"

#define CSI "\033["

/* ---- where things are ---- */

static char g_door_dir[PATH_MAX];      /* the folder the door binary is in */
static char g_session[PATH_MAX];       /* this caller's own folder in /tmp */
static char g_player[96];              /* their handle and user number, for the log's name */
static char g_mine[PATH_MAX];          /* their kept disks, players/<player>; "" if it couldn't be made */

static void find_door_dir(void)
{
    ssize_t n = readlink("/proc/self/exe", g_door_dir, sizeof(g_door_dir) - 1);
    char *slash;
    if (n <= 0)
    {
        strcpy(g_door_dir, ".");
        return;
    }
    g_door_dir[n] = '\0';
    if ((slash = strrchr(g_door_dir, '/')) != NULL)
        *slash = '\0';
}

static void door_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", g_door_dir, name);
}

/* The Commodore ROMs aren't shipped with the door (they're still under copyright) and the core is built without its
 * own copies (vice-noroms.patch, build-core.sh), so the sysop supplies them in system/vice/, under the names VICE gives
 * them. Without one the core can't put the machine together and crashes, so the door looks before it starts: the
 * first one missing, or NULL. The 1581's is needed only for a .D81, but every ROM set has it, so it's asked for too. */
static const char *missing_rom(void)
{
    static const char *const roms[] = {
        "C64/basic-901226-01.bin", "C64/kernal-901227-03.bin", "C64/chargen-901225-01.bin",
        "DRIVES/dos1541ii-251968-03.bin", "DRIVES/dos1581-318045-02.bin",
    };
    char path[PATH_MAX];

    for (size_t i = 0; i < sizeof(roms) / sizeof(roms[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/system/vice/%s", g_door_dir, roms[i]);
        if (access(path, R_OK) != 0)
            return roms[i];
    }
    return NULL;
}

/* The handle cut down to plain characters, then the BBS user number, as the Doom door keeps its saves */
static void make_player_name(void)
{
    size_t n = 0;
    for (const char *p = door_info.handle; *p != '\0' && n < 40; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')
            g_player[n++] = *p;
    g_player[n] = '\0';
    if (n == 0)
        strcpy(g_player, "player");
    snprintf(g_player + strlen(g_player), sizeof(g_player) - strlen(g_player), "-%d", door_info.user_record);
}

/* ---- the session folder ---- */

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

static void session_cleanup(void)
{
    if (g_session[0] != '\0')
        nftw(g_session, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static void on_signal(int sig)
{
    session_cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

static bool session_create(void)
{
    strcpy(g_session, "/tmp/c64door-XXXXXX");
    if (mkdtemp(g_session) == NULL)
    {
        g_session[0] = '\0';
        return false;
    }
    atexit(session_cleanup);
    signal(SIGHUP, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    return true;
}

/* ---- screens ---- */

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;34m"
               "  ===============================================\r\n"
               CSI "1;37m"
               "              C O M M O D O R E   6 4\r\n"
               CSI "1;34m"
               "  ===============================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "              BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

static int press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key..." CSI "0m");
    return door_read_char();
}

/* The caller has to agree before the C64 is switched on: the ROMs are still under copyright */
static bool disclaimer(void)
{
    int c;

    title();
    door_write(CSI "1;33m  Before you switch it on\r\n\r\n" CSI "0;37m");
    door_write("  This door emulates a Commodore 64. It is for use only by people who\r\n"
               "  own a Commodore 64 or a licensed copy of its ROMs.\r\n\r\n"
               "  Upload only disk images you have the right to use. The disks you\r\n"
               "  upload are kept for your next visit until you delete them (F12, M).\r\n\r\n");
    door_write(CSI "1;37m  Press Y to agree, or any other key to go back. " CSI "0m");
    c = door_read_char();
    return c == 'y' || c == 'Y';
}

/* ---- the terminal (as the Doom door's main.c) ---- */

typedef struct
{
    bool cterm;
    int  cterm_major, cterm_minor;
    bool jxl, sound, keys;
    int  px_w, px_h;
    int  rows;          /* text rows, from where the cursor stops at 999;999 (0: no answer) */
} terminal_t;

static void detect_cterm(terminal_t *term)
{
    char reply[64];
    int n = 0, c;
    const char *p;

    door_write(CSI "c");
    c = door_read_char_timeout(500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        if (c == 'c' && n > 2)
            break;
        c = door_read_char_timeout(200);
    }
    reply[n] = '\0';
    p = strstr(reply, "[=67;84;101;114;109;");
    if (p != NULL && sscanf(p + strlen("[=67;84;101;114;109;"), "%d;%d", &term->cterm_major, &term->cterm_minor) == 2)
        term->cterm = true;
}

static bool cterm_at_least(const terminal_t *term, int major, int minor)
{
    return term->cterm && (term->cterm_major > major || (term->cterm_major == major && term->cterm_minor >= minor));
}

/*
 *   Q;JXL                          ESC [ = 1 ; 1 - n       JPEG XL pictures
 *   Q;libsndfile                   ESC [ = 7 ; 100 ; 1 n   sound files
 *   Q;libsndfileFormat;1;5         ESC [ = 7 ; 101 ; 1 ; 5 ; 1 n      WAV, 8-bit
 *   CSI < 0 c                      ESC [ < 0 ; ... c       its features: 8 = key presses and releases
 *   CSI ? 2 ; 1 S                  ESC [ ? 2 ; 0 ; w ; h S the screen in pixels
 */
static void detect_pixels(terminal_t *term)
{
    char reply[512];
    int n = 0, c;
    const char *p;

    if (!term->cterm)
        return;
    door_write(APC_PREFIX "Q;JXL" APC_END APC_PREFIX "Q;libsndfile" APC_END
               APC_PREFIX "Q;libsndfileFormat;1;5" APC_END CSI "<0c" CSI "?2;1S"
               CSI "s" CSI "999;999H" CSI "6n" CSI "u" CSI "5n");
    c = door_read_char_timeout(1500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        reply[n] = '\0';
        if (strstr(reply, "\033[0n") != NULL)
            break;
        c = door_read_char_timeout(500);
    }
    reply[n] = '\0';

    term->jxl = strstr(reply, "\033[=1;1-n") != NULL;
    term->sound = strstr(reply, "\033[=7;100;1n") != NULL && strstr(reply, "\033[=7;101;1;5;1n") != NULL;
    p = strstr(reply, "\033[<0");
    if (p != NULL)
        for (p += 4; *p == ';'; )
        {
            if (atoi(++p) == 8)
                term->keys = true;
            while (*p >= '0' && *p <= '9')
                p++;
        }
    p = strstr(reply, "\033[?2;0;");
    if (p == NULL || sscanf(p + 7, "%d;%d", &term->px_w, &term->px_h) != 2)
        term->px_w = 640, term->px_h = 400;
    /* The cursor position report from the bottom-right corner: the screen's rows */
    for (p = reply; (p = strstr(p, "\033[")) != NULL; p += 2)
    {
        int r, c;
        char end;
        if (sscanf(p, "\033[%d;%d%c", &r, &c, &end) == 3 && end == 'R' && r > 0 && r < 300)
        {
            term->rows = r;
            break;
        }
    }
}

/* ---- ZMODEM ---- */

static struct termios g_door_termios;

/* Runs rz or sz on the caller's connection in dir, and puts the door's terminal settings back afterwards */
static int run_transfer(const char *dir, char *const argv[])
{
    pid_t pid;
    int status = -1;

    tcgetattr(STDIN_FILENO, &g_door_termios);
    if ((pid = fork()) == 0)
    {
        if (chdir(dir) != 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &status, 0);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_door_termios);
    /* whatever the transfer left unread (its last packets, a cancel) isn't a menu choice */
    while (door_read_char_timeout(300) >= 0)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* On the way out, any disk a game wrote to can go home with the caller */
static void offer_changed_disks(void)
{
    disk_t ledger[64];
    int n = ledger_load(ledger, 64);

    for (int i = 0; i < n; i++)
    {
        char md5[33], line[200];
        int c;
        file_md5(ledger[i].path, md5);
        if (md5[0] == '\0' || strcmp(md5, ledger[i].md5) == 0)
            continue;
        title();
        snprintf(line, sizeof(line), CSI "1;37m  %s was written to\r\n\r\n" CSI "0;37m", ledger[i].name);
        door_write(line);
        door_write("  A game saved something on it. Download the changed disk now (ZMODEM)?\r\n"
                   "  It is deleted when you leave the door.\r\n\r\n");
        door_write(CSI "1;37m  Y = download, any other key = no " CSI "0m");
        c = door_read_char();
        if (c == 'y' || c == 'Y')
        {
            char *const argv[] = { "sz", "-b", ledger[i].path, NULL };
            door_write("\r\n\r\n  Starting the download...\r\n");
            run_transfer(g_session, argv);
        }
    }
}

/* ---- the C64 session, in a child process ---- */

static bool bwrap_works(void)
{
    static int known = -1;
    if (known < 0)
        known = system("bwrap --ro-bind / / --unshare-all --die-with-parent true >/dev/null 2>&1") == 0;
    return known == 1;
}

static play_result_t run_c64(const terminal_t *term)
{
    char self[PATH_MAX], core[PATH_MAX], system_dir[PATH_MAX], log[PATH_MAX], logs[PATH_MAX];
    char caps[64], deadline[32], link[32];
    char *argv[64];
    int argc = 0, kbps = 0, ping = 0, status;
    pid_t pid;
    FILE *f;

    snprintf(self, sizeof(self), "%s/c64door", g_door_dir);
    door_path(core, sizeof(core), "vice_x64sc_libretro.so");
    door_path(system_dir, sizeof(system_dir), "system");
    door_path(logs, sizeof(logs), "logs");
    mkdir(logs, 0755);
    snprintf(log, sizeof(log), "%s/%s.log", logs, g_player);

    /* A modelled slower link for one player: logs/<player>.linktest holding "kbps=300 ping=80" */
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.linktest", logs, g_player);
        if ((f = fopen(path, "r")) != NULL)
        {
            if (fscanf(f, "kbps=%d ping=%d", &kbps, &ping) < 1)
                kbps = 0;
            fclose(f);
        }
    }

    snprintf(caps, sizeof(caps), "%d,%d,%d,%d,%d,%d,%d", cterm_at_least(term, 1, 329), cterm_at_least(term, 1, 332),
             term->keys, term->sound, term->px_w, term->px_h, term->rows);
    snprintf(deadline, sizeof(deadline), "%ld", (long)(time(NULL) + door_time_remaining() * 60L));
    snprintf(link, sizeof(link), "%d,%d", kbps, ping);
    setenv("C64DOOR_SAVE_DIR", g_session, 1);         /* the core's own scratch space: writable, and private */
    setenv("C64DOOR_MINE", g_mine, 1);

    if (bwrap_works())
    {
        const char *box[] = { "bwrap", "--ro-bind", "/", "/", "--tmpfs", "/home", "--tmpfs", "/tmp", "--tmpfs", "/var/tmp",
                              "--tmpfs", "/run", "--ro-bind", g_door_dir, g_door_dir, "--bind", logs, logs,
                              "--bind", g_session, g_session, "--proc", "/proc", "--unshare-all", "--die-with-parent",
                              "--cap-drop", "ALL", "--" };
        for (size_t i = 0; i < sizeof(box) / sizeof(box[0]) - 1; i++)
            argv[argc++] = (char *)box[i];
        if (g_mine[0] != '\0')
        {
            argv[argc++] = "--bind";                    /* the kept disks: writable, inside the read-only door folder */
            argv[argc++] = g_mine;
            argv[argc++] = g_mine;
        }
        argv[argc++] = "--";
    }
    argv[argc++] = self;
    argv[argc++] = "--play";
    argv[argc++] = core;
    argv[argc++] = system_dir;
    argv[argc++] = g_session;
    argv[argc++] = caps;
    argv[argc++] = deadline;
    argv[argc++] = link;
    argv[argc++] = log;
    argv[argc] = NULL;

    if ((pid = fork()) == 0)
    {
        execvp(argv[0], argv);
        _exit(100 + PLAY_FAILED);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return PLAY_FAILED;
    if (WIFEXITED(status) && WEXITSTATUS(status) >= 100 && WEXITSTATUS(status) <= 100 + PLAY_RELOAD)
        return (play_result_t)(WEXITSTATUS(status) - 100);
    /* Killed by a signal (a crash: the C64 side put the terminal back first) */
    return PLAY_FAILED;
}

/* The child: c64door --play core system session blob,zoom,keys,sound,w,h deadline kbps,ping log */
static int play_main(int argc, char **argv)
{
    play_caps_t caps = { 0 };
    play_setup_t setup = { 0 };
    drive_t d;
    char m3u[600], door_dir[PATH_MAX], *slash;
    int b, z, k, s;

    if (argc < 9 || sscanf(argv[5], "%d,%d,%d,%d,%d,%d,%d", &b, &z, &k, &s, &caps.px_w, &caps.px_h, &caps.rows) < 6)
        return 100 + PLAY_FAILED;
    caps.blob = b, caps.zoom = z, caps.keys = k, caps.sound = s;
    snprintf(door_dir, sizeof(door_dir), "%s", argv[2]);
    if ((slash = strrchr(door_dir, '/')) != NULL)
        *slash = '\0';
    session_set_dirs(argv[4], door_dir);
    session_set_mine(getenv("C64DOOR_MINE"));

    /* What to boot with: drive.txt, which the session menu rewrites when a disk is chosen */
    drive_load(&d);
    setup.core = argv[2];
    setup.system = argv[3];
    setup.session = argv[4];
    setup.m3u = drive_write_m3u(&d, m3u, sizeof(m3u)) ? m3u : NULL;
    if (d.count > 0 && !d.autostart)
        setenv("C64DOOR_NO_AUTOSTART", "1", 1);
    setup.deadline = (time_t)atol(argv[6]);
    sscanf(argv[7], "%d,%d", &setup.link_kbps, &setup.link_ping);
    setup.log = argv[8];
    return 100 + (int)play(&caps, &setup);
}

/* ---- the door ---- */

static void how_to_play(const terminal_t *term)
{
    title();
    door_write(CSI "1;37m  How it works\r\n\r\n" CSI "0;37m");
    door_write("  The C64 switches on at the BASIC prompt. Press " CSI "1;37mF12" CSI "0;37m (or " CSI "1;37mCtrl-Q"
               CSI "0;37m) any time for\r\n  the menu: load a disk from the library, upload your own (kept for next\r\n"
               "  time), change disks, or leave.\r\n\r\n");
    if (term->keys)
        door_write("  Joystick: arrows (or the keypad) move, right Ctrl or Alt fires.\r\n"
                   "  Esc is RUN/STOP, Page Up is RESTORE, Home is CLR/HOME.\r\n");
    else
        door_write("  Joystick: arrows move, Space fires (your terminal can't report held\r\n"
                   "  keys, so fast games will be hard). Esc is RUN/STOP.\r\n");
    if (!term->sound)
        door_write(CSI "1;33m  Your terminal doesn't play sound files, so the C64 is silent.\r\n" CSI "0;37m");
    door_write(CSI "1;37m\r\n  Press any key to switch on. " CSI "0m");
}

static void goodbye(void)
{
    title();
    door_write(CSI "0;37m  Thanks for visiting. Your uploaded disks are kept for next time.\r\n\r\n");
    door_write("  The C64 here is VICE (x64sc), free software under the GNU GPL v2:\r\n"
               "  https://vice-emu.sourceforge.io\r\n");
    press_any_key();
}

int main(int argc, char **argv)
{
    terminal_t term = { 0 };
    char core[PATH_MAX];
    drive_t empty = { 0 };
    play_result_t r;

    if (argc > 1 && strcmp(argv[1], "--play") == 0)
        return play_main(argc, argv);

    find_door_dir();
    door_init(argc > 1 ? argv[1] : NULL);
    make_player_name();

    title();
    door_path(core, sizeof(core), "vice_x64sc_libretro.so");
    if (access(core, R_OK) != 0 || !jxl_enc_available() || !session_create())
    {
        door_write(CSI "1;33m  This door isn't installed properly. Please tell the sysop.\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 1;
    }
    if (missing_rom() != NULL)
    {
        char line[200];
        snprintf(line, sizeof(line), CSI "1;33m  This door isn't installed properly. Please tell the sysop.\r\n\r\n"
                 CSI "0;37m  (the C64's ROMs are missing: system/vice/%s)\r\n" CSI "0m", missing_rom());
        door_write(line);
        press_any_key();
        door_cleanup();
        return 1;
    }
    session_set_dirs(g_session, g_door_dir);
    drive_save(&empty);
    {
        char players[PATH_MAX];
        door_path(players, sizeof(players), "players");
        mkdir(players, 0700);
        snprintf(g_mine, sizeof(g_mine), "%s/%s", players, g_player);
        if (mkdir(g_mine, 0700) != 0 && errno != EEXIST)
            g_mine[0] = '\0';                           /* uploads then last this visit only */
    }

    detect_cterm(&term);
    detect_pixels(&term);
    if (!term.jxl)
    {
        title();
        door_write(CSI "1;37m  This door needs a terminal that draws pictures\r\n\r\n" CSI "0;37m");
        door_write("  The C64's screen is sent as JPEG XL pictures, which SyncTERM 1.2 or newer\r\n"
                   "  draws. Call back with SyncTERM to play.\r\n\r\n"
                   "  A version for TERMinator, where the C64 runs on your own computer, is\r\n"
                   "  on its way.\r\n");
        press_any_key();
        door_cleanup();
        return 0;
    }
    if (!disclaimer())
    {
        door_cleanup();
        return 0;
    }
    how_to_play(&term);
    if (door_read_char() < 0)
    {
        door_cleanup();
        return 0;
    }

    /* The C64, again each time the caller picks new disks, until they leave */
    while ((r = run_c64(&term)) == PLAY_RELOAD)
        ;
    if (r == PLAY_HANGUP)
    {
        door_cleanup();
        return 0;
    }
    if (r == PLAY_FAILED)
    {
        title();
        door_write(CSI "1;33m  The C64 couldn't start, or it crashed on that disk.\r\n" CSI "0;37m"
                   "  If it keeps happening, please tell the sysop.\r\n");
        press_any_key();
    }
    if (r == PLAY_TIMEOUT)
    {
        title();
        door_write(CSI "1;33m  Your time on the BBS is up.\r\n" CSI "0m");
        press_any_key();
    }
    else
    {
        offer_changed_disks();
        goodbye();
    }
    door_cleanup();
    return 0;
}
