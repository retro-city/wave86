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
 *   cd=CD\SYNDICAT.ISO      (found by itself when it sits in CD\)
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

/* The INI is held in memory for reading (ini_global, ini_apply); writing
   streams the file through a .TMP instead, so a file with more lines than
   fit is never rewritten short. What does not fit is simply not seen, so
   these are sized for a big collection while leaving the 64K data segment
   room for a stack and a heap - two open streams need about 1.2K of it. */
#define MAX_LINES 220
#define LINE_LEN  80

static char lines[MAX_LINES][LINE_LEN];
static int nlines = 0;

int cfg_modrate = -1;           /* modrate= : MOD mixer rate, 0 disables */
int cfg_adlib = -1;             /* adlib=   : 1 force FM on, 0 off */
int cfg_music = 0;              /* music=   : 1 = play at startup */
char cfg_theme[16] = "";        /* theme=   : wave86 (default) or exodos */
int cfg_netcd = 0;              /* netcd=   : 1 = leave CDs on the server (NET CD) */

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

static char ini_path[PATH_LEN] = "WAVE86.INI";

void ini_load(const char *fname)
{
    FILE *f;
    char buf[LINE_LEN];

    if (fname != ini_path) {
        strncpy(ini_path, fname, PATH_LEN - 1);
        ini_path[PATH_LEN - 1] = 0;
    }
    f = fopen(fname, "r");

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
            } else if (strnicmp(s, "netcd=", 6) == 0) {
                cfg_netcd = atoi(trim(s + 6)) ? 1 : 0;
            } else if (strnicmp(s, "theme=", 6) == 0) {
                int k;
                strncpy(cfg_theme, trim(s + 6), sizeof(cfg_theme) - 1);
                cfg_theme[sizeof(cfg_theme) - 1] = 0;
                for (k = 0; cfg_theme[k]; k++)          /* just the word */
                    if (cfg_theme[k] == ' ' || cfg_theme[k] == ';' || cfg_theme[k] == '\t')
                        cfg_theme[k] = 0;
            } else if (strnicmp(s, "server=", 7) == 0) {
                strncpy(cfg_server, trim(s + 7), sizeof(cfg_server) - 1);
                cfg_server[sizeof(cfg_server) - 1] = 0;
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
                    cur->flags |= GF_INI;
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
            else if (stricmp(key, "sound") == 0)
                set_field(cur->sound, sizeof(cur->sound), val);
            else if (stricmp(key, "cd") == 0)
                set_field(cur->cdimg, sizeof(cur->cdimg), val);
            else if (stricmp(key, "source") == 0) {
                if (stricmp(val, "exodos") == 0) cur->flags |= GF_EXODOS;
                if (stricmp(val, "tdc") == 0)    cur->flags |= GF_TDC;
            }
            else if (stricmp(key, "hide") == 0 && val[0] == '1')
                cur->flags |= GF_HIDE;
            *eq = '=';
        }
    }
}

/* value of a key that sits above the first [section], or NULL */
const char *ini_global(const char *key)
{
    static char val[LINE_LEN];
    unsigned kl = strlen(key);
    int i;
    for (i = 0; i < nlines; i++) {
        char *s = lines[i];
        if (s[0] == '[')
            break;
        if (strnicmp(s, key, kl) == 0 && s[kl] == '=') {
            strcpy(val, trim(s + kl + 1));
            return val;
        }
    }
    return NULL;
}

static int section_is(const char *line, const char *dir)
{
    char sect[FN_LEN];
    const char *e = strchr(line, ']');
    unsigned n;
    if (line[0] != '[' || !e)
        return 0;
    n = (unsigned)(e - line - 1);
    if (n >= FN_LEN) n = FN_LEN - 1;
    memcpy(sect, line + 1, n);
    sect[n] = 0;
    return stricmp(trim(sect), dir) == 0;
}

/*
 * Set (or add) name= for a game, keeping every other line and comment.
 * Streams the file to WAVE86.TMP and swaps it in, so it needs no more
 * memory than one line.
 */
