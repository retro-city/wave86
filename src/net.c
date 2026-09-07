/*
 * net.c - the eXoDOS list and the bookkeeping around downloads.
 *
 * The launcher itself never talks to the network. WAVEGET.EXE (an mTCP
 * program next to the launcher) fetches NETLIST.TXT from waveserve and
 * streams game packs into C:\GAMES; it runs through the same batch
 * hand-off as a game. What lives here: reading the list into far memory,
 * and two small state files that survive the round trip:
 *   NETGAME.TXT  DIR|Title|EXE   a download was started; on return, name
 *                                the new folder in the INI and select it
 *   NETVIEW.TXT                  reopen the network view on return
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "wave86.h"

#define NET_PER_BLOCK 700          /* 700 x 62 bytes < 44K */
#define NET_BLOCKS    3            /* up to 2100 games in memory */

int net_count = 0;
char cfg_server[32] = "";

static NetGame __far *blocks[NET_BLOCKS];
static char pending_dir[FN_LEN] = "";

static void net_path(char *dst, const char *name)
{
    sprintf(dst, "%s%s%s", home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\", name);
}

NetGame __far *net_get(int i)
{
    return blocks[i / NET_PER_BLOCK] + (i % NET_PER_BLOCK);
}

void net_free(void)
{
    int i;
    for (i = 0; i < NET_BLOCKS; i++)
        if (blocks[i]) { _ffree(blocks[i]); blocks[i] = NULL; }
    net_count = 0;
}

static char *field(char **p)
{
    char *s = *p, *bar;
    if (!s) return "";
    bar = strchr(s, '|');
    if (bar) { *bar = 0; *p = bar + 1; } else { *p = NULL; }
    return s;
}

/* NETLIST.TXT: id|DIR|Title|year|genre|KB|EXE|CD per line */
int net_load(void)
{
    char path[PATH_LEN + 16];
    char line[160];
    FILE *f;

    net_free();
    net_path(path, "NETLIST.TXT");
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f) && net_count < NET_PER_BLOCK * NET_BLOCKS) {
        char *p = line, *e;
        NetGame __far *g;
        int b = net_count / NET_PER_BLOCK;
        char *id, *dir, *title, *year, *genre, *kb, *exe, *cd;

        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        id = field(&p); dir = field(&p); title = field(&p); year = field(&p);
        genre = field(&p); kb = field(&p); exe = field(&p); cd = field(&p);
        (void)id; (void)genre;
        if (!dir[0] || !title[0])
            continue;
        if (!blocks[b]) {
            blocks[b] = (NetGame __far *)_fmalloc(NET_PER_BLOCK * sizeof(NetGame));
            if (!blocks[b]) break;
        }
        g = net_get(net_count);
        _fmemset(g, 0, sizeof(NetGame));
        _fstrncpy(g->dir, dir, 8);
        _fstrncpy(g->title, title, 32);
        _fstrncpy(g->exe, exe, 12);
        g->year = (unsigned)atoi(year);
        g->kb = strtoul(kb, NULL, 10);
        g->cd = (cd[0] == '1');
        net_count++;
    }
    fclose(f);
    return net_count;
}

void net_mark_pending(const NetGame __far *g)
{
    char path[PATH_LEN + 16];
    FILE *f;
    char dir[9], title[33], exe[13];

    _fstrcpy(dir, g->dir); _fstrcpy(title, g->title); _fstrcpy(exe, g->exe);
    net_path(path, "NETGAME.TXT");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s|%s|%s\n", dir, title, exe);
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

/* returns 1 if the network view should be reopened */
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
 * proper name and exe in the INI. Returns its index in games[], or -1.
 */
int net_apply_pending(void)
{
    char path[PATH_LEN + 16];
    char line[96];
    FILE *f;
    char *p, *dir, *title, *exe;
    int i;

    net_path(path, "NETGAME.TXT");
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    remove(path);
    p = line;
    dir = field(&p); title = field(&p); exe = field(&p);
    if (exe) {
        char *e = exe + strlen(exe);
        while (e > exe && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    }
    strncpy(pending_dir, dir, FN_LEN - 1);
    i = find_game(dir);
    if (i < 0)
        return -1;                      /* nothing arrived */
    if (title[0]) {
        strncpy(games[i].name, title, NAME_LEN - 1);
        ini_write_name(dir, title);
    }
    if (exe && exe[0]) {
        strncpy(games[i].exe, exe, FN_LEN - 1);
        ini_write_key(dir, "exe", exe);
    }
    sort_games();
    return find_game(dir);
}
