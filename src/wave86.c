/*
 * wave86.c - main program.
 *
 * Launch strategy: WAVE86 never spawns the game itself (that would pin
 * ~100K of launcher in memory under the game). Instead it writes
 * RUNGAME.BAT and exits; the WAVE.BAT wrapper runs the game and then
 * restarts the menu. Games get every byte of conventional memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <bios.h>
#include <i86.h>
#include "wave86.h"

/* ui.c */
void ui_static(void);
void ui_list(int sel, int top);
void ui_details(int sel);
void ui_keybar(const Game *sel);
void ui_status(const char *msg);
void ui_music_tick(void);
void ui_music_volshow(void);
void ui_edit_field(const char *text);
void ui_idle(int sel);
void ui_key_seen(void);
int ui_picture_shown(void);
int ui_picture(const Game *g);

#define K_UP    0x4800
#define K_DOWN  0x5000
#define K_PGUP  0x4900
#define K_PGDN  0x5100
#define K_HOME  0x4700
#define K_END   0x4F00

static char launcher_dir[PATH_LEN];     /* cwd at start: where to return */
char home_dir[PATH_LEN];                /* EXE directory: INI and MUSIC\ */
static int opt_dump = 0;
static int opt_dumpidle = 0;
static int cur_sel = 0;
static int idle_pictures = 1;
static const char *opt_dumpsel = NULL;
static int opt_mustest = 0;
static int opt_diag = 0;
static const char *opt_launch = NULL;
static int opt_name = 0;             /* argv index of /name */

/* music.c / mod.c internals exposed for the self-test */
extern volatile int mus_cseg;
extern volatile unsigned mus_coff;
extern int mus_loops;
extern FILE *mod_dumpf;

static unsigned getkey(void)
{
    unsigned k;
    while (!_bios_keybrd(_KEYBRD_READY)) {
        mus_poll();                 /* idle: keep the playlist moving */
        ui_music_tick();            /* ... and the VU meter bouncing */
        if (idle_pictures)
            ui_idle(cur_sel);       /* ... and the picture after 5 s */
    }
    k = _bios_keybrd(_KEYBRD_READ);
    ui_key_seen();
    if (ui_picture_shown())
        ui_details(cur_sel);        /* any key: back to the details */
    if ((k & 0xFF) == 0 || (k & 0xFF) == 0xE0)
        return k & 0xFF00;          /* extended key: scan code only */
    return k & 0x00FF;              /* ascii */
}

static void write_bat(const Game *g, int use_setup)
{
    FILE *f = fopen("RUNGAME.BAT", "w");
    const char *prog = use_setup ? g->setup : g->exe;
    const char *dot = strrchr(prog, '.');
    int is_bat = dot && stricmp(dot + 1, "BAT") == 0;

    if (!f)
        return;
    fprintf(f, "@echo off\n");
    fprintf(f, "%c:\n", gamedir[0]);
    fprintf(f, "cd %s\\%s\n", gamedir, g->dir);
    ini_emit_extras(f, g->dir, 0);     /* sound mode, env, pre */
    fprintf(f, "%s%s", is_bat ? "call " : "", prog);
    if (!use_setup && g->args[0])
        fprintf(f, " %s", g->args);
    fprintf(f, "\n");
    ini_emit_extras(f, g->dir, 1);     /* post */
    fprintf(f, "%c:\n", launcher_dir[0]);
    fprintf(f, "cd %s\n", launcher_dir);
    fclose(f);
}

static void redraw(int sel, int top);

/* type a new display name for the selected game; Enter saves it to the
   INI and re-sorts, Esc leaves things alone */
static void edit_name(int *sel, int *top)
{
    char buf[NAME_LEN], dir[FN_LEN], oldname[NAME_LEN];
    unsigned len;

    strcpy(buf, games[*sel].name);
    strcpy(oldname, buf);
    strcpy(dir, games[*sel].dir);
    len = strlen(buf);
    ui_status("TYPE THE NAME. ENTER SAVES, ESC CANCELS.");
    idle_pictures = 0;
    for (;;) {
        unsigned k;
        ui_edit_field(buf);
        k = getkey();
        if (k == 0x1B)
            break;
        if (k == 0x0D) {
            if (buf[0] && strcmp(buf, oldname)) {
                int i = find_game(dir);
                if (i >= 0) strcpy(games[i].name, buf);
                ini_write_name(dir, buf);
                sort_games();
                i = find_game(dir);
                *sel = i < 0 ? 0 : i;
                if (*sel < *top || *sel >= *top + 14)
                    *top = *sel > 6 ? *sel - 6 : 0;
                if (*top > game_count - 14) *top = game_count - 14;
                if (*top < 0) *top = 0;
            }
            break;
        }
        if (k == 0x08) {
            if (len) buf[--len] = 0;
        } else if (k >= 32 && k < 127 && len < NAME_LEN - 1) {
            buf[len++] = (char)k;
            buf[len] = 0;
        }
    }
    idle_pictures = 1;
    redraw(*sel, *top);
}