int ini_write_key(const char *dir, const char *key, const char *name)
{
    char tmp[PATH_LEN + 4];
    char buf[LINE_LEN], line[LINE_LEN];
    char keyeq[24];
    FILE *in, *out;
    int in_target = 0, seen = 0, done = 0;
    unsigned kl;
    char *dot;

    sprintf(keyeq, "%s=", key);
    kl = strlen(keyeq);

    strcpy(tmp, ini_path);
    dot = strrchr(tmp, '.');
    if (dot && !strchr(dot, '\\'))
        *dot = 0;
    strcat(tmp, ".TMP");

    out = fopen(tmp, "w");
    if (!out)
        return 1;
    in = fopen(ini_path, "r");
    if (in) {
        while (fgets(buf, sizeof(buf), in)) {
            char *t;
            strcpy(line, buf);
            t = trim(line);
            if (t[0] == '[') {
                if (in_target && !done) {
                    fprintf(out, "%s%s\n", keyeq, name);
                    done = 1;
                }
                in_target = section_is(t, dir);
                if (in_target)
                    seen = 1;
            } else if (in_target && !done) {
                if (strnicmp(t, keyeq, kl) == 0) {
                    fprintf(out, "%s%s\n", keyeq, name);   /* replace */
                    done = 1;
                    continue;
                }
                if (t[0] == 0) {                    /* section ends */
                    fprintf(out, "%s%s\n", keyeq, name);
                    done = 1;
                }
            }
            fputs(buf, out);
        }
        fclose(in);
        if (in_target && !done)
            fprintf(out, "%s%s\n", keyeq, name);
    }
    if (!seen)
        fprintf(out, "\n[%s]\n%s%s\n", dir, keyeq, name);
    fclose(out);
    remove(ini_path);
    if (rename(tmp, ini_path) != 0)
        return 1;
    ini_load(ini_path);                 /* pick the change up */
    return 0;
}

int ini_write_name(const char *dir, const char *name)
{
    return ini_write_key(dir, "name", name);
}

/*
 * Extra lines for RUNGAME.BAT from a game's section:
 *   before the game: the soundcmd_<mode> for its sound= (or the global
 *   default), then env=VAR=value as SET, then pre=command
 *   after the game: post=command
 */
void ini_emit_extras(FILE *bat, const char *dir, int after)
{
    char buf[LINE_LEN];
    char mode[16] = "";
    FILE *in = fopen(ini_path, "r");
    int in_target = 0;

    if (!after) {
        const char *d = ini_global("sound");
        if (d) { strncpy(mode, d, 15); mode[15] = 0; }
    }
    if (in) {
        while (fgets(buf, sizeof(buf), in)) {
            char *t = trim(buf);
            if (t[0] == '[') {
                if (in_target) break;
                in_target = section_is(t, dir);
                continue;
            }
            if (!in_target)
                continue;
            if (!after && strnicmp(t, "sound=", 6) == 0) {
                strncpy(mode, trim(t + 6), 15);
                mode[15] = 0;
            }
        }
        fclose(in);
    }
    if (!after && mode[0]) {
        char key[32];
        const char *cmd;
        sprintf(key, "soundcmd_%s", mode);
        cmd = ini_global(key);
        if (cmd && cmd[0])
            fprintf(bat, "%s\n", cmd);
    }
    in = fopen(ini_path, "r");
    if (!in)
        return;
    in_target = 0;
    while (fgets(buf, sizeof(buf), in)) {
        char *t = trim(buf);
        if (t[0] == '[') {
            if (in_target) break;
            in_target = section_is(t, dir);
            continue;
        }
        if (!in_target)
            continue;
        if (!after && strnicmp(t, "env=", 4) == 0)
            fprintf(bat, "set %s\n", trim(t + 4));
        else if (!after && strnicmp(t, "pre=", 4) == 0)
            fprintf(bat, "%s\n", trim(t + 4));
        else if (after && strnicmp(t, "post=", 5) == 0)
            fprintf(bat, "%s\n", trim(t + 5));
    }
    fclose(in);
}
