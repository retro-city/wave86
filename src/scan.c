/*
 * scan.c - find games under the games root and guess how to start them.
 */
#include <dos.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include "wave86.h"

Game games[MAX_GAMES];
int game_count = 0;
char gamedir[PATH_LEN] = "GAMES";

/* programs that are never the game itself (basename, no extension) */
static const char *blacklist[] = {
    "SETUP", "INSTALL", "SETSOUND", "SOUNDSET", "SETSND", "SETMAIN", "CONFIG",
    "UVCONFIG", "AUTODET", "DOS4GW", "DOS4G", "MPSCOPY", "PKUNZJR",
    "PKUNZIP", "SOUND", "SOUNDRV", "MUSIC", "INTRO", "CATALOG",
    "DEALERS", "ORDER", "HELPME", "README", "UPDATE", "UNINST",
    "SWCBBS", "VESA", "UNIVBE", "CRACK", "UFOCRACK", "FILE_ID",
    NULL
};

/* batch/exe names that usually mean "start the game" */
static const char *starters[] = { "START", "PLAY", "GO", "RUN", "GAME", NULL };

static int in_list(const char *base, const char **list)
{
    int i;
    for (i = 0; list[i]; i++)
        if (stricmp(base, list[i]) == 0)
            return 1;
    return 0;
}

static void split_name(const char *fn, char *base, char *ext)
{
    const char *dot = strrchr(fn, '.');
    if (dot) {
        unsigned n = (unsigned)(dot - fn);
        if (n > 8) n = 8;
        memcpy(base, fn, n);
        base[n] = 0;
        strncpy(ext, dot + 1, 3);
        ext[3] = 0;
    } else {
        strncpy(base, fn, 8);
        base[8] = 0;
        ext[0] = 0;
    }
}

/* score a candidate executable for directory dirname */
static int score_exe(const char *fn, const char *dirname)
{
    char base[9], ext[4];
    int s = 0;
    unsigned bl, dl;

    split_name(fn, base, ext);

    if (in_list(base, blacklist))
        return -1;

    if (stricmp(base, dirname) == 0)
        s += 100;
    else {
        bl = strlen(base);
        dl = strlen(dirname);
        if (bl >= 3 && dl >= 3) {
            if (strnicmp(base, dirname, bl < dl ? bl : dl) == 0)
                s += 60;
        }
    }
    if (in_list(base, starters))
        s += 45;

    if (stricmp(ext, "EXE") == 0) s += 15;
    else if (stricmp(ext, "BAT") == 0) s += 12;
    else if (stricmp(ext, "COM") == 0) s += 10;

    return s;
}

/* pick setup program by priority */
static int setup_rank(const char *fn)
{
    char base[9], ext[4];
    split_name(fn, base, ext);
    if (stricmp(base, "SETUP") == 0)
        return stricmp(ext, "EXE") == 0 ? 5 : 4;
    if (stricmp(base, "SETSND") == 0)   return 4;    /* Virgin/Disney games */
    if (stricmp(base, "INSTALL") == 0)  return 3;
    if (stricmp(base, "SETSOUND") == 0) return 2;
    if (stricmp(base, "SOUNDSET") == 0) return 2;
    if (stricmp(base, "CONFIG") == 0)   return 1;
    return 0;
}

static int has_ext(const char *fn, const char *ext)
{
    const char *dot = strrchr(fn, '.');
    return dot && stricmp(dot + 1, ext) == 0;
}

static void scan_one(Game *g)
{
    char pat[PATH_LEN + 16];
    struct find_t ft;
    unsigned rc;
    char best_exe[FN_LEN] = "";
    int best_score = -1;
    char best_setup[FN_LEN] = "";
    int best_setup_rank = 0;

    sprintf(pat, "%s\\%s\\*.*", gamedir, g->dir);
    rc = _dos_findfirst(pat, _A_NORMAL | _A_RDONLY | _A_ARCH, &ft);
    while (rc == 0) {
        const char *fn = ft.name;
        if (fn[0] != '.' && fn[0] != '_') {
            if (has_ext(fn, "EXE") || has_ext(fn, "COM") ||
                has_ext(fn, "BAT")) {
                char base[9], ext[4];
                int sr, sc;

                split_name(fn, base, ext);
                if (stricmp(base, "DOS4GW") == 0)
                    g->flags |= GF_DOS4GW;

                sr = setup_rank(fn);
                if (sr > best_setup_rank) {
                    best_setup_rank = sr;
                    strcpy(best_setup, fn);
                }
                sc = score_exe(fn, g->dir);
                if (sc > best_score ||
                    (sc == best_score && sc >= 0 &&
                     stricmp(fn, best_exe) < 0)) {
                    if (sc >= 0) {
                        best_score = sc;
                        strcpy(best_exe, fn);
                    }
                }
            }
        }
        rc = _dos_findnext(&ft);
    }

    strcpy(g->exe, best_exe);
    strcpy(g->setup, best_setup);
}

static int cmp_games(const void *a, const void *b)
{
    return stricmp(((const Game *)a)->name, ((const Game *)b)->name);
}

int scan_games(void)
{
    char pat[PATH_LEN + 8];
    struct find_t ft;
    unsigned rc;

    game_count = 0;

    sprintf(pat, "%s\\*.*", gamedir);
    rc = _dos_findfirst(pat, _A_SUBDIR, &ft);
    while (rc == 0 && game_count < MAX_GAMES) {
        if ((ft.attrib & _A_SUBDIR) &&
            ft.name[0] != '.' && ft.name[0] != '_') {
            Game *g = &games[game_count];
            memset(g, 0, sizeof(*g));
            strncpy(g->dir, ft.name, FN_LEN - 1);
            strncpy(g->name, ft.name, NAME_LEN - 1);
            scan_one(g);
            if (g->exe[0])
                game_count++;
        }
        rc = _dos_findnext(&ft);
    }

    ini_apply();

    /* drop hidden entries */
    {
        int i, n = 0;
        for (i = 0; i < game_count; i++)
            if (!(games[i].flags & GF_HIDE))
                games[n++] = games[i];
        game_count = n;
    }

    qsort(games, game_count, sizeof(Game), cmp_games);
    return game_count;
}
