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
    "NETWORK", "ASKECHO", "CWSDPMI", "DOS32A", "PMODEW", "LOADFIX", "CHOICE",
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

/* how much a program's name looks like the folder's: 3 the same, 2 one
   starts the other, 1 one holds the other (KEEN4 in CKEEN4), 0 not at
   all - three characters at least */
static int likeness(const char *base, const char *dirname)
{
    char a[9], b[9], d[9];
    unsigned al, bl, i;
    for (i = 0; i < 8 && dirname[i] && dirname[i] != '.'; i++)
        d[i] = dirname[i];              /* WOLF3D.V14: the name before the dot */
    d[i] = 0;
    dirname = d;
    if (stricmp(base, dirname) == 0)
        return 3;
    al = strlen(base); bl = strlen(dirname);
    if (al < 3 || bl < 3 || al > 8 || bl > 8)
        return 0;
    if (strnicmp(base, dirname, al < bl ? al : bl) == 0)
        return 2;
    for (i = 0; i <= al; i++) a[i] = (char)toupper((unsigned char)base[i]);
    for (i = 0; i <= bl; i++) b[i] = (char)toupper((unsigned char)dirname[i]);
    return strstr(a, b) || strstr(b, a) ? 1 : 0;
}

/* How likely a program starts the game in folder dirname, highest first:
   a BAT named like the folder, RUN.BAT, the other start batches (START,
   PLAY, GO, GAME), then COM and EXE files - named like the folder, a
   start name, the rest - and last any other BAT. -1: never the game.
   Equal scores go to the bigger file. */
static int score_exe(const char *fn, const char *dirname)
{
    char base[9], ext[4];
    int like, bat;

    split_name(fn, base, ext);
    if (in_list(base, blacklist))
        return -1;
    bat = stricmp(ext, "BAT") == 0;
    if (!bat && stricmp(ext, "EXE") != 0 && stricmp(ext, "COM") != 0)
        return -1;
    like = likeness(base, dirname);
    if (bat) {
        if (like) return 800 + like * 50;
        if (stricmp(base, "RUN") == 0) return 700;
        if (in_list(base, starters)) return 650;
        return 100;
    }
    if (like) return 350 + like * 50;
    if (in_list(base, starters)) return 350;
    return 200;
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

/* the best program and setup in one folder: the game's own (sub "") or a
   folder inside it, whose name then leads the ones found (SUB\GAME.EXE) */
typedef struct {
    char exe[EXE_LEN], setup[EXE_LEN];
    int score, setup_rank;
    unsigned long size;
} Pick;

static void pick_in(Game *g, const char *sub, Pick *pk)
{
    char pat[PATH_LEN + 32];
    struct find_t ft;
    unsigned rc;

    /* every attribute is asked for and folders and labels skipped here: a
       redirector drive that reads the mask its own way still lists the
       programs. An empty EXE cannot run: a download that is still
       incomplete on the server leaves such files behind. */
    sprintf(pat, "%s\\%s%s%s\\*.*", gamedir, g->dir, sub[0] ? "\\" : "", sub);
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
                sprintf(pat, "%s\\%s%s%s\\%s", gamedir, g->dir, sub[0] ? "\\" : "", sub, fn);
                if ((h = open(pat, O_RDONLY | O_BINARY)) >= 0) {
                    size = filelength(h);
                    close(h);
                }
                scan_sizes_missing++;
            }
            if (size != 0) {
                char base[9], ext[4], full[EXE_LEN];
                int sr, sc;

                split_name(fn, base, ext);
                if (stricmp(base, "DOS4GW") == 0)
                    g->flags |= GF_DOS4GW;
                if (sub[0]) sprintf(full, "%.8s\\%.12s", sub, fn); else strcpy(full, fn);
                sr = setup_rank(fn);
                if (sr > pk->setup_rank) {
                    pk->setup_rank = sr;
                    strcpy(pk->setup, full);
                }
                sc = score_exe(fn, g->dir);
                if (sc >= 0)                    /* the game's folder decides; a name like the */
                    sc = sc * 4 + (sub[0] ? likeness(base, sub) : 0);   /* subfolder's breaks ties */
                if (sc >= 0 && (sc > pk->score ||
                                (sc == pk->score && (size > pk->size ||
                                                     (size == pk->size && stricmp(full, pk->exe) < 0))))) {
                    pk->score = sc;
                    pk->size = size;
                    strcpy(pk->exe, full);
                }
            }
        }
        rc = _dos_findnext(&ft);
    }
}