static void redraw(int sel, int top)
{
    ui_static();
    ui_list(sel, top);
    ui_details(sel);
    ui_keybar(game_count ? &games[sel] : NULL);
    ui_status(NULL);
}

static void text_mode_plain(void)
{
    union REGS r;
    r.x.ax = 0x0003;                /* text mode: also restores palette */
    int86(0x10, &r, &r);
}

static void quit(void)
{
    mus_shutdown();                 /* unhook timer, silence cards */
    text_mode_plain();
    printf("WAVE86 closed. Type WAVE to restart.\n");
    exit(0);
}

/*
 * Under WAVE.BAT (it sets WAVE86=LAUNCH) we hand the game to the batch
 * loop and exit, so it gets every byte of memory. Started bare, we free
 * the music buffers and run it ourselves, then come back to the menu.
 */
static void launch(const Game *g, int use_setup)
{
    const char *what = use_setup ? g->setup : g->name;

    write_bat(g, use_setup);
    if (getenv("WAVE86")) {
        mus_shutdown();
        text_mode_plain();
        printf("WAVE86: running %s ...\n", what);
        exit(0);
    }

    mus_release();
    text_mode_plain();
    printf("WAVE86: running %s ...\n", what);
    printf("(type WAVE instead to give games all memory)\n");
    system("RUNGAME.BAT");
    remove("RUNGAME.BAT");

    vid_text_mode();
    vid_set_palette();
    mus_init();
}

