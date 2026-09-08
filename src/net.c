/*
 * net.c - the game list from the server and the bookkeeping around
 * downloads.
 *
 * The launcher itself never talks to the network. WAVEGET.EXE (an mTCP
 * program next to the launcher) fetches NETLIST.TXT from waveserve and
 * streams game packs into C:\GAMES; it runs through the same batch
 * hand-off as a game. What lives here:
 *   - the list: NETLIST.TXT can hold thousands of games (a Total DOS
 *     Collection), so only a table of line offsets is kept in far memory
 *     and lines are read on demand; a first-letter index makes jumping
 *     cheap.
 *   - two small state files that survive the round trip:
 *     NETGAME.TXT  DIR|Title|EXE|SRC  a download was started; on return,
 *                                    name and tag the new folder, select it
 *     NETVIEW.TXT                    reopen the network view on return
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <io.h>
#include "wave86.h"

#define OFF_PER_BLOCK 8000         /* 8000 x 4 bytes = 32000 per block */
#define OFF_BLOCKS    2            /* up to 16000 games */

int net_count = 0;
char cfg_server[32] = "";
char net_pending_dir[9] = "";

static unsigned long __far *offs[OFF_BLOCKS];
static int letter_first[27];       /* first index per initial A..Z, [26] = none */
static FILE *listf = NULL;
static NetGame cur;
static int cur_idx = -1;

static void net_path(char *dst, const char *name)
{
    sprintf(dst, "%s%s%s", home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\", name);
}

static char *field(char **p)
{
    char *s = *p, *bar;
    if (!s) return "";
    bar = strchr(s, '|');
    if (bar) { *bar = 0; *p = bar + 1; } else { *p = NULL; }
    return s;
}

static void chomp(char *line)
{
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
}

/* id|DIR|Title|year|genre|KB|EXE|CD|SRC -> cur */
static void parse_line(char *line, NetGame *g)
{
    char *p = line;
    char *id, *dir, *title, *year, *genre, *kb, *exe, *cd, *src;

    chomp(line);
    id = field(&p); dir = field(&p); title = field(&p); year = field(&p);
    genre = field(&p); kb = field(&p); exe = field(&p); cd = field(&p);
    src = field(&p);
    (void)id; (void)genre;
    memset(g, 0, sizeof(*g));
    strncpy(g->dir, dir, 8);
    strncpy(g->title, title, 32);
    strncpy(g->exe, exe, 12);
    g->year = (unsigned)atoi(year);
    g->kb = strtoul(kb, NULL, 10);
    g->cd = (atoi(cd) & 1) != 0;          /* flags: 1 = needs CD, 2 = incomplete */
    g->partial = (atoi(cd) & 2) != 0;
    g->netcd = (atoi(cd) & 4) != 0;
    strncpy(g->src, src[0] ? src : "exodos", 7);
}

void net_free(void)
{
    int i;
    for (i = 0; i < OFF_BLOCKS; i++)
        if (offs[i]) { _ffree(offs[i]); offs[i] = NULL; }
    if (listf) { fclose(listf); listf = NULL; }
    net_count = 0;
    cur_idx = -1;
}

static int initial_of(const char *title)
{
    char c = title[0];
    if (c >= 'a' && c <= 'z') c -= 32;
    return (c >= 'A' && c <= 'Z') ? c - 'A' : 26;
}

/* one pass over NETLIST.TXT: remember where every line starts */
int net_load(void)
{
    char path[PATH_LEN + 16];
    char line[160];
    int i;

    net_free();
    net_path(path, "NETLIST.TXT");
    listf = fopen(path, "r");
    if (!listf)
        return 0;
    for (i = 0; i < 27; i++) letter_first[i] = -1;
    for (;;) {
        long at = ftell(listf);
        int b = net_count / OFF_PER_BLOCK;
        char *p, *t;
        if (net_count >= OFF_PER_BLOCK * OFF_BLOCKS) break;
        if (!fgets(line, sizeof(line), listf)) break;
        if (!offs[b]) {
            offs[b] = (unsigned long __far *)_fmalloc(OFF_PER_BLOCK * sizeof(unsigned long));
            if (!offs[b]) break;
        }
        p = line; field(&p); field(&p); t = field(&p);    /* id, DIR, title */
        if (!t[0]) continue;
        {
            int li = initial_of(t);
            if (letter_first[li] < 0) letter_first[li] = net_count;
        }
        offs[b][net_count % OFF_PER_BLOCK] = (unsigned long)at;
        net_count++;
    }
    return net_count;
}

const NetGame *net_get(int i)
{
    char line[160];
    if (i < 0 || i >= net_count || !listf)
        return &cur;
    if (i == cur_idx)
        return &cur;
    fseek(listf, (long)offs[i / OFF_PER_BLOCK][i % OFF_PER_BLOCK], SEEK_SET);
    if (fgets(line, sizeof(line), listf))
        parse_line(line, &cur);
    cur_idx = i;
    return &cur;
}

/* index of the first title starting with c (A-Z), or -1 */
int net_letter_first(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 'A' || c > 'Z') return -1;
    return letter_first[c - 'A'];
}

void net_mark_pending(const NetGame *g)
{
    char path[PATH_LEN + 16];
    FILE *f;
    net_path(path, "NETGAME.TXT");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s|%s|%s|%s\n", g->dir, g->title, g->exe, g->src);
    fclose(f);
}

void net_mark_view(void)
{
    char path[PATH_LEN + 16];
    FILE *f;
    net_path(path, "NETVIEW.TXT");
    f = fopen(path, "w");
    if (f) { fputs("1\n", f); fclose(f); }
}

int net_view_pending(void)
{
    char path[PATH_LEN + 16];
    FILE *f;
    net_path(path, "NETVIEW.TXT");
    f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    remove(path);
    return 1;
}

/*
 * After a scan: if a download was started, give the new folder its
 * proper name, exe and source in the INI. Returns its index; -1 if
 * nothing arrived, -2 if the folder is there but the scan found nothing
 * to run in it (the server had only part of the game).
 */
int net_apply_pending(void)
{
    char path[PATH_LEN + 16];
    char line[96];
    FILE *f;
    char *p, *dir, *title, *exe, *src;
    int i;

    net_path(path, "NETGAME.TXT");
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    remove(path);
    chomp(line);
    p = line;
    dir = field(&p); title = field(&p); exe = field(&p); src = field(&p);
    if (!src[0]) src = "exodos";
    strncpy(net_pending_dir, dir, 8);
    net_pending_dir[8] = 0;
    i = find_game(dir);
    if (i < 0) {
        sprintf(path, "%s\\%s", gamedir, dir);
        return access(path, 0) == 0 ? -2 : -1;
    }
    if (title[0]) {
        strncpy(games[i].name, title, NAME_LEN - 1);
        ini_write_name(dir, title);
    }
    if (exe[0]) {
        strncpy(games[i].exe, exe, FN_LEN - 1);
        ini_write_key(dir, "exe", exe);
    }
    games[i].flags |= stricmp(src, "tdc") == 0 ? GF_TDC : GF_EXODOS;
    ini_write_key(dir, "source", src);
    sort_games();
    return find_game(dir);
}
