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
int net_cdmode = 0;             /* LOCAL CD until netcd=1 says otherwise */
int net_rawcd = 0;

/*
 * The install queue. DOS runs one program at a time and the launcher
 * leaves memory to WAVEGET while it fetches, so nothing can download in
 * the background; what it can do is line the games up and fetch them one
 * after another, unattended. Indices into the list, which is why L and
 * leaving the view empty it again.
 */
#define QUEUE_MAX 16
static struct { char dir[9]; unsigned long kb; } queue[QUEUE_MAX];
int net_qcount = 0;

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
    char *id, *dir, *title, *year, *genre, *kb, *exe, *cd, *src, *cdkb, *rawkb;

    chomp(line);
    id = field(&p); dir = field(&p); title = field(&p); year = field(&p);
    genre = field(&p); kb = field(&p); exe = field(&p); cd = field(&p);
    src = field(&p); cdkb = field(&p); rawkb = field(&p);
    (void)id; (void)genre;
    memset(g, 0, sizeof(*g));
    strncpy(g->dir, dir, 8);
    strncpy(g->title, title, 32);
    strncpy(g->exe, exe, 12);
    g->year = (unsigned)atoi(year);
    g->kb = strtoul(kb, NULL, 10);
    g->cdkb = strtoul(cdkb, NULL, 10);
    g->rawkb = strtoul(rawkb, NULL, 10);  /* 0 from a server that has no such thing */
    g->cd = (atoi(cd) & 1) != 0;          /* flags: 1 = needs CD, 2 = incomplete */
    g->partial = (atoi(cd) & 2) != 0;
    g->netcd = (atoi(cd) & 4) != 0;
    g->netplay = (atoi(cd) & 8) != 0;
    strncpy(g->src, src[0] ? src : "exodos", 7);
}

/* folder names, not list positions: the queue then survives a new list,
   the hand-off to WAVEGET and the machine being switched off half way */
int net_queued(const char *dir)
{
    int k;
    for (k = 0; k < net_qcount; k++)
        if (stricmp(queue[k].dir, dir) == 0)
            return 1;
    return 0;
}

int net_queue_add(const char *dir, unsigned long kb)
{
    if (net_queued(dir))
        return 1;
    if (net_qcount >= QUEUE_MAX)
        return -1;
    strncpy(queue[net_qcount].dir, dir, 8);
    queue[net_qcount].dir[8] = 0;
    queue[net_qcount].kb = kb;
    net_qcount++;
    return 1;
}

/* 1 = queued now, 0 = taken out again, -1 = no room */
int net_queue_toggle(const char *dir, unsigned long kb)
{
    int k;
    for (k = 0; k < net_qcount; k++)
        if (stricmp(queue[k].dir, dir) == 0) {
            for (; k + 1 < net_qcount; k++)
                queue[k] = queue[k + 1];
            net_qcount--;
            return 0;
        }
    return net_queue_add(dir, kb);
}

void net_queue_clear(void)
{
    net_qcount = 0;
}

/* what the whole queue comes to, as it was when each game was put in */
unsigned long net_queue_kb(void)
{
    unsigned long kb = 0;
    int k;
    for (k = 0; k < net_qcount; k++)
        kb += queue[k].kb;
    return kb;
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

/* the size of the download as the mode has it: without the disc when it
   stays on the server */
unsigned long net_size(const NetGame *g)
{
    if (g->cd && g->netcd && net_cdmode && g->cdkb < g->kb)
        return g->kb - g->cdkb;
    if (g->cd && net_rawcd && g->rawkb && g->cdkb <= g->kb)
        return g->kb - g->cdkb + g->rawkb;  /* the cue/bin instead of the ISO */
    return g->kb;
}

/* index of the first title starting with c (A-Z), or -1 */
int net_letter_first(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 'A' || c > 'Z') return -1;
    return letter_first[c - 'A'];
}

void net_pending_reset(void)
{
    char path[PATH_LEN + 16];
    net_path(path, "NETGAME.TXT");
    remove(path);
}

/* One line per game on the way in - the install queue, really, which is
   why it holds the size too and why net_apply_pending puts back whatever
   did not make it. */
void net_mark_pending(const NetGame *g)
{
    char path[PATH_LEN + 16];
    FILE *f;
    net_path(path, "NETGAME.TXT");
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s|%s|%s|%s|%lu\n", g->dir, g->title, g->exe, g->src, net_size(g));
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
 * After a scan: every game that was on its way in gets its proper name,
 * exe and source in the INI - one line each, a whole queue's worth.
 * Returns the index of the first that arrived; -1 if none did, -2 if a
 * folder is there but the scan found nothing to run in it (the server
 * had only part of the game).
 */
int net_apply_pending(void)
{
    char path[PATH_LEN + 16], keep[PATH_LEN + 16];
    char line[112], copy[112], firstdir[9] = "";
    FILE *f, *rest;
    int empty = 0, left = 0;

    net_path(path, "NETGAME.TXT");
    f = fopen(path, "r");
    if (!f)
        return -1;
    net_path(keep, "NETGAME.$$$");      /* what is still wanted */
    rest = fopen(keep, "w");
    while (fgets(line, sizeof(line), f)) {      /* one line per game */
        char *p = line, *dir, *title, *exe, *src, *kb;
        int i, unfinished;
        chomp(p);
        strcpy(copy, line);
        dir = field(&p); title = field(&p); exe = field(&p); src = field(&p);
        kb = field(&p);
        if (!dir[0])
            continue;
        if (!src[0]) src = "exodos";
        strncpy(net_pending_dir, dir, 8);
        net_pending_dir[8] = 0;
        /* WAVEGET leaves its resume note behind when a download stops
           part way; such a game goes back into the queue, and so does one
           whose folder never appeared */
        sprintf(path, "%s\\%s\\WAVE86.RSM", gamedir, dir);
        unfinished = access(path, 0) == 0;
        i = unfinished ? -1 : find_game(dir);
        if (i < 0) {
            if (rest) { fputs(copy, rest); fputc('\n', rest); left++; }
            net_queue_add(dir, kb[0] ? strtoul(kb, NULL, 10) : 0);
            if (!unfinished) {          /* nothing runnable in the folder? */
                sprintf(path, "%s\\%s", gamedir, dir);
                if (access(path, 0) == 0)
                    empty = 1;
            }
            continue;
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
        sort_games();                   /* indices move: hold on to the name */
        if (!firstdir[0]) {
            strncpy(firstdir, dir, 8);
            firstdir[8] = 0;
        }
    }
    fclose(f);
    if (rest) fclose(rest);
    net_path(path, "NETGAME.TXT");
    remove(path);
    if (left)                           /* the rest of the queue, for next time */
        rename(keep, path);
    else
        remove(keep);
    if (!firstdir[0])
        return empty ? -2 : -1;
    strcpy(net_pending_dir, firstdir);  /* the one to land on in the list */
    return find_game(net_pending_dir);
}
