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
 * The file is read once into a line store so sections can be applied
 * again after every rescan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include "wave86.h"

/* The INI is held in memory for reading (ini_global, ini_apply, ini_game):
   the lines that matter, trimmed and packed one after another in a 60K
   block of far memory, room for a few thousand of them. The 64K data
   segment could spare a table of 220, and a collection past 45 games
   installed from the server fell off its end unseen. Writing streams the
   file through a .TMP instead, so nothing is ever rewritten short. */
#define POOL_MAX  0xF000u
#define LINE_LEN  80

static char __far *pool = NULL;     /* the lines, a NUL after each */
static unsigned pool_used = 0;
static int nlines = 0;

/* the line at pos into buf; what comes back is where the next one starts */
static unsigned line_at(unsigned pos, char *buf)
{
    _fstrcpy(buf, pool + pos);
    return pos + strlen(buf) + 1;
}

int cfg_modrate = -1;           /* modrate= : MOD mixer rate, 0 disables */
int cfg_adlib = -1;             /* adlib=   : 1 force FM on, 0 off */
int cfg_music = 0;              /* music=   : 1 = play at startup */
char cfg_theme[16] = "";        /* theme=   : exodos (default) or wave86 */
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
static char games_path[PATH_LEN + 12] = "";     /* <gamedir>\GAMES.INI: the [sections], once the folder is known */

/* where a game's section is written: GAMES.INI next to the games, or the INI itself before that is known */
static const char *section_file(void)
{
    return games_path[0] ? games_path : ini_path;
}

const char *ini_file_path(void)  { return ini_path; }
const char *ini_games_path(void) { return games_path; }

/* the same file with .TMP for its extension: written whole, then swapped in */
static void tmp_name(char *tmp, const char *path)
{
    char *dot;
    strcpy(tmp, path);
    dot = strrchr(tmp, '.');
    if (dot && !strchr(dot, '\\'))
        *dot = 0;
    strcat(tmp, ".TMP");
}

/* the lines of fname into the pool: part 0 all of them, 1 those above the
   first [section], 2 the sections and on */
static void load_part(const char *fname, int part)
{
    FILE *f = fopen(fname, "r");
    char buf[LINE_LEN];
    int in_sections = 0;

    if (!f)
        return;
    while (pool && fgets(buf, sizeof(buf), f)) {
        char *s = trim(buf);
        unsigned n;
        if (*s == 0 || *s == ';' || *s == '#')
            continue;
        if (*s == '[')
            in_sections = 1;
        if (part == 1 && in_sections)
            break;
        if (part == 2 && !in_sections)
            continue;
        n = strlen(s) + 1;
        if (pool_used + n > POOL_MAX)
            break;                      /* 60K of settings: the rest goes unseen */
        _fmemcpy(pool + pool_used, s, n);
        pool_used += n;
        nlines++;
    }
    fclose(f);
}

