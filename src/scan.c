/*
 * scan.c - find games under the games root and guess how to start them.
 */
#include <dos.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include "wave86.h"

Game games[MAX_GAMES];
int game_count = 0;
char gamedir[PATH_LEN] = "GAMES";

/* programs that are never the game itself (basename, no extension) */
static const char *blacklist[] = {
    "SETUP", "INSTALL", "IMGMOUNT", "SETSOUND", "SOUNDSET", "SETSND", "SETMAIN", "CONFIG",
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

unsigned scan_sizes_missing = 0;    /* programs the listing showed as 0 bytes (/diag) */
unsigned scan_names_fixed = 0;      /* names that came back in raw 11-character form (/diag) */
unsigned scan_probed_dirs = 0;      /* folders the listing did not mark as folders (/diag) */
unsigned scan_exe_from_ini = 0;     /* programs the listing did not show, the INI knew (/diag) */

/* A name as a listing gives it back. DOS itself gives NAME.EXT; a
   redirector drive may hand over the raw 11 characters of the entry,
   "KEEN4   EXE", or pad around the dot. Made into NAME.EXT here, and
   counted, so the scan and /diag both see what is there. */
static int copy_part(char *dst, const char *src, int max)
{
    int n = 0, keep = 0;
    while (n < max && src[n] && src[n] != '.') {
        dst[n] = src[n];
        if (src[n] != ' ') keep = n + 1;
        n++;
    }
    dst[keep] = 0;
    return n;                           /* characters looked at */
}

void scan_fixname(char *name)
{
    char out[13];
    const char *dot = strchr(name, '.');
    int n, b;

    if (name[0] == '.')                 /* . and .. */
        return;
    n = copy_part(out, name, 8);
    if (dot) n = (int)(dot - name) + 1;
    else if (strlen(name) <= 8) n = 0;  /* no extension at all */
    if (n) {
        b = strlen(out);
        out[b] = '.';
        copy_part(out + b + 1, name + n, 3);
        if (!out[b + 1]) out[b] = 0;
    }
    if (strcmp(out, name) != 0) {
        scan_names_fixed++;
        strcpy(name, out);
    }
}

/* what a game folder is listed with: everything but the volume label */
#define SCAN_ATTRS (_A_NORMAL | _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_SUBDIR | _A_ARCH)

static void scan_one(Game *g)
{
    char pat[PATH_LEN + 16];
    struct find_t ft;
    unsigned rc;
    char best_exe[FN_LEN] = "";
    int best_score = -1;
    char best_setup[FN_LEN] = "";
    int best_setup_rank = 0;

    /* every attribute is asked for and folders and labels skipped here: a
       redirector drive that reads the mask its own way still lists the
       programs. An empty EXE cannot run: a download that is still
       incomplete on the server leaves such files behind. */
    sprintf(pat, "%s\\%s\\*.*", gamedir, g->dir);
    rc = _dos_findfirst(pat, SCAN_ATTRS, &ft);
    while (rc == 0) {
        const char *fn = ft.name;
        scan_fixname(ft.name);
        if (!(ft.attrib & (_A_SUBDIR | _A_VOLID)) &&
            fn[0] != '.' && fn[0] != '_' &&
            (has_ext(fn, "EXE") || has_ext(fn, "COM") || has_ext(fn, "BAT"))) {
            unsigned long size = ft.size;
            if (size == 0) {            /* a redirector drive (the card's SD through
                                           PMDFS) may list no size: ask the file */
                int h;
                sprintf(pat, "%s\\%s\\%s", gamedir, g->dir, fn);
                if ((h = open(pat, O_RDONLY | O_BINARY)) >= 0) {
                    size = filelength(h);
                    close(h);
                }
                scan_sizes_missing++;
            }
            if (size != 0) {
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

    /* a CD image next to the game (CD\*.ISO) is mounted while it runs */
    sprintf(pat, "%s\\%s\\CD\\*.ISO", gamedir, g->dir);
    if (_dos_findfirst(pat, _A_NORMAL | _A_RDONLY | _A_ARCH, &ft) == 0) {
        scan_fixname(ft.name);
        sprintf(g->cdimg, "CD\\%s", ft.name);
    }
    /* or a start batch that mounts it itself (IMGMOUNT.BAT). Whether that
       disc is on this machine or still on the server is the difference
       between the CD and NET CD tags, and the batch is what knows. */
    sprintf(pat, "%s\\%s\\IMGMOUNT.BAT", gamedir, g->dir);
    if (_dos_findfirst(pat, _A_NORMAL | _A_RDONLY | _A_ARCH, &ft) == 0) {
        char buf[160];
        int h, n, keep = 0;
        g->flags |= GF_CDBAT;
        sprintf(pat, "%s\\%s\\IMGMOUNT.BAT", gamedir, g->dir);
        if ((h = open(pat, O_RDONLY | O_BINARY)) >= 0) {
            while ((n = read(h, buf + keep, 128)) > 0) {
                n += keep;
                buf[n] = 0;
                if (strstr(buf, "NETDRIVE")) {
                    g->flags |= GF_NETCD;
                    break;
                }
                keep = n < 12 ? n : 12;
                memmove(buf, buf + n - keep, keep);
            }
            close(h);
        }
    }
}

/*
 * Delete a folder and everything in it (Del in the games list). One path
 * buffer serves the whole walk, grown and cut back at each level: the
 * stack is 2K, and a find_t per level is all it can afford. DOS carries
 * on a findnext across deletions in the same folder. Read-only files are
 * made plain first, as the eXoDOS zips leave a few. Stops RM_LEVELS deep,
 * which no game folder reaches. Returns 0 when the folder is gone.
 */
#define RM_LEVELS 12
static char rm_path[PATH_LEN * 2];

static int rm_level(unsigned len, int depth)
{
    struct find_t ft;
    unsigned rc;

    if (depth > RM_LEVELS || len + 14 >= sizeof(rm_path))
        return 1;
    strcpy(rm_path + len, "\\*.*");
    rc = _dos_findfirst(rm_path, _A_NORMAL | _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH | _A_SUBDIR, &ft);
    while (rc == 0) {
        scan_fixname(ft.name);
        if (ft.name[0] != '.') {
            rm_path[len] = '\\';
            strcpy(rm_path + len + 1, ft.name);
            if (ft.attrib & _A_SUBDIR) {
                if (rm_level(len + 1 + strlen(ft.name), depth + 1))
                    return 1;
            } else {
                if (ft.attrib & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM))
                    _dos_setfileattr(rm_path, _A_NORMAL);
                if (remove(rm_path) != 0)
                    return 1;
            }
        }
        rc = _dos_findnext(&ft);
    }
    rm_path[len] = 0;
    return rmdir(rm_path) != 0;
}

int scan_rmtree(const char *path)
{
    if (strlen(path) + 16 >= sizeof(rm_path))
        return 1;
    strcpy(rm_path, path);
    return rm_level(strlen(rm_path), 0);
}

static int cmp_games(const void *a, const void *b)
{
    return stricmp(((const Game *)a)->name, ((const Game *)b)->name);
}

/* /diag: the first game folder as the scan sees it - a drive that lists
   things its own way (the card's SD through PMDFS) shows here: the names,
   sizes and attributes with the scan's own mask, then with every mask */
void scan_diag(void)
{
    char pat[PATH_LEN + 16], dir[FN_LEN] = "";
    struct find_t ft;
    unsigned rc, n = 0;

    sprintf(pat, "%s\\*.*", gamedir);
    rc = _dos_findfirst(pat, _A_SUBDIR, &ft);
    while (rc == 0 && !dir[0]) {
        scan_fixname(ft.name);
        if ((ft.attrib & _A_SUBDIR) && ft.name[0] != '.' && ft.name[0] != '_')
            strcpy(dir, ft.name);
        rc = _dos_findnext(&ft);
    }
    if (!dir[0] && game_count)          /* none marked as one: the first the scan kept */
        strcpy(dir, games[0].dir);
    if (!dir[0]) {
        printf("Folder : no game folder under %s (findfirst %u)\n", gamedir, rc);
        return;
    }
    printf("Folder : %s\\%s as the scan lists it (raw name [fixed] size attr):", gamedir, dir);
    sprintf(pat, "%s\\%s\\*.*", gamedir, dir);
    rc = _dos_findfirst(pat, SCAN_ATTRS, &ft);
    if (rc)
        printf(" nothing (error %u)", rc);
    while (rc == 0 && n < 10) {
        char raw[16];
        strncpy(raw, ft.name, 15); raw[15] = 0;
        scan_fixname(ft.name);
        printf("%s \"%s\"", n ? "," : "", raw);
        if (strcmp(raw, ft.name) != 0) printf(" [%s]", ft.name);
        printf(" %lu %02X", ft.size, ft.attrib);
        n++;
        rc = _dos_findnext(&ft);
    }
    printf("%s\n", rc == 0 ? ", ..." : "");
    printf("         names in raw form: %u, folders found by probing: %u, programs from the INI: %u, listed as 0 bytes: %u\n",
           scan_names_fixed, scan_probed_dirs, scan_exe_from_ini, scan_sizes_missing);
}

int scan_games(void)
{
    char pat[PATH_LEN + 8];
    struct find_t ft;
    unsigned rc;

    game_count = 0;

    /* Two passes: the folders first, their contents after, so that no
       listing runs inside another - a redirector drive with one search
       at a time (the card's) would lose its place. An entry the listing
       does not mark as a folder, but that lists as one, is one. */
    {
        int i, n = 0, isdir;
        static unsigned char dirs[MAX_GAMES];  /* 1: listed as a folder, 0: to be probed */
        sprintf(pat, "%s\\*.*", gamedir);
        rc = _dos_findfirst(pat, SCAN_ATTRS, &ft);
        while (rc == 0 && n < MAX_GAMES) {
            scan_fixname(ft.name);
            isdir = (ft.attrib & _A_SUBDIR) != 0;
            if ((isdir || !strchr(ft.name, '.')) && !(ft.attrib & _A_VOLID) &&
                ft.name[0] != '.' && ft.name[0] != '_') {
                memset(&games[n], 0, sizeof(Game));
                strncpy(games[n].dir, ft.name, FN_LEN - 1);
                dirs[n++] = (unsigned char)isdir;
            }
            rc = _dos_findnext(&ft);
        }
        for (i = 0; i < n; i++) {
            Game *g = &games[game_count];
            if (g != &games[i]) memcpy(g, &games[i], sizeof(Game));
            if (!dirs[i]) {                 /* not marked a folder: does it list as one? */
                struct find_t probe;
                sprintf(pat, "%s\\%s\\*.*", gamedir, g->dir);
                if (_dos_findfirst(pat, SCAN_ATTRS, &probe) != 0)
                    continue;
                scan_probed_dirs++;
            }
            strncpy(g->name, g->dir, NAME_LEN - 1);
            scan_one(g);
            if (!g->exe[0]) {               /* the listing showed no program: the INI may know one */
                const char *e = ini_game(g->dir, "exe");
                if (e && e[0]) {
                    sprintf(pat, "%s\\%s\\%s", gamedir, g->dir, e);
                    if (access(pat, 0) == 0) {
                        strncpy(g->exe, e, FN_LEN - 1);
                        scan_exe_from_ini++;
                    }
                }
            }
            if (g->exe[0])
                game_count++;
        }
    }

    /* games still on their way in from the server (netinstall=pending in
       the INI) are listed too, folder or no folder: Enter fetches the rest */
    {
        char dir[FN_LEN];
        unsigned pos = 0;
        while (ini_next_pending(&pos, dir)) {
            int j = find_game(dir);
            if (j < 0 && game_count < MAX_GAMES) {
                const char *e = ini_game(dir, "netexe");    /* the server's word, until it is there */
                Game *g = &games[j = game_count++];
                memset(g, 0, sizeof(Game));
                strncpy(g->dir, dir, FN_LEN - 1);
                strncpy(g->name, dir, NAME_LEN - 1);
                if (e) strncpy(g->exe, e, FN_LEN - 1);
            }
            if (j >= 0)
                games[j].flags |= GF_NETPEND;
        }
    }

    ini_apply();

    /* a folder we have not seen before gets its own section, so the
       name can be edited (F2) and the file documents the collection */
    {
        int i;
        for (i = 0; i < game_count; i++)
            if (!(games[i].flags & GF_INI))
                ini_write_name(games[i].dir, games[i].name);
    }

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

void sort_games(void)
{
    qsort(games, game_count, sizeof(Game), cmp_games);
}

int find_game(const char *dir)
{
    int i;
    for (i = 0; i < game_count; i++)
        if (stricmp(games[i].dir, dir) == 0)
            return i;
    return -1;
}
