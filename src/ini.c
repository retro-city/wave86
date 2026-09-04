/*
 * ini.c - NEON86.INI: global settings + per-game overrides.
 *
 *   gamedir=C:\GAMES
 *
 *   [KEEN4]
 *   name=Commander Keen 4
 *   exe=KEEN4E.EXE
 *   setup=SETUP.EXE
 *   args=/comp
 *   hide=1
 *
 * Section names match game directory names (case-insensitive).
 * The file is read once into a small line store so sections can be
 * applied again after every rescan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "wave86.h"

#define MAX_LINES 256
#define LINE_LEN  96

static char lines[MAX_LINES][LINE_LEN];
static int nlines = 0;

int cfg_modrate = -1;           /* modrate= : MOD mixer rate, 0 disables */
int cfg_adlib = -1;             /* adlib=   : 1 force FM on, 0 off */
int cfg_music = 0;              /* music=   : 1 = play at startup */

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

void ini_load(const char *fname)
{
    FILE *f = fopen(fname, "r");
    char buf[LINE_LEN];

    nlines = 0;
    if (!f)
        return;
    while (nlines < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        char *s = trim(buf);
        if (*s == 0 || *s == ';' || *s == '#')
            continue;
        strncpy(lines[nlines], s, LINE_LEN - 1);
        lines[nlines][LINE_LEN - 1] = 0;
        nlines++;
    }
    fclose(f);

    /* global keys live before the first [section] */
    {
        int i;
        for (i = 0; i < nlines; i++) {
            char *s = lines[i];
            if (s[0] == '[')
                break;
            if (strnicmp(s, "gamedir=", 8) == 0) {
                strncpy(gamedir, trim(s + 8), PATH_LEN - 1);
                gamedir[PATH_LEN - 1] = 0;
            } else if (strnicmp(s, "modrate=", 8) == 0) {
                cfg_modrate = atoi(trim(s + 8));
            } else if (strnicmp(s, "adlib=", 6) == 0) {
                cfg_adlib = atoi(trim(s + 6));
            } else if (strnicmp(s, "music=", 6) == 0) {
                cfg_music = atoi(trim(s + 6)) ? 1 : 0;
            }
        }
    }
}

static void set_field(char *dst, unsigned dstlen, const char *val)
{
    strncpy(dst, val, dstlen - 1);
    dst[dstlen - 1] = 0;
}

void ini_apply(void)
{
    int i;
    Game *cur = NULL;

    for (i = 0; i < nlines; i++) {
        char *s = lines[i];
        if (s[0] == '[') {
            char sect[FN_LEN];
            char *e = strchr(s, ']');
            int g;
            cur = NULL;
            if (!e)
                continue;
            *e = 0;
            strncpy(sect, trim(s + 1), FN_LEN - 1);
            sect[FN_LEN - 1] = 0;
            *e = ']';
            for (g = 0; g < game_count; g++) {
                if (stricmp(games[g].dir, sect) == 0) {
                    cur = &games[g];
                    break;
                }
            }
        } else if (cur) {
            char *eq = strchr(s, '=');
            char *key, *val;
            if (!eq)
                continue;
            *eq = 0;
            key = trim(s);
            val = trim(eq + 1);
            if (stricmp(key, "name") == 0)
                set_field(cur->name, NAME_LEN, val);
            else if (stricmp(key, "exe") == 0)
                set_field(cur->exe, FN_LEN, val);
            else if (stricmp(key, "setup") == 0)
                set_field(cur->setup, FN_LEN, val);
            else if (stricmp(key, "args") == 0)
                set_field(cur->args, sizeof(cur->args), val);
            else if (stricmp(key, "hide") == 0 && val[0] == '1')
                cur->flags |= GF_HIDE;
            *eq = '=';
        }
    }
}