/* the folders inside a game's folder, a few (CD\ and THUMBS\ are ours) */
static int subfolders(const Game *g, char (*out)[9], int max)
{
    char pat[PATH_LEN + 16];
    struct find_t ft;
    unsigned rc;
    int n = 0;
    sprintf(pat, "%s\\%s\\*.*", gamedir, g->dir);
    rc = _dos_findfirst(pat, SCAN_ATTRS, &ft);
    while (rc == 0 && n < max) {
        scan_fixname(ft.name);
        if ((ft.attrib & _A_SUBDIR) && ft.name[0] != '.' && ft.name[0] != '_' &&
            stricmp(ft.name, "CD") != 0 && stricmp(ft.name, "THUMBS") != 0 && strlen(ft.name) <= 8)
            strcpy(out[n++], ft.name);
        rc = _dos_findnext(&ft);
    }
    return n;
}

static void scan_one(Game *g)
{
    char pat[PATH_LEN + 16];
    struct find_t ft;
    Pick top;

    memset(&top, 0, sizeof(top));
    top.score = -1;
    pick_in(g, "", &top);
    /* nothing to run at the top: a game installed one folder down
       (GAMES\DOOM\DOOM\DOOM.EXE), looked for once the top listing is done,
       since a redirector drive keeps one search at a time */
    if (!top.exe[0]) {
        static char subs[12][9];
        char setup0[EXE_LEN];           /* the top's own setup, if the winner has none */
        int n = subfolders(g, subs, 12), i;
        strcpy(setup0, top.setup);
        for (i = 0; i < n; i++) {
            Pick pk;
            memset(&pk, 0, sizeof(pk));
            pk.score = -1;
            pick_in(g, subs[i], &pk);
            if (pk.exe[0] && (pk.score > top.score || (pk.score == top.score && pk.size > top.size))) {
                strcpy(top.exe, pk.exe);
                top.score = pk.score;
                top.size = pk.size;
                strcpy(top.setup, pk.setup[0] ? pk.setup : setup0);   /* the winner's setup */
            }
        }
    }
    strcpy(g->exe, top.exe);
    strcpy(g->setup, top.setup);

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

/* The programs in a game's folder, and one folder down, best first in the
   scan's own order (the ones it would never pick last): what E offers
   with left and right. Returns how many, max at most. */
int scan_programs(const char *dir, char (*out)[EXE_LEN], int max)
{
    static Game tmp;
    static char subs[12][9];
    static int score[24];
    char pat[PATH_LEN + 32];
    struct find_t ft;
    unsigned rc;
    int n = 0, ns, i, j, pass;

    if (max > 24) max = 24;
    memset(&tmp, 0, sizeof(tmp));
    strncpy(tmp.dir, dir, FN_LEN - 1);
    ns = subfolders(&tmp, subs, 12);
    for (pass = -1; pass < ns && n < max; pass++) {
        const char *sub = pass < 0 ? "" : subs[pass];
        sprintf(pat, "%s\\%s%s%s\\*.*", gamedir, dir, sub[0] ? "\\" : "", sub);
        rc = _dos_findfirst(pat, SCAN_ATTRS, &ft);
        while (rc == 0 && n < max) {
            scan_fixname(ft.name);
            if (!(ft.attrib & (_A_SUBDIR | _A_VOLID)) &&
                (has_ext(ft.name, "EXE") || has_ext(ft.name, "COM") || has_ext(ft.name, "BAT"))) {
                char base[9], ext[4];
                split_name(ft.name, base, ext);
                if (sub[0]) sprintf(out[n], "%.8s\\%.12s", sub, ft.name); else strcpy(out[n], ft.name);
                score[n] = score_exe(ft.name, dir);
                if (score[n] < 0) score[n] = in_list(base, blacklist) ? -2 : -1;
                n++;
            }
            rc = _dos_findnext(&ft);
        }
    }
    for (i = 1; i < n; i++)             /* insertion sort: a folder has a handful */
        for (j = i; j > 0 && score[j] > score[j - 1]; j--) {
            char t[EXE_LEN];
            int ts = score[j];
            strcpy(t, out[j]); strcpy(out[j], out[j - 1]); strcpy(out[j - 1], t);
            score[j] = score[j - 1]; score[j - 1] = ts;
        }
    return n;
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

static int show_empty = 0;          /* showempty=1: list folders with no program too */

int scan_games(void)
{
    char pat[PATH_LEN + 8];
    struct find_t ft;
    unsigned rc;

    game_count = 0;
    {
        const char *v = ini_global("showempty");
        show_empty = v && v[0] == '1';
    }

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
                        strncpy(g->exe, e, EXE_LEN - 1);
                        scan_exe_from_ini++;
                    }
                }
            }
            /* showempty=1: every folder, the ones with nothing to run too
               (dimmed; E sets their program) */
            if (g->exe[0] || show_empty)
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
                if (e) strncpy(g->exe, e, EXE_LEN - 1);
            } else if (j >= 0 && !games[j].exe[0]) {  /* listed by showempty, nothing there yet */
                const char *e = ini_game(dir, "netexe");
                if (e) strncpy(games[j].exe, e, EXE_LEN - 1);
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