int main(int argc, char **argv)
{
    int sel = 0, top = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "/dump") == 0) {
            opt_dump = 1;           /* optional: /dump DIR preselects a game */
            if (i + 1 < argc && argv[i + 1][0] != '/')
                opt_dumpsel = argv[i + 1];
            if (i + 2 < argc && stricmp(argv[i + 2], "idle") == 0)
                opt_dumpidle = 1;   /* as if left alone for 5 s */
        }
        if (stricmp(argv[i], "/nopal") == 0) opt_nopal = 1;
        if (stricmp(argv[i], "/mustest") == 0) {
            opt_mustest = 5;            /* seconds, optional argument */
            if (i + 1 < argc && atoi(argv[i + 1]) > 0)
                opt_mustest = atoi(argv[i + 1]);
        }
        if (stricmp(argv[i], "/launch") == 0 && i + 1 < argc)
            opt_launch = argv[i + 1];   /* boot straight into a game */
        if (stricmp(argv[i], "/name") == 0 && i + 2 < argc)
            opt_name = i;               /* /name DIR New Name Words */
        if (stricmp(argv[i], "/diag") == 0)
            opt_diag = 1;
    }

    remove("RUNGAME.BAT");
    getcwd(launcher_dir, PATH_LEN);

    /* DOS hands us our full path in argv[0]: resources live beside it,
       so "wave" works from anywhere on the PATH */
    strcpy(home_dir, launcher_dir);
    {
        const char *bs = strrchr(argv[0], '\\');
        if (bs && bs - argv[0] < PATH_LEN - 1) {
            unsigned n = (unsigned)(bs - argv[0]);
            if (n == 2 && argv[0][1] == ':') n = 3;   /* keep "C:\" */
            memcpy(home_dir, argv[0], n);
            home_dir[n] = 0;
        }
    }
    {
        char path[PATH_LEN + 12];
        sprintf(path, "%s%sWAVE86.INI", home_dir,
                home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\");
        ini_load(path);
    }
    {
        char abs[PATH_LEN], rel[PATH_LEN + 8];
        const char *src = gamedir;
        /* relative gamedir is relative to the EXE, not the cwd */
        if (!(gamedir[1] == ':' || gamedir[0] == '\\')) {
            sprintf(rel, "%s%s%s", home_dir,
                    home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\",
                    gamedir);
            src = rel;
        }
        if (_fullpath(abs, src, PATH_LEN))
            strcpy(gamedir, abs);
        /* strip trailing backslash (root stays "C:\") */
        i = strlen(gamedir);
        if (i > 3 && gamedir[i - 1] == '\\')
            gamedir[i - 1] = 0;
    }

    vid_detect();
    scan_games();
    if (!opt_launch)
        cpu_identify();             /* ~0.25 s: measures the clock */

    if (opt_name) {
        char nm[NAME_LEN] = "";
        for (i = opt_name + 2; i < argc; i++) {
            if (nm[0] && strlen(nm) < NAME_LEN - 2) strcat(nm, " ");
            strncat(nm, argv[i], NAME_LEN - 1 - strlen(nm));
        }
        if (ini_write_name(argv[opt_name + 1], nm) == 0)
            printf("WAVE86: %s is now \"%s\"\n", argv[opt_name + 1], nm);
        else
            printf("WAVE86: could not update the INI\n");
        return 0;
    }

    if (opt_diag) {
        union REGS r;
        r.h.ah = 0x30;
        int86(0x21, &r, &r);
        printf("WAVE86 %s diagnostics\n", VERSION_STR);
        printf("DOS    : %d.%02d, %uK largest free block\n",
               r.h.al, r.h.ah, dos_free_kb());
        printf("Video  : %s\n", vid_is_vga ? "VGA" :
               (vid_is_color ? "CGA/EGA" : "mono"));
        printf("Started: %s, in %s\n",
               getenv("WAVE86") ? "by WAVE.BAT" : "bare (will run games in place)",
               launcher_dir);
        printf("Games  : %d under %s\n", game_count, gamedir);
        mus_init();
        mus_diag();
        mus_shutdown();
        return 0;
    }

    if (opt_launch) {
        for (i = 0; i < game_count; i++)
            if (stricmp(games[i].dir, opt_launch) == 0) {
                launch(&games[i], 0);   /* returns only when run bare */
                return 0;
            }
        printf("WAVE86: no game in directory '%s'\n", opt_launch);
        return 1;
    }

    if (opt_mustest) {
        /* play ~5 s headless, then report the player position */
        unsigned long __far *ticks =
            (unsigned long __far *)MK_FP(0x40, 0x6C);
        unsigned long t0;
        mod_dumpf = fopen("MODDUMP.RAW", "wb");
        mus_init();                     /* starts silent ... */
        mus_toggle();                   /* ... then the test presses M */
        t0 = *ticks;
        while (*ticks - t0 < (unsigned long)opt_mustest * 182 / 10)
            mus_poll();
        printf("mustest: present=%d tracks=%d track=%s kind=%d "
               "seg=%d off=%u vol=%d loops=%d\n",
               mus_present, mus_ntracks, mus_track, mus_kind,
               mus_cseg, mus_coff, mus_vol, mus_loops);
        printf("mod: cpu=%d sb=%d rate=%u irqs=%u playing=%d "
               "order=%d row=%d\n",
               cpu_level(), sb_present, sb_rate, mod_irqs(),
               mod_playing, mod_order, mod_row);
        mus_shutdown();
        if (mod_dumpf) fclose(mod_dumpf);
        mod_dumpf = NULL;
        return 0;
    }

    mus_init();
    if (opt_dumpsel) {
        i = find_game(opt_dumpsel);
        if (i >= 0) { sel = i; top = sel > 13 ? sel - 13 : 0; }
    }
    vid_text_mode();
    vid_set_palette();
    redraw(sel, top);

    if (opt_dump) {
        if (opt_dumpidle && game_count)
            ui_picture(&games[sel]);
        scr_dump("SCREEN.BIN", "FONT.BIN", "PAL.BIN");
        quit();
    }

    ui_key_seen();
    for (;;) {
        unsigned k;
        int old = sel;

        cur_sel = sel;
        k = getkey();

        switch (k) {
        case K_UP:   sel--; break;
        case K_DOWN: sel++; break;
        case K_PGUP: sel -= 14; break;
        case K_PGDN: sel += 14; break;
        case K_HOME: sel = 0; break;
        case K_END:  sel = game_count - 1; break;
        case 0x3C00:                    /* F2: rename in place */
            if (game_count) {
                edit_name(&sel, &top);
                old = -1;               /* force a list refresh */
            }
            break;
        case 0x1B:
            quit();
        case 0x0D:
            if (game_count) {
                launch(&games[sel], 0);
                redraw(sel, top);       /* back from a bare-mode run */
            }
            break;
        case 's': case 'S':
            if (game_count && games[sel].setup[0]) {
                launch(&games[sel], 1);
                redraw(sel, top);
            }
            break;
        case 'r': case 'R':
            scan_games();
            sel = 0; top = 0;
            redraw(sel, top);
            break;
        case 'm': case 'M':
            mus_toggle();
            ui_status(NULL);
            break;
        case '+': case '=':
            mus_volume(1);
            ui_music_volshow();
            break;
        case '-': case '_':
            mus_volume(-1);
            ui_music_volshow();
            break;
        case '.': case '>':
            mus_skip(1);
            ui_status(NULL);
            break;
        case ',': case '<':
            mus_skip(-1);
            ui_status(NULL);
            break;
        default:
            /* letter jump */
            if (k >= 'a' && k <= 'z') k -= 32;
            if (k >= 'A' && k <= 'Z' && game_count) {
                for (i = 1; i <= game_count; i++) {
                    int gi = (sel + i) % game_count;
                    char c = games[gi].name[0];
                    if (c >= 'a' && c <= 'z') c -= 32;
                    if ((unsigned)c == k) { sel = gi; break; }
                }
            }
            break;
        }

        if (game_count == 0)
            continue;
        if (sel < 0) sel = 0;
        if (sel >= game_count) sel = game_count - 1;
        if (sel < top) top = sel;
        if (sel >= top + 14) top = sel - 13;

        if (sel != old) {
            ui_list(sel, top);
            ui_details(sel);
            ui_keybar(&games[sel]);
        }
    }
}