void ini_load(const char *fname)
{
    char buf[LINE_LEN];

    if (fname != ini_path) {
        strncpy(ini_path, fname, PATH_LEN - 1);
        ini_path[PATH_LEN - 1] = 0;
    }
    nlines = 0;
    pool_used = 0;
    if (!pool)
        pool = (char __far *)_fmalloc(POOL_MAX);
    load_part(ini_path, 1);             /* the machine's settings */
    if (games_path[0])
        load_part(games_path, 0);       /* the games' sections, next to the games */
    load_part(ini_path, 2);             /* sections still in the INI: the first for a game wins */

    /* global keys live before the first [section] */
    {
        unsigned pos;
        for (pos = 0; pos < pool_used; ) {
            char *s = buf;
            pos = line_at(pos, buf);
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
    char buf[LINE_LEN];
    unsigned pos;
    Game *cur = NULL;

    for (pos = 0; pos < pool_used; ) {
        char *s = buf;
        pos = line_at(pos, buf);
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
                    if (games[g].flags & GF_INI)    /* a section earlier in the pool had it */
                        break;
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
    char buf[LINE_LEN];
    unsigned kl = strlen(key), pos;
    for (pos = 0; pos < pool_used; ) {
        char *s = buf;
        pos = line_at(pos, buf);
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
    char tmp[PATH_LEN + 16];
    char buf[LINE_LEN], line[LINE_LEN];
    char keyeq[24];
    FILE *in, *out;
    const char *file = section_file();
    int in_target = 0, seen = 0, done = 0;
    unsigned kl;

    sprintf(keyeq, "%s=", key);
    kl = strlen(keyeq);
    tmp_name(tmp, file);

    out = fopen(tmp, "w");
    if (!out)
        return 1;
    in = fopen(file, "r");
    if (!in && file == games_path)      /* the first game: the file gets its header */
        fputs("; GAMES.INI - WAVE86's settings for the games in this folder, a\n"
              "; [section] per game folder: name=, exe= and the rest (WAVE86.INI next\n"
              "; to the launcher lists every key). The launcher keeps it; edit away.\n", out);
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
    remove(file);
    if (rename(tmp, file) != 0)
        return 1;
    ini_load(ini_path);                 /* pick the change up */
    return 0;
}

/* N with no server=: a key set, or added, in the global part above the
   first [section], every other line kept; streamed like ini_write_key */
int ini_write_global(const char *key, const char *value)
{
    char tmp[PATH_LEN + 4], buf[LINE_LEN], line[LINE_LEN], keyeq[24];
    FILE *in, *out;
    int done = 0;
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
            if (!done && t[0] == '[') {         /* the sections start: it goes above them */
                fprintf(out, "%s%s\n\n", keyeq, value);
                done = 1;
            } else if (!done && strnicmp(t, keyeq, kl) == 0) {
                fprintf(out, "%s%s\n", keyeq, value);      /* replace */
                done = 1;
                continue;
            }
            fputs(buf, out);
        }
        fclose(in);
    }
    if (!done)
        fprintf(out, "%s%s\n", keyeq, value);
    fclose(out);
    remove(ini_path);
    if (rename(tmp, ini_path) != 0)
        return 1;
    ini_load(ini_path);
    return 0;
}

/* Del in the games list: the section goes with the folder, or the file
   fills up with games that are not there. Its keys go, and the blank line
   after them; a comment between that and the next [section] introduces
   the next one, so it stays. */
static int remove_section_in(const char *file, const char *dir)
{
    char tmp[PATH_LEN + 16], buf[LINE_LEN], line[LINE_LEN];
    FILE *in, *out;
    int skipping = 0, found = 0;    /* 1 in the section, 2 past its blank line */

    tmp_name(tmp, file);
    in = fopen(file, "r");
    if (!in)
        return 1;
    out = fopen(tmp, "w");
    if (!out) { fclose(in); return 1; }
    while (fgets(buf, sizeof(buf), in)) {
        char *t;
        strcpy(line, buf);
        t = trim(line);
        if (t[0] == '[') {
            skipping = section_is(t, dir);
            found |= skipping;
        } else if (skipping == 1 && t[0] == 0)
            skipping = 2;
        if (skipping == 1)
            continue;
        if (skipping == 2 && t[0] != ';' && t[0] != '#')
            continue;                   /* a stray key or blank line still belongs to it */
        fputs(buf, out);
    }
    fclose(in);
    fclose(out);
    if (!found) { remove(tmp); return 0; }
    remove(file);
    if (rename(tmp, file) != 0)
        return 1;
    return 0;
}

int ini_remove_section(const char *dir)
{
    int rc = remove_section_in(section_file(), dir);
    if (games_path[0])
        remove_section_in(ini_path, dir);       /* one the INI still had */
    ini_load(ini_path);
    return rc;
}

/* the options box turning a setting off: the line goes, and with it the
   game's own say, so the global default is back */
int ini_remove_key(const char *dir, const char *key)
{
    char tmp[PATH_LEN + 16], buf[LINE_LEN], line[LINE_LEN];
    FILE *in, *out;
    const char *file = section_file();
    int in_target = 0, found = 0;
    unsigned kl = strlen(key);

    tmp_name(tmp, file);
    in = fopen(file, "r");
    if (!in)
        return 1;
    out = fopen(tmp, "w");
    if (!out) { fclose(in); return 1; }
    while (fgets(buf, sizeof(buf), in)) {
        char *t;
        strcpy(line, buf);
        t = trim(line);
        if (t[0] == '[')
            in_target = section_is(t, dir);
        else if (in_target && strnicmp(t, key, kl) == 0 && t[kl] == '=') {
            found = 1;
            continue;
        }
        fputs(buf, out);
    }
    fclose(in);
    fclose(out);
    if (!found) { remove(tmp); return 0; }
    remove(file);
    if (rename(tmp, file) != 0)
        return 1;
    ini_load(ini_path);
    return 0;
}

/*
 * The games' sections live in GAMES.INI next to the games, so settings
 * travel with the collection (an SD card, say) and WAVE86.INI keeps the
 * machine's. Called once the games folder is known: what WAVE86.INI still
 * holds of sections moves over (appended, as they are), and the INI keeps
 * everything above them. 1 when something moved.
 */
int ini_games_file(const char *dir)
{
    FILE *in, *out;
    char buf[LINE_LEN], tmp[PATH_LEN + 16];
    int had = 0, moved = 0;

    sprintf(games_path, "%s%sGAMES.INI", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\");
    in = fopen(ini_path, "r");
    if (!in) {
        ini_load(ini_path);
        return 0;
    }
    while (fgets(buf, sizeof(buf), in))
        if (trim(buf)[0] == '[') { had = 1; break; }
    if (had && (out = fopen(games_path, "a")) != NULL) {
        int in_sections = 0;
        if (ftell(out) == 0)
            fputs("; GAMES.INI - WAVE86's settings for the games in this folder, a\n"
                  "; [section] per game folder: name=, exe= and the rest (WAVE86.INI next\n"
                  "; to the launcher lists every key). The launcher keeps it; edit away.\n", out);
        rewind(in);
        while (fgets(buf, sizeof(buf), in)) {   /* the sections, as they are */
            char line[LINE_LEN];
            strcpy(line, buf);
            if (trim(line)[0] == '[')
                in_sections = 1;
            if (in_sections)
                fputs(buf, out);
        }
        if (ferror(out) || fclose(out) != 0) {
            fclose(in);
            ini_load(ini_path);
            return 0;                   /* the INI keeps them: nothing lost */
        }
        tmp_name(tmp, ini_path);
        rewind(in);
        out = fopen(tmp, "w");
        if (out) {
            while (fgets(buf, sizeof(buf), in)) {
                char line[LINE_LEN];
                strcpy(line, buf);
                if (trim(line)[0] == '[')
                    break;
                fputs(buf, out);
            }
            fclose(out);
            fclose(in);
            in = NULL;
            remove(ini_path);
            if (rename(tmp, ini_path) == 0)
                moved = 1;
        }
    }
    if (in)
        fclose(in);
    ini_load(ini_path);
    return moved;
}

const char *ini_game(const char *dir, const char *key)
{
    static char val[LINE_LEN];
    char buf[LINE_LEN];
    unsigned kl = strlen(key), pos;
    int in = 0;
    for (pos = 0; pos < pool_used; ) {
        char *s = buf;
        pos = line_at(pos, buf);
        if (s[0] == '[') {
            if (in) break;
            in = section_is(s, dir);
        } else if (in && strnicmp(s, key, kl) == 0 && s[kl] == '=') {
            strcpy(val, trim(s + kl + 1));
            return val;
        }
    }
    return NULL;
}

/* the sound modes the INI knows commands for: soundcmd_sb, soundcmd_gus... */
int ini_sound_modes(char (*modes)[16], int max)
{
    char buf[LINE_LEN];
    unsigned pos;
    int n = 0;
    for (pos = 0; pos < pool_used && n < max; ) {
        char *s = buf;
        pos = line_at(pos, buf);
        if (s[0] == '[')
            break;
        if (strnicmp(s, "soundcmd_", 9) == 0) {
            int k;
            for (k = 0; k < 15 && s[9 + k] && s[9 + k] != '=' && s[9 + k] != ' '; k++)
                modes[n][k] = (char)tolower((unsigned char)s[9 + k]);
            modes[n][k] = 0;
            if (k) n++;
        }
    }
    return n;
}

int ini_write_name(const char *dir, const char *name)
{
    return ini_write_key(dir, "name", name);
}

/*
 * Extra lines for WAVERUN.BAT from a game's section:
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
