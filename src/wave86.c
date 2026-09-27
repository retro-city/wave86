/*
 * wave86.c - main program.
 *
 * Launch strategy: WAVE86 never spawns the game itself (that would pin
 * ~100K of launcher in memory under the game). Instead it writes
 * RUNGAME.BAT and exits; the WAVE.BAT wrapper runs the game and then
 * restarts the menu. Games get every byte of conventional memory.
 */
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dos.h>
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
void ui_prompt(const char *label, const char *text);
void ui_net_static(void);
void ui_net_list(int sel, int top);
void ui_net_details(int sel);
void ui_net_keybar(int cur);
void ui_queue_box(int n, int sel, int top, const char __far *titles, const unsigned long __far *kbs);
void ui_menu(const char *title, const MenuItem __far *items, int n, int sel, const unsigned char *dim);
void ui_options(const char *title, const char *const *labels, const char *const *values, int n, int sel);
void ui_game_edit(const char *title, const char *const *labels, const char *const *values, int n, int sel, int can_step, const char *msg);

#define K_UP    0x4800
#define K_DOWN  0x5000
#define K_PGUP  0x4900
#define K_PGDN  0x5100
#define K_HOME  0x4700
#define K_END   0x4F00
#define K_LEFT  0x4B00
#define K_RIGHT 0x4D00
#define K_F1    0x3B00
#define K_F3    0x3D00
#define K_F4    0x3E00
#define K_DEL   0x5300
#define PAGE    14              /* rows in a list */
#define QROWS   12              /* rows in the queue box */

static char launcher_dir[PATH_LEN];     /* cwd at start: where to return */
char home_dir[PATH_LEN];                /* EXE directory: INI and MUSIC\ */
/*
 * The batch the launcher hands work to. It was RUNGAME.BAT, but it runs
 * WAVEGET as often as it runs a game. WAVE.BAT cannot be updated over the
 * network - COMMAND.COM is reading it line by line while the update runs
 * - so a copy from before the rename only knows the old name; while that
 * is the one in use, a one-line RUNGAME.BAT calling this one keeps
 * everything working.
 */
#define RUNBAT  "WAVERUN.BAT"
#define OLDBAT  "RUNGAME.BAT"

static int opt_debug = 0;       /* /debug, or debug=1 in the INI */
static int opt_dump = 0;
static int opt_dumpnet = 0;
static int view = 0;                 /* 0 = games, 1 = the eXoDOS list */
static const char *opt_dumpsel = NULL;
static const char *opt_netsel = NULL;   /* /dump NET DIR: preselect that entry */
static int opt_mustest = 0;
static int opt_diag = 0;
static const char *opt_launch = NULL;
static const char *opt_play = NULL;     /* /play DIR: a game off the server */
static int opt_name = 0;             /* argv index of /name */

/* music.c / mod.c internals exposed for the self-test */
extern volatile int mus_cseg;
extern volatile unsigned mus_coff;
extern int mus_loops;
extern FILE *mod_dumpf;
extern unsigned char __far *midi_log;       /* midi.c: the bytes sent, for /mustest */
extern unsigned midi_log_len, midi_log_cap;
extern unsigned long midi_clk;

/* /keys <script>, for the test harness: these come out of getkey() before
   the keyboard does, and with /dump the screen is dumped when they run
   out. ~ Enter, ` Esc, { } left and right, [ ] up and down, ! F3,
   ^ F1, $ F4, # Del, _ space. */
static const char *opt_keys = NULL;
static void quit(void);

/*
 * Ctrl-C, Ctrl-Break and DOS's critical errors must not end the launcher
 * behind its back: DOS would drop it with the timer still hooked and a
 * MIDI note still sounding. Ctrl-C and Ctrl-Break quit it the way Esc
 * does, at the next key; a critical error (a drive with no disk in it)
 * fails the call that met it rather than asking Abort, Retry, Fail over
 * the screen. DOS puts both vectors back when the launcher exits, and a
 * game run from here without WAVE.BAT gets them as they were.
 */
static volatile int break_hit = 0;
static void (__interrupt __far *old_int23)(void);
static void (__interrupt __far *old_int24)(void);

static void __interrupt __far on_break(void)
{
    break_hit = 1;              /* IRET: DOS carries on with the call */
}

static int __far on_crit(unsigned deverr, unsigned errcode, unsigned __far *devhdr)
{
    (void)errcode;
    (void)devhdr;
    if (deverr & 0x0800)
        return _HARDERR_FAIL;
    if (deverr & 0x2000)
        return _HARDERR_IGNORE;
    return _HARDERR_ABORT;      /* DOS allows nothing else */
}

static void guard_breaks(int on)
{
    static int saved = 0;
    if (!saved) {
        old_int23 = _dos_getvect(0x23);
        old_int24 = _dos_getvect(0x24);
        saved = 1;
    }
    if (on) {
        _dos_setvect(0x23, on_break);
        _harderr(on_crit);
    } else {
        _dos_setvect(0x23, old_int23);
        _dos_setvect(0x24, old_int24);
    }
}

static unsigned getkey(void)
{
    unsigned k;
    if (opt_keys) {
        char c = *opt_keys;
        if (c) {
            opt_keys++;
            switch (c) {
            case '~': return 0x0D;
            case '`': return 0x1B;
            case '{': return K_LEFT;
            case '}': return K_RIGHT;
            case '!': return K_F3;
            case '^': return K_F1;
            case '$': return K_F4;
            case '#': return K_DEL;
            case '[': return K_UP;
            case ']': return K_DOWN;
            case '_': return ' ';
            }
            return (unsigned char)c;
        }
        opt_keys = NULL;
        if (opt_dump) {
            scr_dump("SCREEN.BIN", "FONT.BIN", "PAL.BIN");
            quit();
        }
    }
    while (!_bios_keybrd(_KEYBRD_READY)) {
        if (break_hit)
            quit();                 /* Ctrl-C or Ctrl-Break, seen by DOS */
        mus_poll();                 /* idle: keep the playlist moving */
        ui_music_tick();            /* ... and the VU meter bouncing */
    }
    k = _bios_keybrd(_KEYBRD_READ);
    if ((k & 0xFF) == 0 || (k & 0xFF) == 0xE0)
        return k & 0xFF00;          /* extended key: scan code only */
    return k & 0x00FF;              /* ascii */
}

/* write an INI cdmount=/cdunmount= line with $ISO filled in */
static void put_template(FILE *f, const char *t, const char *iso)
{
    const char *p = t;
    while (*p) {
        if (strnicmp(p, "$ISO", 4) == 0) { fputs(iso, f); p += 4; }
        else fputc(*p++, f);
    }
    fputc('\n', f);
}

/* cdrom_storage= without a trailing backslash, so the batches'
   %WAVECDROM%\NAME never comes out as W:\\NAME. NULL when the INI is silent. */
static const char *cd_root(void)
{
    static char buf[PATH_LEN];
    const char *v = ini_global("cdrom_storage");
    int n;
    if (!v || !v[0])
        return NULL;
    strncpy(buf, v, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    n = strlen(buf);
    if (n > 1 && buf[n - 1] == '\\')
        buf[n - 1] = 0;
    return buf;
}

/* the card commands are prefixes - the image is appended - but $ISO at the
   end is how cdmount= is written, so accept it there too and drop it */
static void clean_cmd(char *dst, const char *cmd)
{
    int n;
    strncpy(dst, cmd, 79);
    dst[79] = 0;
    n = strlen(dst);
    if (n >= 4 && stricmp(dst + n - 4, "$ISO") == 0)
        n -= 4;
    while (n && (dst[n - 1] == ' ' || dst[n - 1] == '\t'))
        n--;
    dst[n] = 0;
}

static void put_cmd(FILE *f, const char *var, const char *cmd)
{
    char buf[80];
    clean_cmd(buf, cmd);
    if (buf[0])
        fprintf(f, "set %s=%s\n", var, buf);
}

/* WMODE=Network in the environment (the PicoMem image's boot menu): the
   launcher opens on the network view, Enter plays a game off the server,
   and discs come over NetDrive, mounted in software */
int wmode_network = 0;
static int wmode_net(void)
{
    const char *e = getenv("WMODE");
    return e && stricmp(e, "NETWORK") == 0;
}

/* imgmount= from the INI; SOFTWARE in network mode */
static const char *mount_mode(void)
{
    return wmode_network ? "SOFTWARE" : ini_global("imgmount");
}

/* What the game batches need to know about discs, from the INI: where the
   images are (cdrom_storage=), how to mount them (imgmount=SOFTWARE, or a
   card with its own CD-ROM emulation: PICOGUS, PICOMEM), the card's command
   (cdmount_<mode>=, the image appended) and the letter its disc appears on
   (cdrom_letter=). cdrom_name=1 passes the image's name alone, not its path. */
static void emit_cd_env(FILE *f)
{
    const char *v;
    char mode[16], key[24];
    int i;

    if ((v = cd_root()) != NULL)
        fprintf(f, "set WAVECDROM=%s\n", v);
    v = mount_mode();
    if (v && v[0]) {
        for (i = 0; i < (int)sizeof(mode) - 1 && v[i]; i++)
            mode[i] = (char)toupper((unsigned char)v[i]);
        mode[i] = 0;
        fprintf(f, "set IMGMOUNT=%s\n", mode);
        if (stricmp(mode, "SOFTWARE") != 0) {
            sprintf(key, "cdmount_%s", mode);
            v = ini_global(key);
            if (v && v[0])
                put_cmd(f, "WAVECDCMD", v);
            else if (stricmp(mode, "PICOGUS") == 0)
                fprintf(f, "set WAVECDCMD=PGUSINIT.EXE /cdloadname\n");
            sprintf(key, "cdunmount_%s", mode);
            v = ini_global(key);
            if (v && v[0])
                put_cmd(f, "WAVECDCMDU", v);
        }
    }
    if ((v = ini_global("cdrom_letter")) && v[0])
        fprintf(f, "set WAVECDL=%c\n", toupper((unsigned char)v[0]));
    if ((v = ini_global("cdrom_name")) && v[0] == '1')
        fprintf(f, "set WAVECDN=1\n");
}

/* DOSBox: its Z: drive carries the shell's programs. Returns IMGMOUNT's
   full path (a bare IMGMOUNT could resolve to a game's IMGMOUNT.BAT in
   the current directory), or NULL on real DOS. */
static const char *under_dosbox(void)
{
    static const char *const where[] = {
        "Z:\\IMGMOUNT.COM", "Z:\\SYSTEM\\IMGMOUNT.COM", "Z:\\BIN\\IMGMOUNT.COM", NULL };
    int i;
    for (i = 0; where[i]; i++)
        if (access(where[i], 0) == 0)
            return where[i];
    return NULL;
}

/*
 * The lines that put a game's CD image on D: before it runs and take it
 * off afterwards. cdmount=/cdunmount= in the INI win ($ISO stands for the
 * image); otherwise DOSBox gets IMGMOUNT and real DOS gets Jason Hood's
 * SHSUCDHD (image as a CD device) + SHSUCDX (drive letter) from the
 * launcher's folder, in their 8086 builds on anything below a 386.
 */
/*
 * What a game's IMGMOUNT.BAT says about itself: bit 1 if it fetches its
 * disc over NetDrive (only then is the server address worth setting -
 * two variables saved on every local disc), bit 2 if it names an image,
 * which debug mode then shows the mount command for.
 *
 * Read through the DOS calls rather than stdio, and before RUNGAME.BAT is
 * opened. The launcher's data segment is a hair under its 64K: there is
 * room for one open stream and its buffer, not two, and the second one
 * fails with "not enough memory to allocate file structures".
 */
#define BAT_NETDRIVE 1
#define BAT_ISO      2
#define BAT_GEN      4          /* generated: ours to rewrite */
#define BAT_OWN      8          /* rewritten just now, values written in */

static int bat_scan(const Game *g, char *iso)
{
    char path[PATH_LEN + 24], buf[160];
    int h, flags = 0, keep = 0, n, e;

    iso[0] = 0;
    sprintf(path, "%s\\%s\\IMGMOUNT.BAT", gamedir, g->dir);
    h = open(path, O_RDONLY | O_BINARY);
    if (h < 0)
        return 0;
    while ((n = read(h, buf + keep, 128)) > 0) {
        char *dot;
        n += keep;
        buf[n] = 0;
        if (strstr(buf, "NETDRIVE"))
            flags |= BAT_NETDRIVE;
        if (strstr(buf, "waveserve made") || strstr(buf, "WAVE86 wrote"))
            flags |= BAT_GEN;
        for (e = 0; e < 2 && !iso[0]; e++)  /* a cue sheet first, then an ISO */
            for (dot = strstr(buf, e ? ".ISO" : ".CUE"); dot; dot = strstr(dot + 1, e ? ".ISO" : ".CUE")) {
                char *s = dot;
                while (s > buf && s[-1] != '\\' && s[-1] != ' ' && s[-1] != ':')
                    s--;
                if (s > buf && dot - s <= 8) {   /* a whole name, delimiter and all */
                    memcpy(iso, s, (unsigned)(dot - s) + 4);
                    iso[(dot - s) + 4] = 0;
                    flags |= BAT_ISO;
                    break;
                }
            }
        keep = n < 20 ? n : 20;         /* a name or marker split across reads */
        memmove(buf, buf + n - keep, keep);
    }
    close(h);
    return flags;
}

/* where this machine keeps the image for a game: cdrom_storage if the INI
   names a folder for all of them, else the game's own CD folder */
static void disc_path(const Game *g, const char *iso, char *dst)
{
    const char *root = cd_root();
    if (root && root[0])
        sprintf(dst, "%s\\%s", root, iso);
    else
        sprintf(dst, "%s\\%s\\CD\\%s", gamedir, g->dir, iso);
}

static char dbg_mount[100] = "";    /* the mount line, for debug mode */

/*
 * Write the game's IMGMOUNT.BAT from this machine's WAVE86.INI, with the
 * image path, the card's command and the letter written into it as they
 * are. The server ships one that reads all of that from the environment,
 * which asks the shell for five SETs it may not have room for, and which
 * cannot know what card this machine has anyway; this one needs only
 * CD, which the game's own batch reads. It also means a change in the
 * INI reaches a game that is already installed.
 *
 * Only ever replaces a batch that was generated - the server's or one of
 * ours - and never one whose disc comes over NetDrive: that is the
 * server's business and needs its address at run time.
 */
static int write_imgmount(const Game *g, const char *img, const char *iso)
{
    char path[PATH_LEN + 24], mode[16], cmd[80], cmdu[80];
    const char *v, *sep;
    char letter = 'D';
    int byname = 0, card, i, settle = 1, manual = 0;
    FILE *f;

    mode[0] = cmd[0] = cmdu[0] = 0;
    if ((v = mount_mode()) != NULL)
        for (i = 0; i < (int)sizeof(mode) - 1 && v[i]; i++) {
            mode[i] = (char)toupper((unsigned char)v[i]);
            mode[i + 1] = 0;
        }
    card = mode[0] && stricmp(mode, "SOFTWARE") != 0;
    if (card) {
        char key[24];
        sprintf(key, "cdmount_%s", mode);
        if ((v = ini_global(key)) != NULL && v[0])
            clean_cmd(cmd, v);
        else if (stricmp(mode, "PICOGUS") == 0)
            strcpy(cmd, "PGUSINIT.EXE /cdloadname");
        sprintf(key, "cdunmount_%s", mode);
        if ((v = ini_global(key)) != NULL && v[0])
            clean_cmd(cmdu, v);
        /* a card with no command for it yet (the PicoMem 2, whose discs are
           picked on the card): the batch asks for the disc and waits */
        manual = !cmd[0];
    }
    if ((v = ini_global("cdrom_letter")) != NULL && v[0])
        letter = (char)toupper((unsigned char)v[0]);
    if ((v = ini_global("cdrom_name")) != NULL && v[0] == '1')
        byname = 1;
    /* A card takes a moment to present a disc it has just been handed, and
       a game that looks straight away finds an empty drive: wait for a key
       unless cdrom_pause=0 says the card is quick enough. */
    if ((v = ini_global("cdrom_pause")) != NULL && v[0] == '0')
        settle = 0;

    sprintf(path, "%s\\%s\\IMGMOUNT.BAT", gamedir, g->dir);
    f = fopen(path, "w");
    if (!f)
        return 0;
    sep = home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\";
    fprintf(f, "@echo off\n");
    fprintf(f, "if \"%%1\"==\"/U\" goto unmount\n");
    fprintf(f, "rem WAVE86 wrote this from WAVE86.INI, and writes it again\n");
    fprintf(f, "rem at every launch. The game's batch calls it and reads CD.\n");
    fprintf(f, "if exist Z:\\IMGMOUNT.COM goto dosbox\n");
    fprintf(f, "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto dosboxx\n");
    if (card && manual) {
        strcpy(dbg_mount, "(the disc is loaded on the card by hand)");
        fprintf(f, "echo.\n");
        fprintf(f, "echo   PLEASE LOAD %s ON YOUR %s NOW\n", iso, mode);
        fprintf(f, "echo   (the image is %s)\n", img);
        fprintf(f, "echo   and press any key when the card has it.\n");
        fprintf(f, "pause > NUL\n");
        fprintf(f, "set CD=%c\n", letter);
        fprintf(f, "goto done\n");
    } else if (card) {
        /* Stand in the image's folder while the card loads it, the way you
           would at the prompt: a bare name (cdrom_name=1) is looked up in
           the current directory, and the game batch that calls this one is
           standing in the game's folder. Back there afterwards - by name,
           since the image may be on the same drive. */
        const char *last = strrchr(img, '\\');
        if (img[1] == ':' && last) {
            fprintf(f, "%c:\n", img[0]);
            if (last - img <= 2)
                fprintf(f, "cd \\\n");
            else
                fprintf(f, "cd %.*s\n", (int)(last - img - 2), img + 2);
        }
        sprintf(dbg_mount, "%s %s", cmd, byname ? iso : img);
        /* A card that is still busy - pgusinit /mode reloads the PicoGUS
           firmware, and the game's sound= command ran just before this -
           refuses the image. Ask again rather than start a game with an
           empty drive. */
        fprintf(f, ":cdtry\n");
        fprintf(f, "%s\n", dbg_mount);
        fprintf(f, "if not errorlevel 1 goto cdok\n");
        fprintf(f, "echo The card did not take %s. It may still be switching mode.\n", iso);
        fprintf(f, "echo Press a key to try again, or Ctrl-C to give up.\n");
        fprintf(f, "pause > NUL\n");
        fprintf(f, "goto cdtry\n");
        fprintf(f, ":cdok\n");
        if (settle)
            fprintf(f, "echo The card is loading %s. Press a key once it is ready.\npause > NUL\n", iso);
        if (img[1] == ':' && last) {
            fprintf(f, "%c:\n", gamedir[0]);
            fprintf(f, "cd %s\\%s\n", gamedir, g->dir);
        }
        fprintf(f, "set CD=%c\n", letter);
        fprintf(f, "goto done\n");
    } else if (stricmp(iso + strlen(iso) - 4, ".CUE") == 0) {
        strcpy(dbg_mount, "(nothing: SHSUCDHD cannot read a cue sheet)");
        fprintf(f, "echo %s is a cue sheet, which keeps the CD audio, and SHSUCDHD\n", iso);
        fprintf(f, "echo mounts plain ISO images only. Set imgmount= to your card in\n");
        fprintf(f, "echo WAVE86.INI, or install the game again in software mode.\n");
        fprintf(f, "goto done\n");
    } else {
        sprintf(dbg_mount, "LH %s%sSHCDHD86.EXE /F:%s /Q", home_dir, sep, img);
        fprintf(f, "%s\n", dbg_mount);
        fprintf(f, "LH %s%sSHCDX86.COM /D:SHSU-CDH,%c /I /Q\n", home_dir, sep, letter);
        fprintf(f, "%s%sSHCDX86.COM /L:1 /QQ\n", home_dir, sep);
        for (i = 3; i <= 26; i++)   /* which letter SHSUCDX actually gave it */
            fprintf(f, "if errorlevel %d set CD=%c\n", i, 'A' + i - 1);
        fprintf(f, "if errorlevel 27 set CD=\n");
        fprintf(f, "if \"%%CD%%\"==\"\" set CD=%c\n", letter);
        fprintf(f, "goto done\n");
    }
    fprintf(f, ":dosbox\nZ:\\IMGMOUNT.COM %c %s -t iso\nset CD=%c\ngoto done\n",
            letter, img, letter);
    fprintf(f, ":dosboxx\nZ:\\SYSTEM\\IMGMOUNT.COM %c %s -t iso\nset CD=%c\ngoto done\n",
            letter, img, letter);
    fprintf(f, ":unmount\n");
    fprintf(f, "if exist Z:\\IMGMOUNT.COM Z:\\IMGMOUNT.COM -u %c\n", letter);
    fprintf(f, "if exist Z:\\SYSTEM\\IMGMOUNT.COM Z:\\SYSTEM\\IMGMOUNT.COM -u %c\n", letter);
    fprintf(f, "if exist Z:\\IMGMOUNT.COM goto gone\n");
    fprintf(f, "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto gone\n");
    if (card) {
        if (cmdu[0])
            fprintf(f, "%s\n", cmdu);
    } else {
        fprintf(f, "%s%sSHCDX86.COM /U /Q\n", home_dir, sep);
        fprintf(f, "%s%sSHCDHD86.EXE /U /Q\n", home_dir, sep);
    }
    fprintf(f, ":gone\nset CD=\n:done\n");
    fclose(f);
    return 1;
}

static void emit_cd(FILE *f, const Game *g, int after, int bat)
{
    char iso[PATH_LEN + 32];
    const char *t, *sep, *hd, *cdx;
    const char *dot = strrchr(g->exe, '.');

    if ((g->flags & GF_CDBAT) && dot && stricmp(dot + 1, "BAT") == 0) {
        if (bat & BAT_OWN) {            /* the batch carries its own values */
            if (after)
                fprintf(f, "call IMGMOUNT.BAT /U\n");
            return;
        }
        /* the start batch mounts the disc through its IMGMOUNT.BAT; we tell
           it where the NetDrive server is (the machine of server=, port
           netdrive_port=) and take everything down afterwards */
        if (after) {
            fprintf(f, "call IMGMOUNT.BAT /U\n");
        } else {
            char host[32];
            char *colon;
            const char *v;
            if (bat & BAT_NETDRIVE) {
                strncpy(host, cfg_server, sizeof(host) - 1);
                host[sizeof(host) - 1] = 0;
                colon = strchr(host, ':');
                if (colon) *colon = 0;
                v = ini_global("netdrive_port");
                if (host[0])
                    fprintf(f, "set WAVENDSRV=%s:%s\n", host, v && v[0] ? v : "2002");
                v = ini_global("netdrive");
                if (v && v[0])
                    fprintf(f, "set WAVEND=%c\n", toupper((unsigned char)v[0]));
            }
            emit_cd_env(f);
        }
        return;
    }
    if (!g->cdimg[0])
        return;
    if (strchr(g->cdimg, ':') || g->cdimg[0] == '\\')
        strcpy(iso, g->cdimg);
    else
        sprintf(iso, "%s\\%s\\%s", gamedir, g->dir, g->cdimg);
    t = ini_global(after ? "cdunmount" : "cdmount");
    if (t && t[0]) {
        put_template(f, t, iso);
        return;
    }
    if ((t = under_dosbox()) != NULL) {
        if (after) fprintf(f, "%s -u D\n", t);
        else       fprintf(f, "%s D %s -t iso\n", t, iso);
        return;
    }
    sep = home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\";
    hd = "SHCDHD86.EXE";
    cdx = "SHCDX86.COM";
    if (cpu_level() >= 3) {             /* the 386 builds, when shipped */
        char tool[PATH_LEN + 16];
        sprintf(tool, "%s%sSHSUCDX.COM", home_dir, sep);
        if (access(tool, 0) == 0) {
            hd = "SHSUCDHD.EXE";
            cdx = "SHSUCDX.COM";
        }
    }
    if (after) {
        fprintf(f, "%s%s%s /U /Q\n", home_dir, sep, cdx);
        fprintf(f, "%s%s%s /U /Q\n", home_dir, sep, hd);
    } else {
        fprintf(f, "LH %s%s%s /F:%s /Q\n", home_dir, sep, hd, iso);
        fprintf(f, "LH %s%s%s /D:SHSU-CDH,D /I /Q\n", home_dir, sep, cdx);
    }
}

/*
 * Free XMS, through the driver itself: INT 2Fh AX=4300 says whether one is
 * loaded, AX=4310 hands back its entry point, and function 08h returns the
 * largest free block and the total free, both in KB. Games that want XMS
 * say "out of XMS memory" without saying how much they wanted or what took
 * it, and a disk cache can quietly be holding most of it.
 */
static void (__far *xms_entry)(void) = NULL;

static int xms_free(unsigned *largest, unsigned *total)
{
    union REGS r;
    struct SREGS sr;
    unsigned a = 0, d = 0;

    r.x.ax = 0x4300;
    int86(0x2F, &r, &r);
    if (r.h.al != 0x80)
        return 0;
    segread(&sr);
    r.x.ax = 0x4310;
    int86x(0x2F, &r, &r, &sr);
    xms_entry = (void (__far *)(void))MK_FP(sr.es, r.x.bx);
    _asm {
        push bx
        push cx
        mov  ah, 8
        xor  bl, bl
        call dword ptr [xms_entry]
        mov  a, ax
        mov  d, dx
        pop  cx
        pop  bx
    }
    *largest = a;
    *total = d;
    return 1;
}

/* The largest upper-memory block the XMS driver would still hand out, in
   KB (XMS function 10h asked for more than it can have says so in DX):
   0 when there is none left, -1 when it provides none at all. What LH
   and DEVICEHIGH have to work with; xms_free() must have run. */
static int umb_largest(void)
{
    unsigned a = 0, d = 0, b = 0;
    if (!xms_entry)
        return -1;
    _asm {
        push bx
        push cx
        mov  ah, 10h
        mov  dx, 0FFFFh
        call dword ptr [xms_entry]
        mov  a, ax
        mov  d, dx
        xor  bh, bh
        mov  b, bx
        pop  cx
        pop  bx
    }
    if (a == 1) return 1024;            /* it gave 64 MB: not a real answer, but not none */
    if (b == 0xB0) return (int)(d / 64);
    if (b == 0xB1) return 0;
    return -1;
}

/*
 * How much room the shell has left for SET. The batches a game needs set
 * a handful of variables (where the disc is, how to mount it, which
 * letter it landed on), and when the block is full the shell says so -
 * "Out of environment space", or 4DOS's "Out of environment/alias space"
 * - and the SET quietly does nothing, which leaves %CD% empty and the
 * game looking for its disc on drive ":".
 *
 * The block that matters is the shell's own, not the copy we were handed:
 * ours is cut to fit at load time and always looks full. The parent PSP
 * (ours + 16h) is COMMAND.COM or 4DOS, its environment segment is at
 * +2Ch, and the MCB in front of that says how big it is.
 */
static void env_space(unsigned *used, unsigned *size)
{
    unsigned seg = *(unsigned __far *)MK_FP(_psp, 0x16);   /* parent PSP */
    unsigned char __far *p;
    unsigned n = 0;

    seg = seg ? *(unsigned __far *)MK_FP(seg, 0x2C) : 0;
    if (!seg)                                   /* no parent block: ours */
        seg = *(unsigned __far *)MK_FP(_psp, 0x2C);
    if (!seg) { *used = *size = 0; return; }
    p = (unsigned char __far *)MK_FP(seg, 0);
    *size = *(unsigned __far *)MK_FP(seg - 1, 3) * 16;   /* MCB: paragraphs */
    if (*size > 32768U) { *used = *size = 0; return; }   /* not a sane block */
    while (n < *size && p[n]) {
        while (n < *size && p[n]) n++;                   /* one string */
        n++;                                             /* its NUL */
    }
    *used = n + 1;                                       /* the empty one */
    if (*used > *size) *used = *size;
}

/* debug=1 in the INI, or WAVE /debug: the batch that runs a game stops at
   each step and leaves its lines on the screen, which is the only way to
   watch a CD being mounted - the game's own batch calls IMGMOUNT.BAT, and
   with echo on its lines and the drivers' answers show up too. */
static int debug_mode(void)
{
    const char *v;
    if (opt_debug)
        return 1;
    v = ini_global("debug");
    return v && v[0] == '1';
}

/*
 * slowdown= and memlimit= from a game's section: SLOWDOWN.COM (Bret
 * Johnson's, in third-party/; make slowdown copies it in) and MEMLIM.EXE (ours) run before
 * the game and are undone after it - and undone at the top of every
 * batch too, for a game that never got to its own end. Only when the
 * program is next to the launcher: a folder without it changes nothing.
 * The slowdown value is an era (pentium, 486, 386, 286, xt), each with
 * SLOWDOWN's argument for it - slow_<era>= in the INI overrides one -
 * or SLOWDOWN's own notation: 486:40, 286:12, 25%.
 */
static const char *slow_arg(const char *v, char *arg)
{
    static const char *const eras[] = {"pentium", "486", "386", "286", "xt"};
    static const char *const args[] = {"/MHz486:150", "/MHz486:66", "/MHz486:25", "/ATSpeed", "/XTSpeed"};
    char key[16];
    const char *o;
    int i, n = strlen(v);
    if (!v[0] || stricmp(v, "0") == 0 || stricmp(v, "off") == 0)
        return NULL;
    for (i = 0; i < 5; i++)
        if (stricmp(v, eras[i]) == 0) {
            sprintf(key, "slow_%s", eras[i]);
            o = ini_global(key);
            strcpy(arg, o && o[0] ? o : args[i]);
            return arg;
        }
    if (stricmp(v, "at") == 0) { strcpy(arg, "/ATSpeed"); return arg; }
    if (n > 1 && v[n - 1] == '%') { sprintf(arg, "/Percent:%.*s", n - 1, v); return arg; }
    if (strnicmp(v, "486:", 4) == 0) { sprintf(arg, "/MHz486:%s", v + 4); return arg; }
    if (strnicmp(v, "286:", 4) == 0) { sprintf(arg, "/MHz286:%s", v + 4); return arg; }
    if (v[0] == '/') { strcpy(arg, v); return arg; }    /* SLOWDOWN's own switch, as is */
    return NULL;
}

static void emit_limits(FILE *f, const char *dir, int when)     /* 0 before, 1 after, 2 the top */
{
    char tool[PATH_LEN + 16], arg[96];     /* an INI value is up to 79 chars, plus SLOWDOWN's prefix */
    const char *sep = home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\";
    const char *v;
    int nocache, int70;

    sprintf(tool, "%s%sMEMLIM.EXE", home_dir, sep);
    if (access(tool, 0) == 0) {
        if (when == 0) {
            if ((v = ini_game(dir, "memlimit")) != NULL && atoi(v) > 0)
                fprintf(f, "%s %d\n", tool, atoi(v));
        } else
            fprintf(f, "%s /FREE > NUL\n", tool);
    }
    sprintf(tool, "%s%sSLOWDOWN.COM", home_dir, sep);
    if (access(tool, 0) == 0) {
        /* SLOWDOWN says so, beep and all, when asked to uninstall while not
           in memory (/Q keeps only its chatter down, not its errors), so
           the uninstall runs only when an install of ours left its mark:
           SLOWDOWN.ON next to the launcher, made when the install went
           well and removed with the uninstall - after the game, and at
           the top of the next batch for a game that never got to its end */
        char mark[PATH_LEN + 16];
        sprintf(mark, "%s%sSLOWDOWN.ON", home_dir, sep);
        if (when == 0) {
            if ((v = ini_game(dir, "slowdown")) != NULL && slow_arg(v, arg)) {
                /* ini_global hands out one static buffer: each value is judged before the next call */
                v = ini_global("slowcache"); nocache = v && v[0] == '0';
                v = ini_global("slowint70"); int70 = v && v[0] == '1';
                fprintf(f, "%s /Q /DisableHotKeys /Beep:No %s%s%s\n", tool, arg,
                        nocache ? " /UseCPUCache:No" : "", int70 ? " /UseInt70" : "");
                fprintf(f, "if not errorlevel 1 echo 1 > %s\n", mark);
            }
        } else {
            fprintf(f, "if exist %s %s /Q /Uninstall > NUL\n", mark, tool);
            fprintf(f, "if exist %s del %s\n", mark, mark);
        }
    }
}

static void write_bat(const Game *g, int use_setup)
{
    const char *prog = use_setup ? g->setup : g->exe;
    const char *sub = strrchr(prog, '\\');       /* SUB\GAME.EXE: run from SUB */
    const char *dot = strrchr(prog, '.');
    int is_bat = dot && stricmp(dot + 1, "BAT") == 0;
    int dbg = debug_mode();
    char iso[16], img[PATH_LEN + 24];
    int bat = (g->flags & GF_CDBAT) ? bat_scan(g, iso) : 0;   /* before the open */
    FILE *f;

    img[0] = 0;
    if (bat & BAT_ISO) {
        disc_path(g, iso, img);
        /* a generated batch that works off a local image is ours to write
           from the INI; one that fetches over NetDrive is left alone */
        if ((bat & BAT_GEN) && !(bat & BAT_NETDRIVE) && write_imgmount(g, img, iso))
            bat |= BAT_OWN;
    }
    f = fopen(RUNBAT, "w");
    if (!f)
        return;
    if (dbg) {
        fprintf(f, "@echo off\ncopy %s WAVELAST.BAT > NUL\n", RUNBAT);
        fprintf(f, "echo [WAVE86] %s, debug=1. Ctrl-C stops here.\n", g->dir);
        fprintf(f, "@echo on\n");
    } else {
        fprintf(f, "@echo off\n");
    }
    emit_limits(f, g->dir, 2);          /* what an earlier game may have left behind */
    fprintf(f, "%c:\n", gamedir[0]);
    fprintf(f, "cd %s\\%s\n", gamedir, g->dir);
    ini_emit_extras(f, g->dir, 0);     /* sound mode, env, pre */
    emit_limits(f, g->dir, 0);          /* less memory, a slower machine */
    emit_cd(f, g, 0, bat);
    if (dbg) {
        fprintf(f, "@echo off\necho [WAVE86] the environment the game will see:\n");
        fprintf(f, "set\npause\n@echo on\n");
    }
    if (dbg && (g->flags & GF_CDBAT)) {
        /* The game's own batch calls IMGMOUNT.BAT, which starts with echo
           off and gives its drivers /Q, so the mount goes past in silence.
           Say what it is about to do - the shell expands the variables in
           an echo, so empty ones show up as gaps - check the image is
           where the batch will look for it, then run it here on its own,
           look at what it produced, and take it down again before the game
           does the same thing for real. */
        fprintf(f, "@echo off\n");
        if (bat & BAT_ISO) {
            if (bat & BAT_OWN)
                fprintf(f, "echo [WAVE86] IMGMOUNT.BAT will run: %s\n", dbg_mount);
            else
                fprintf(f, "echo [WAVE86] IMGMOUNT.BAT works off the server's own values\n");
            fprintf(f, "if exist %s echo [WAVE86] and the image is there\n", img);
            fprintf(f, "if not exist %s echo [WAVE86] BUT THERE IS NO IMAGE AT %s\n", img, img);
            fprintf(f, "pause\n");
        }
        fprintf(f, "echo [WAVE86] mounting the disc on its own first:\n@echo on\n");
        fprintf(f, "call IMGMOUNT.BAT\n");
        fprintf(f, "@echo off\necho [WAVE86] IMGMOUNT.BAT is done and says CD=%%CD%%\n");
        fprintf(f, "if \"%%CD%%\"==\"\" echo [WAVE86] it set no letter at all\n");
        fprintf(f, "if not \"%%CD%%\"==\"\" dir /w %%CD%%:\\\n");
        fprintf(f, "pause\n");
        fprintf(f, "call IMGMOUNT.BAT /U\n");
        fprintf(f, "echo [WAVE86] taken down again; now the game mounts it itself\n");
        fprintf(f, "pause\n@echo on\n");
    }
    /* WAVESHELL, when the boot set it: a batch game runs in that shell -
       the image's item 3 gives 4DOS, swapping, only while an eXoDOS batch
       runs, under FreeCOM, so a game gets the memory 4DOS would hold */
    if (sub)
        fprintf(f, "cd %.*s\n", (int)(sub - prog), prog);
    fprintf(f, "%s%s", is_bat ? "%WAVESHELL% call " : "", sub ? sub + 1 : prog);
    if (!use_setup && g->args[0])
        fprintf(f, " %s", g->args);
    fprintf(f, "\n");
    if (sub)                            /* back in the game's folder for what comes after */
        fprintf(f, "cd %s\\%s\n", gamedir, g->dir);
    if (dbg) {                         /* the disc as it stands, before it goes */
        fprintf(f, "@echo off\npause\n");
        fprintf(f, "echo [WAVE86] the game has finished. CD=%%CD%%\n");
        fprintf(f, "if not \"%%CD%%\"==\"\" dir /w %%CD%%:\\\n");
        fprintf(f, "pause\n@echo on\n");
    }
    emit_cd(f, g, 1, bat);
    emit_limits(f, g->dir, 1);          /* full speed and all the memory again */
    ini_emit_extras(f, g->dir, 1);     /* post */
    if (dbg)
        fprintf(f, "pause\n@echo off\n");
    fprintf(f, "%c:\n", launcher_dir[0]);
    fprintf(f, "cd %s\n", launcher_dir);
    fclose(f);
}

static void redraw(int sel, int top);

/* type a new display name for the selected game; Enter saves it to the
   INI and re-sorts, Esc leaves things alone */
/*
 * S or / asks for a few letters and goes to the next title that holds
 * them, round past the end; F3, or Enter on an empty line, looks for the
 * same letters again. Returns the entry (which may be the one we are on:
 * the only one), -1 when there is nothing to look for - the caller puts
 * its status line back - or -2 when nothing matches, which the status
 * line now says.
 */
static char find_text[20];      /* in capitals; kept for F3 */
static int ini_moved;           /* the sections went to GAMES.INI at this start: say so once */

/*
 * N with no server= in the INI: ask for the address on the bottom line,
 * :8086 (waveserve's port) added when none is typed, and keep it in
 * WAVE86.INI's global part. 1 when there is a server now, 0 on Esc.
 */
static int ask_server(void)
{
    char buf[32];
    unsigned len = 0;

    buf[0] = 0;
    for (;;) {
        unsigned k;
        ui_prompt("SERVER (ADDRESS:PORT):", buf);
        k = getkey();
        if (k == 0x1B) return 0;
        if (k == 0x0D) break;
        if (k == 0x08) {
            if (len) buf[--len] = 0;
        } else if (k > 32 && k < 127 && len < sizeof(buf) - 6) {   /* room for :8086 */
            buf[len++] = (char)k;
            buf[len] = 0;
        }
    }
    if (!buf[0]) return 0;
    if (!strchr(buf, ':')) strcat(buf, ":8086");
    strncpy(cfg_server, buf, sizeof(cfg_server) - 1);
    cfg_server[sizeof(cfg_server) - 1] = 0;
    if (ini_write_global("server", cfg_server) == 0)
        ui_status("SERVER= WRITTEN TO WAVE86.INI.");
    else
        ui_status("COULD NOT WRITE WAVE86.INI: THE SERVER IS KEPT FOR THIS RUN.");
    return 1;
}

static int find_title(int net, int from, int again)
{
    char buf[20], msg[60];
    unsigned len = 0;
    int i, hit = -1;

    buf[0] = 0;
    while (!again) {
        unsigned k;
        ui_prompt("SEARCH:", buf);
        k = getkey();
        if (k == 0x1B) return -1;
        if (k == 0x0D) break;
        if (k == 0x08) {
            if (len) buf[--len] = 0;
        } else if (k >= 32 && k < 127 && len < sizeof(buf) - 1) {
            buf[len++] = (char)(k >= 'a' && k <= 'z' ? k - 32 : k);
            buf[len] = 0;
        }
    }
    if (buf[0]) strcpy(find_text, buf);
    if (!find_text[0]) return -1;
    if (net) {
        ui_prompt("SEARCHING FOR", find_text);  /* a long list on a slow disk */
        hit = net_find(find_text, from);
    } else {
        for (i = 1; i <= game_count && hit < 0; i++)
            if (text_has(games[(from + i) % game_count].name, find_text))
                hit = (from + i) % game_count;
    }
    if (hit < 0) {
        sprintf(msg, "NO TITLE HAS \"%s\" IN IT.", find_text);
        ui_status(msg);
        return -2;
    }
    if (hit == from)
        ui_status("THAT IS THE ONLY ONE.");
    return hit;
}

/*
 * F1 or ?: the menu. Everything the view does, with its key, over the
 * bottom of the screen; the arrows and Enter pick one, or the key itself
 * does. What comes back is the key the main loop would have got - so a
 * choice is handled exactly as the key is - or 0 for nothing (Esc, or an
 * item that does nothing just now). M, the music, comes back as K_MUSIC,
 * which the views also turn M into.
 */
#define GAMES_MENU(X) \
    X(g01, "ENTER", "RUN GAME",                0x0D) \
    X(g14, "S",     "SETUP PROGRAM",           's') \
    X(g02, "F2",    "RENAME",                  0x3C00) \
    X(g03, "/",     "SEARCH",                  '/') \
    X(g04, "F3",    "SEARCH NEXT",             K_F3) \
    X(g05, "DEL",   "DELETE GAME",             K_DEL) \
    X(g06, "R",     "RESCAN GAMES FOLDER",     'r') \
    X(g07, "N",     "OPEN NETWORK INSTALL",    'n') \
    X(g08, "P/D",   "DETAILS",                 'p') \
    X(g13, "O",     "GAME OPTIONS",            'o') \
    X(g16, "E",     "EDIT SELECTED ITEM",      'e') \
    X(g15, "A",     "SOUND CARD (PICOMEM)",    'a') \
    X(g09, "M",     "MUSIC ON/OFF",            K_MUSIC) \
    X(g10, "+/-",   "VOLUME CONTROL",          '+') \
    X(g11, "</>",   "PREVIOUS/NEXT SONG",      '>') \
    X(g17, "F4",    "MUSIC FORMAT",            K_F4) \
    X(g12, "ESC",   "QUIT TO DOS",             0x1B)
#define NET_MENU(X) \
    X(n01, "ENTER", "INSTALL IT, OR THE QUEUE", 0x0D) \
    X(n02, "P",     "PLAY IT OFF THE SERVER",  'p') \
    X(n03, "SPACE", "QUEUE IT, OR TAKE IT OUT", ' ') \
    X(n04, "Q",     "THE QUEUE",               'q') \
    X(n05, "/",     "SEARCH",                  '/') \
    X(n06, "F3",    "SEARCH NEXT",             K_F3) \
    X(n07, "L",     "FETCH THE LIST AGAIN",    'l') \
    X(n08, "C",     "LOCAL/NET CD",            'c') \
    X(n09, "U",     "UPDATE WAVE86",           'u') \
    X(n10, "M",     "MUSIC ON/OFF",            K_MUSIC) \
    X(n11, "+/-",   "VOLUME CONTROL",          '+') \
    X(n12, "</>",   "PREVIOUS/NEXT SONG",      '>') \
    X(n14, "F4",    "MUSIC FORMAT",            K_F4) \
    X(n13, "ESC",   "BACK TO THE GAMES",       0x1B)
/* the text and the tables live in far memory: the data segment is full */
#define MENU_TEXT(id, k, l, c) static const char __far id##k_[] = k, id##l_[] = l;
#define MENU_ITEM(id, k, l, c) { id##k_, id##l_, c },
GAMES_MENU(MENU_TEXT)
NET_MENU(MENU_TEXT)
static const MenuItem __far menu_games[] = { GAMES_MENU(MENU_ITEM) };
static const MenuItem __far menu_net[]   = { NET_MENU(MENU_ITEM) };
#define MENU_ROWS 9
static int menu_sel[2];         /* where the bar was, per view */

/* The PicoMEM's sound cards, switched from the menu: PMINIT takes the
   switch and sets the card up. The image's AUTOEXEC runs the Sound
   Blaster (and the GUS when its files are there) at boot; this is for
   a game that wants the other one. The command runs through the batch
   loop like a game, and the launcher comes back. */
#define PMINIT "C:\\PICOMEM\\PMINIT.EXE"
#define SOUND_MENU(X) \
    X(s01, "1", "SOUND BLASTER     PMINIT /SB 1",  '1') \
    X(s02, "2", "GRAVIS ULTRASOUND PMINIT /GUS 1", '2')
SOUND_MENU(MENU_TEXT)
static const MenuItem __far menu_sound[] = { SOUND_MENU(MENU_ITEM) };
static void hand_off(const char *msg);

static void sound_card_menu(void)
{
    static const char __far *cmds[] = { "/SB 1", "/GUS 1" };
    static int sel = 0;
    unsigned char dim[2] = { 0, 0 };
    for (;;) {
        unsigned k;
        int pick = -1;
        ui_menu(" SOUND CARD ", menu_sound, 2, sel, dim);
        k = getkey();
        if (k == 0x1B) return;
        if (k == '1' || k == '2') pick = k - '1';
        else if (k == 0x0D) pick = sel;
        else if (k == K_UP && sel) sel--;
        else if (k == K_DOWN && sel < 1) sel++;
        if (pick >= 0) {
            char msg[80];
            FILE *f = fopen(RUNBAT, "w");
            if (!f) return;
            fprintf(f, "@echo off\n%s %Fs\n", PMINIT, cmds[pick]);
            fclose(f);
            sel = pick;
            sprintf(msg, "WAVE86: PMINIT %Fs ...", cmds[pick]);
            hand_off(msg);
            return;
        }
    }
}

static unsigned menu_pick(int net, int cur)
{
    const MenuItem __far *items = net ? menu_net : menu_games;
    int n = net ? (int)(sizeof(menu_net) / sizeof(*menu_net)) : (int)(sizeof(menu_games) / sizeof(*menu_games));
    int sel = menu_sel[net], i;
    unsigned char dim[2 * MENU_ROWS];
    int nomusic = !mus_present || !mus_ntracks;

    for (i = 0; i < n; i++) {
        unsigned c = items[i].code;
        int d = 0;
        if (c == K_MUSIC || c == '+' || c == '-' || c == '<' || c == '>') d = nomusic;
        else if (c == K_F4) d = !mus_present;      /* a style can bring tracks back */
        else if (net) {
            if (c == 'q') d = !net_qcount;
            else if (c == 'p') d = !net_count || !net_get(cur)->netplay;
            else if (c == 0x0D || c == ' ' || c == '/' || c == K_F3) d = !net_count;
        } else {
            if (c == 's') d = !game_count || !games[cur].setup[0];
            else if (c == 'a') d = access(PMINIT, 0) != 0;
            else if (c != 'r' && c != 0x1B && c != 'p' && c != 'n') d = !game_count;
        }
        dim[i] = (unsigned char)d;
    }
    for (;;) {
        unsigned k;
        ui_menu(" MENU ", items, n, sel, dim);
        k = getkey();
        if (k == 0x1B || k == '?' || k == K_F1) return 0;
        if (k == 'm' || k == 'M') k = K_MUSIC;
        if (k == 0x0D) { menu_sel[net] = sel; return dim[sel] ? 0 : items[sel].code; }
        if (k >= 'a' && k <= 'z') k -= 32;
        if (!net && k == 'D') k = 'P';              /* D is the other key for the details */
        /* the music keys that share an item: the view does the right thing */
        if (k == '=' || k == '-' || k == '_' || k == '<' || k == ',' || k == '.')
            return nomusic ? 0 : k;
        for (i = 0; i < n; i++) {
            unsigned c = items[i].code;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == k) { menu_sel[net] = i; return dim[i] ? 0 : items[i].code; }
        }
        switch (k) {
        case K_UP:    sel--; break;
        case K_DOWN:  sel++; break;
        case K_LEFT:  if (sel >= MENU_ROWS) sel -= MENU_ROWS; break;
        case K_RIGHT: if (sel + MENU_ROWS < n) sel += MENU_ROWS; break;
        case K_HOME:  sel = 0; break;
        case K_END:   sel = n - 1; break;
        }
        if (sel < 0) sel = n - 1;
        if (sel >= n) sel = 0;
    }
}

/*
 * O: a game's options - what to do to the machine before it runs, kept
 * in its INI section: slowdown= (percent of full speed, 0 for none),
 * memlimit= (MB the game may see, 0 for all), sound= (a soundcmd_ mode).
 * Left and right step a value, Enter writes them, Esc leaves them.
 */
static const char *const slow_names[] = {"", "pentium", "486", "386", "286", "xt"};
static const char *const slow_shown[] = {"OFF", "PENTIUM 133", "486 DX2-66", "386 DX-33", "286 AT, 8 MHZ", "XT, 4.77 MHZ"};
static const unsigned char mem_levels[] = {0, 4, 8, 12, 16, 24, 31, 63};   /* 31 and 63: under the 32 and 64 MB bugs */
#define NLEVELS(a) ((int)(sizeof(a) / sizeof(*(a))))

static int mem_level_of(const char *v)
{
    int want = v ? atoi(v) : 0, i;
    if (want <= 0) return 0;
    for (i = 1; i < NLEVELS(mem_levels); i++)       /* the first one at or above */
        if (mem_levels[i] >= want) return i;
    return NLEVELS(mem_levels) - 1;
}

static int slow_level_of(const char *v)
{
    int i;
    for (i = 1; v && i < NLEVELS(slow_names); i++)
        if (stricmp(v, slow_names[i]) == 0) return i;
    return 0;
}

static void game_options(int *sel, int *top)
{
    static const char *const labels[] = {"CPU SLOWDOWN", "MEMORY LIMIT", "SOUND MODE"};
    char modes[8][16], vslow[32], vmem[24], vsnd[20], dir[FN_LEN], title[NAME_LEN + 12];
    char cslow[16], cmem[8];        /* a hand-written value the presets do not have */
    /* the eras are SLOWDOWN's calibrated model, not this machine's clock: see slow_arg() */
    const char *values[3];
    const char *v;
    int nmodes = ini_sound_modes(modes, 8), i;
    int slow, mem, snd = 0, row = 0;
    int dirty[3] = {0, 0, 0};       /* Enter writes only what the arrows touched */
    const Game *g = &games[*sel];

    strcpy(dir, g->dir);
    sprintf(title, " OPTIONS: %.40s ", g->name);
    cslow[0] = cmem[0] = 0;
    v = ini_game(dir, "slowdown");
    slow = slow_level_of(v);
    if (v && v[0] && !slow && stricmp(v, "0") != 0 && stricmp(v, "off") != 0) {
        strncpy(cslow, v, 15);      /* 486:40, 25%, a switch: shown as it is, left as it is */
        cslow[15] = 0;
    }
    v = ini_game(dir, "memlimit");
    mem = mem_level_of(v);
    if (v && atoi(v) > 0 && atoi(v) != mem_levels[mem]) {
        strncpy(cmem, v, 7);
        cmem[7] = 0;
    }
    if ((v = ini_game(dir, "sound")) != NULL)
        for (i = 0; i < nmodes; i++)
            if (stricmp(modes[i], v) == 0) snd = i + 1;
    for (;;) {
        unsigned k;
        if (cslow[0] && !dirty[0]) sprintf(vslow, "CUSTOM: %s", cslow);
        else strcpy(vslow, slow_shown[slow]);
        if (cmem[0] && !dirty[1]) sprintf(vmem, "CUSTOM: %s MB", cmem);
        else if (mem) sprintf(vmem, "%d MB", mem_levels[mem]); else strcpy(vmem, "OFF");
        if (snd) { for (i = 0; modes[snd - 1][i] && i < 15; i++) vsnd[i] = (char)toupper((unsigned char)modes[snd - 1][i]); vsnd[i] = 0; }
        else strcpy(vsnd, nmodes ? "(DEFAULT)" : "(NO SOUNDCMD LINES)");
        values[0] = vslow; values[1] = vmem; values[2] = vsnd;
        ui_options(title, labels, values, 3, row);
        k = getkey();
        if (k == 0x1B) break;
        if (k == 0x0D) {
            char num[8];
            if (dirty[0]) {
                if (slow) ini_write_key(dir, "slowdown", slow_names[slow]);
                else ini_remove_key(dir, "slowdown");
            }
            if (dirty[1]) {
                if (mem) { sprintf(num, "%d", mem_levels[mem]); ini_write_key(dir, "memlimit", num); }
                else ini_remove_key(dir, "memlimit");
            }
            if (dirty[2]) {
                if (snd) ini_write_key(dir, "sound", modes[snd - 1]);
                else ini_remove_key(dir, "sound");
            }
            scan_games();               /* the fields come back from the file */
            i = find_game(dir);
            if (i >= 0) *sel = i;
            break;
        }
        switch (k) {
        case K_UP:   if (row > 0) row--; break;
        case K_DOWN: if (row < 2) row++; break;
        case K_LEFT: case K_RIGHT: {
            int d = k == K_LEFT ? -1 : 1;
            if (row == 0) slow = (slow + d + NLEVELS(slow_names)) % NLEVELS(slow_names);
            else if (row == 1) mem = (mem + d + NLEVELS(mem_levels)) % NLEVELS(mem_levels);
            else if (nmodes) snd = (snd + d + nmodes + 1) % (nmodes + 1);
            if (row < 2 || nmodes) dirty[row] = 1;
            break;
        }
        }
    }
    if (*sel < *top || *sel >= *top + PAGE) *top = *sel > 6 ? *sel - 6 : 0;
    redraw(*sel, *top);
}

/*
 * E: a game's properties - its name, the program that starts it, its
 * setup program, the arguments and its CD image - in a box like O's, and
 * into its section of the INI on Enter. The selected row is typed into,
 * Del empties it, left and right step through the programs in the folder
 * (and one folder down) on the PROGRAM and SETUP rows. An empty PROGRAM or
 * SETUP goes back to what the scan finds; a program that is not there is
 * not saved.
 */
#define ED_ROWS 5
static void edit_game(int *sel, int *top)
{
    static const char *const labels[ED_ROWS] = {"NAME", "PROGRAM", "SETUP", "ARGUMENTS", "CD IMAGE"};
    static const char *const keys[ED_ROWS] = {"name", "exe", "setup", "args", "cd"};
    static const unsigned char lens[ED_ROWS] = {NAME_LEN, EXE_LEN, EXE_LEN, 32, 20};
    static char progs[24][EXE_LEN];
    static char val[ED_ROWS][NAME_LEN], orig[ED_ROWS][NAME_LEN];
    char dir[FN_LEN], title[NAME_LEN + 12], msg[80];
    const char *values[ED_ROWS];
    int nprog, row = 1, i, shown = 0;
    const Game *g = &games[*sel];

    strcpy(dir, g->dir);
    sprintf(title, " EDIT: %.40s ", g->name);
    strcpy(val[0], g->name);
    strcpy(val[1], g->exe);
    strcpy(val[2], g->setup);
    strcpy(val[3], g->args);
    strcpy(val[4], g->cdimg);
    for (i = 0; i < ED_ROWS; i++) {
        strcpy(orig[i], val[i]);
        values[i] = val[i];
    }
    ui_status("SCANNING THE FOLDER ...");
    nprog = scan_programs(dir, progs, 24);
    ui_status(NULL);
    for (;;) {
        unsigned k;
        unsigned len = strlen(val[row]);
        ui_game_edit(title, labels, values, ED_ROWS, row, nprog > 0 && (row == 1 || row == 2), shown ? msg : NULL);
        k = getkey();
        shown = 0;
        if (k == 0x1B)
            break;
        if (k == 0x0D) {
            const char *wkeys[ED_ROWS], *wvals[ED_ROWS];
            int nw = 0, bad = 0;
            for (i = 1; i <= 2; i++)            /* a program that is not there is no use */
                if (val[i][0] && strcmp(val[i], orig[i])) {
                    char path[PATH_LEN + EXE_LEN + 12];
                    const char *dot = strrchr(val[i], '.');
                    sprintf(path, "%s\\%s\\%s", gamedir, dir, val[i]);
                    if (!dot || (stricmp(dot, ".EXE") && stricmp(dot, ".COM") && stricmp(dot, ".BAT"))) {
                        sprintf(msg, "%.21s IS NOT A PROGRAM (EXE, COM, BAT): NOT SAVED", val[i]);
                        shown = 1;
                        row = i;
                        bad = 1;
                        break;
                    }
                    if (access(path, 0) != 0) {
                        sprintf(msg, "%.21s IS NOT IN %.8s: NOT SAVED", val[i], dir);
                        shown = 1;
                        row = i;
                        bad = 1;
                        break;
                    }
                }
            if (bad)
                continue;
            for (i = 0; i < ED_ROWS; i++) {
                if (!strcmp(val[i], orig[i]))
                    continue;
                if (val[i][0] || i == 2 || i == 4) {
                    wkeys[nw] = keys[i];            /* an empty setup= or cd= is none, */
                    wvals[nw++] = val[i];           /* whatever the scan would find */
                } else {
                    ini_remove_key(dir, keys[i]);   /* the name and program: back to the scan's */
                }
            }
            if (nw)
                ini_write_keys(dir, wkeys, wvals, nw);
            scan_games();               /* the fields come back from the file */
            i = find_game(dir);
            if (i >= 0) *sel = i;       /* or it left the list: nothing to run, no showempty */
            else if (*sel >= game_count) *sel = game_count ? game_count - 1 : 0;
            if (*top > game_count - PAGE) *top = game_count - PAGE;
            if (*top < 0) *top = 0;
            break;
        }
        switch (k) {
        case K_UP:   if (row > 0) row--; break;
        case K_DOWN: if (row < ED_ROWS - 1) row++; break;
        case K_LEFT: case K_RIGHT:
            if ((row == 1 || row == 2) && nprog) {
                int at = -1, d = k == K_LEFT ? -1 : 1;
                for (i = 0; i < nprog; i++)
                    if (!stricmp(val[row], progs[i])) at = i;
                if (at < 0 && d < 0) at = 0;    /* from nothing: left lands on the last */
                at = (at + d + nprog) % nprog;
                strcpy(val[row], progs[at]);
            }
            break;
        case K_DEL:
            val[row][0] = 0;
            break;
        case 0x08:
            if (len) val[row][len - 1] = 0;
            break;
        default:
            if (k >= 32 && k < 127 && len < (unsigned)lens[row] - 1) {
                val[row][len] = (char)(row == 0 || row == 3 ? k : toupper(k));
                val[row][len + 1] = 0;
            }
        }
    }
    if (*sel < *top || *sel >= *top + PAGE) *top = *sel > 6 ? *sel - 6 : 0;
    redraw(*sel, *top);
}

/* left and right turn the page: the list moves, the bar stays on its row */
static void turn_page(int *sel, int *top, int count, int dir)
{
    int row = *sel - *top, was = *top;
    if (count <= PAGE) { *sel = dir < 0 ? 0 : count - 1; return; }
    *top += dir * PAGE;
    if (*top > count - PAGE) *top = count - PAGE;
    if (*top < 0) *top = 0;
    if (*top == was)
        *sel = dir < 0 ? 0 : count - 1;     /* the first or last page already: go to its end */
    else {
        *sel = *top + row;
        if (*sel >= count) *sel = count - 1;
    }
}

static void edit_name(int *sel, int *top)
{
    char buf[NAME_LEN], dir[FN_LEN], oldname[NAME_LEN];
    unsigned len;

    strcpy(buf, games[*sel].name);
    strcpy(oldname, buf);
    strcpy(dir, games[*sel].dir);
    len = strlen(buf);
    ui_status("TYPE THE NAME. ENTER SAVES, ESC CANCELS.");
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

static void net_status(void);

static void net_redraw(int sel, int top)
{
    ui_net_static();
    ui_net_list(sel, top);
    ui_net_details(sel);
    ui_net_keybar(sel);
    net_status();
}

static void net_status(void)
{
    char msg[60];
    if (net_qcount)
        sprintf(msg, "%d QUEUED OF %d GAMES ON %s", net_qcount, net_count,
                cfg_server[0] ? cfg_server : "?");
    else
        sprintf(msg, "%d GAMES ON %s", net_count, cfg_server[0] ? cfg_server : "?");
    ui_status(msg);
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
static void hand_off(const char *msg);

/*
 * A WAVE.BAT from before the rename drives RUNGAME.BAT and would find
 * nothing to do, so every hand-off also leaves a one-liner by that name
 * calling ours. There is no telling which wrapper is in charge - WAVE runs
 * off the PATH, so its folder is not the one we are in - and there is no
 * need to: an old loop runs the one-liner, and a new one runs WAVERUN.BAT
 * and deletes both.
 */
static void shim_old_wave_bat(void)
{
    FILE *f = fopen(OLDBAT, "w");
    if (!f)
        return;
    fprintf(f, "@echo off\ncall %s\n", RUNBAT);
    fclose(f);
}

/* run a command line (WAVEGET) through the same batch loop as a game */
static void run_command(const char *cmd, const char *what)
{
    FILE *f = fopen(RUNBAT, "w");
    if (!f)
        return;
    fprintf(f, "@echo off\n%s\n%c:\ncd %s\n", cmd, launcher_dir[0], launcher_dir);
    fclose(f);
    hand_off(what);
}

static void launch(const Game *g, int use_setup)
{
    char msg[96];                       /* a setup one folder down is 21 characters */
    write_bat(g, use_setup);
    if (use_setup)
        sprintf(msg, "WAVE86: Running %.21s for %.39s ...", g->setup, g->name);
    else
        sprintf(msg, "WAVE86: Running %s ...", g->name);
    hand_off(msg);
}

/* msg is what the user reads while the batch loop takes over */
static void hand_off(const char *msg)
{
    if (getenv("WAVE86")) {
        shim_old_wave_bat();
        mus_shutdown();
        text_mode_plain();
        printf("%s\n", msg);
        exit(0);
    }

    mus_release();
    text_mode_plain();
    printf("%s\n", msg);
    printf("(type WAVE instead to give games all memory)\n");
    guard_breaks(0);                /* the game gets DOS's Ctrl-C and critical errors */
    system(RUNBAT);
    guard_breaks(1);
    remove(RUNBAT);
    remove(OLDBAT);

    vid_text_mode();
    vid_set_palette();
    mus_init();
}

static void waveget_path(char *dst)
{
    sprintf(dst, "%s%sWAVEGET.EXE", home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\");
}

/* fetch NETLIST.TXT; comes back into the network view */
static void net_fetch_list(void)
{
    char cmd[PATH_LEN * 2 + 64], exe[PATH_LEN + 16];
    if (!cfg_server[0]) {
        ui_status("PUT server=A.B.C.D:8086 IN WAVE86.INI FIRST.");
        return;
    }
    waveget_path(exe);
    net_mark_view();
    sprintf(cmd, "%s LIST %s %s%sNETLIST.TXT", exe, cfg_server, home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\");
    run_command(cmd, "WAVE86: Fetching the game list from the eXoDOS server ...");
    net_load();                     /* bare mode: we are back already */
}

/* A fresh WAVE86.EXE (and WAVEGET, DRVOFF) from the machine that builds
   them, into this folder: the short way round the floppy shuffle when
   testing on real hardware. Under WAVE.BAT we never come back here - the
   batch loop starts the new EXE - and started bare we leave rather than
   carry on running the version that has just been replaced. */
static void net_update(void)
{
    char cmd[PATH_LEN * 2 + 64], exe[PATH_LEN + 16];
    if (!cfg_server[0]) {
        ui_status("PUT server=A.B.C.D:8086 IN WAVE86.INI FIRST.");
        return;
    }
    waveget_path(exe);
    sprintf(cmd, "%s UPDATE %s %s", exe, cfg_server, home_dir);
    run_command(cmd, "WAVE86: Fetching a fresh WAVE86 from the server ...");
    quit();
}

/* one WAVEGET GET line: the server key is DIR for eXoDOS, src:DIR for
   other collections, and for a CD game the mode decides whether the disc
   comes along */
/* what WAVEGET asks the server for: the game, and how its disc comes */
static void net_key(char *key, const NetGame *g)
{
    if (stricmp(g->src, "exodos") == 0) strcpy(key, g->dir);
    else sprintf(key, "%s:%s", g->src, g->dir);
    /* NET CD leaves the disc on the server; otherwise a card that mounts
       cue/bin itself gets the discs as they came, audio tracks and all,
       and SHSUCDHD - which reads plain ISOs only - gets the converted ISO */
    if (g->cd) {
        if (g->netcd && net_cdmode)
            strcat(key, "?cd=net");
        else
            strcat(key, net_rawcd && g->rawkb ? "?cd=raw" : "?cd=local");
    }
}

static void net_get_cmd_raw(char *cmd, unsigned size, const char *dir, unsigned long kb, const char *key, const char *exe)
{
    const char *cdrom;
    sprintf(cmd, "%s GET %s %s %s %lu %s", exe, cfg_server, dir, gamedir, kb, key);
    cdrom = cd_root();
    if (cdrom && cdrom[0] && strlen(cmd) + strlen(cdrom) + 2 < size) {
        strcat(cmd, " ");
        strcat(cmd, cdrom);
    }
}

/* The game's section in the INI the moment its install starts: name,
   program and source as the server lists them, netinstall=pending, and
   what WAVEGET was asked for (its key and size), so that Enter on the
   game can fetch the rest with the same request - the resume note WAVEGET
   leaves counts only against that. net_apply_pending turns it to done. */
static void net_register(const NetGame *g, const char *key)
{
    /* exe= only once the program is there (net_apply_pending): the
       server's guess goes in netexe=, and the list shows it meanwhile */
    const char *keys[6], *vals[6];
    char kb[12];
    int n = 0;
    sprintf(kb, "%lu", net_size(g));
    keys[n] = "name";       vals[n++] = g->title[0] ? g->title : g->dir;
    keys[n] = "source";     vals[n++] = g->src;
    keys[n] = "netinstall"; vals[n++] = "pending";
    keys[n] = "netkey";     vals[n++] = key;
    keys[n] = "netkb";      vals[n++] = kb;
    if (g->exe[0]) { keys[n] = "netexe"; vals[n++] = g->exe; }
    ini_write_keys(g->dir, keys, vals, n);
}

/* The game's picture on its own, before any of its files: one quiet
   WAVEGET THUMB line per game at the top of the batch, so a queue has
   every picture at once. */
static void net_thumb_line(FILE *f, const NetGame *g, const char *key, const char *exe)
{
    fprintf(f, "%s THUMB %s %s %s\\%s > NUL\n", exe, cfg_server, key, gamedir, g->dir);
}

static void net_get_cmd(char *cmd, unsigned size, const NetGame *g, const char *exe)
{
    char key[32];
    net_key(key, g);
    net_register(g, key);
    net_get_cmd_raw(cmd, size, g->dir, net_size(g), key, exe);
}

/* Enter on a game whose install never finished: WAVEGET is asked for the
   rest with the key and size the install started with. Comes back the
   way a download does, through NETGAME.TXT and net_apply_pending. */
static void net_continue(const Game *g)
{
    char cmd[PATH_LEN * 2 + 96], exe[PATH_LEN + 16], msg[80], key[32];
    const char *v, *src;
    unsigned long kb;
    FILE *f;

    if (!cfg_server[0] && !ask_server())
        return;
    v = ini_game(g->dir, "netkey");
    strncpy(key, v && v[0] ? v : g->dir, sizeof(key) - 1);
    key[sizeof(key) - 1] = 0;
    v = ini_game(g->dir, "netkb");
    kb = v ? strtoul(v, NULL, 10) : 0;
    src = ini_game(g->dir, "source");
    waveget_path(exe);
    net_pending_forget(g->dir);         /* the rest of a queue stays in NETGAME.TXT */
    net_mark_pending_raw(g->dir, g->name, g->exe, src && src[0] ? src : "exodos", kb);
    f = fopen(RUNBAT, "w");
    if (!f)
        return;
    net_get_cmd_raw(cmd, sizeof(cmd), g->dir, kb, key, exe);
    fprintf(f, "@echo off\n%s\n%c:\ncd %s\n", cmd, launcher_dir[0], launcher_dir);
    fclose(f);
    sprintf(msg, "WAVE86: Fetching the rest of %s ...", g->name);
    hand_off(msg);
}

/*
 * Download the selected game, or the whole queue one after another, and
 * come back with the first of them selected in the games view. DOS runs
 * one program at a time and the launcher hands WAVEGET the machine while
 * it fetches, so nothing can come in behind your back; a queue is the
 * next best thing - mark a few games, walk away, find them installed.
 */
static void net_download(int nsel, int whole_queue)
{
    char cmd[PATH_LEN * 2 + 96], exe[PATH_LEN + 16], msg[80];
    int n = whole_queue ? net_qcount : 0, k;
    FILE *f;

    waveget_path(exe);
    net_pending_reset();
    f = fopen(RUNBAT, "w");
    if (!f)
        return;
    fprintf(f, "@echo off\n");
    {   /* the pictures first, every one of them */
        char key[32];
        int j;
        for (j = 0; j < net_count; j++) {
            const NetGame *g = net_get(j);
            if (n ? !net_queued(g->dir) : j != nsel)
                continue;
            net_key(key, g);
            net_thumb_line(f, g, key, exe);
        }
    }
    if (!n) {
        const NetGame *g = net_get(nsel);
        int netcd = g->cd && g->netcd && net_cdmode;
        net_mark_pending(g);
        net_get_cmd(cmd, sizeof(cmd), g, exe);
        sprintf(msg, "WAVE86: Installing %s from the %s%s ...", g->title,
                stricmp(g->src, "tdc") == 0 ? "Total DOS Collection" : "eXoDOS server",
                !g->cd ? "" : netcd ? " (CD over network)" : " (CD on disk)");
        fprintf(f, "%s\n", cmd);
    } else {
        int j;
        k = 0;
        for (j = 0; j < net_count && k < n; j++) {      /* one pass, list order */
            const NetGame *g = net_get(j);
            if (!net_queued(g->dir))
                continue;
            k++;
            net_mark_pending(g);
            net_get_cmd(cmd, sizeof(cmd), g, exe);
            fprintf(f, "%s /q:%d/%d\n", cmd, k, n);
            /* 3 = the server had nothing for that one, try the next
               anyway; 2 = Esc, and then the whole queue stops */
            fprintf(f, "if errorlevel 3 goto q%d\n", k);
            fprintf(f, "if errorlevel 2 goto qstop\n");
            fprintf(f, ":q%d\n", k);
        }
        fprintf(f, ":qstop\n");
        sprintf(msg, "WAVE86: Installing %d games from the server ...", n);
    }
    fprintf(f, "%c:\ncd %s\n", launcher_dir[0], launcher_dir);
    fclose(f);
    hand_off(msg);
}

static void net_arrived_notice(void);

/* hand a download to WAVEGET, and (bare mode only) come back to the
   games list with what arrived selected */
static void net_install(int nsel, int whole_queue, int *sel, int *top)
{
    int i;
    net_download(nsel, whole_queue);
    scan_games();
    i = net_apply_pending();
    view = 0;
    net_free();
    if (i >= 0) { *sel = i; *top = *sel > 13 ? *sel - 13 : 0; }
    redraw(*sel, *top);
    if (i == -2) net_arrived_notice();
}

/*
 * Q: the install queue in a box, in the order it will be fetched, with
 * titles from the list. Del takes one out; Enter or I starts fetching
 * them, which is what a 1 says to the caller.
 */
static int queue_modal(void)
{
    /* the rows come from the far heap: the data segment has no room */
    char __far *rows = (char __far *)_fmalloc(QUEUE_MAX * QROW + QUEUE_MAX * sizeof(unsigned long));
    unsigned long __far *kbs;
    char d[9];
    int n = 0, j, sel = 0, top = 0, rc = 0;

    if (!rows) return 0;
    kbs = (unsigned long __far *)(rows + QUEUE_MAX * QROW);
#define TITLE(i) (rows + (i) * QROW)
#define DIR(i)   (rows + (i) * QROW + QT_LEN)
    for (j = 0; j < net_count && n < net_qcount; j++) {    /* one pass, list order */
        const NetGame *g = net_get(j);
        if (!net_queued(g->dir))
            continue;
        _fstrncpy(TITLE(n), g->title, QT_LEN - 1);
        TITLE(n)[QT_LEN - 1] = 0;
        _fstrcpy(DIR(n), g->dir);
        kbs[n] = net_size(g);
        n++;
    }
    for (j = 0; j < net_qcount && n < QUEUE_MAX; j++) {   /* queued before this list came */
        unsigned long kb;
        int k;
        net_queue_get(j, d, &kb);
        for (k = 0; k < n && _fstricmp(DIR(k), d); k++) ;
        if (k < n)
            continue;
        _fstrcpy(TITLE(n), d);
        _fstrcpy(DIR(n), d);
        kbs[n] = kb;
        n++;
    }
    for (;;) {
        unsigned k;
        ui_queue_box(n, sel, top, rows, kbs);
        k = getkey();
        switch (k) {
        case K_UP:   sel--; break;
        case K_DOWN: sel++; break;
        case K_PGUP: case K_LEFT:  sel -= QROWS; break;
        case K_PGDN: case K_RIGHT: sel += QROWS; break;
        case K_HOME: sel = 0; break;
        case K_END:  sel = n - 1; break;
        case K_DEL: case 0x08: case ' ':
            if (n) {
                _fstrcpy(d, DIR(sel));
                net_queue_toggle(d, 0);
                for (j = sel; j + 1 < n; j++) {
                    _fmemcpy(TITLE(j), TITLE(j + 1), QROW);
                    kbs[j] = kbs[j + 1];
                }
                n--;
                if (!n) goto out;
            }
            break;
        case 0x0D: case 'i': case 'I':
            rc = n > 0;
            goto out;
        case 0x1B: case 'q': case 'Q':
            goto out;
        }
        if (sel < 0) sel = 0;
        if (sel >= n) sel = n - 1;
        if (top > n - QROWS) top = n - QROWS;   /* fewer rows after a Del: no empty ones */
        if (top < 0) top = 0;
        if (sel < top) top = sel;
        if (sel >= top + QROWS) top = sel - QROWS + 1;
    }
out:
    _ffree(rows);
    return rc;
#undef TITLE
#undef DIR
}

/*
 * Del: the game's folder and everything in it, after a yes, and its
 * section in the INI; a disc kept on the CD storage (cdrom_storage=) is
 * not touched. The bar lands on the next game.
 */
static void uninstall(int *sel, int *top)
{
    char path[PATH_LEN + 16], msg[100], dir[FN_LEN], name[NAME_LEN];
    const Game *g = &games[*sel];
    const char *st = ini_global("cdrom_storage");
    int elsewhere = st && st[0] && (g->flags & GF_CDBAT);
    unsigned k;

    sprintf(path, "%s\\%s", gamedir, g->dir);
    if (st && st[0]) {                  /* a program dropped into the disc folder makes it a game to the scan */
        unsigned n = strlen(path);
        if (strnicmp(st, path, n) == 0 && (st[n] == 0 || st[n] == '\\')) {
            ui_status("THAT FOLDER IS THE CD STORAGE: NOT DELETING IT.");
            return;
        }
    }
    sprintf(msg, "DELETE %.60s AND EVERYTHING IN IT? Y/N", path);
    ui_status(msg);
    k = getkey();
    if (k != 'y' && k != 'Y') { ui_status(NULL); return; }
    strcpy(dir, g->dir);
    strcpy(name, g->name);
    ui_status("DELETING ...");
    if (access(path, 0) != 0 || scan_rmtree(path) == 0) {   /* a pending game may have no folder yet */
        ini_remove_section(dir);
        net_pending_forget(dir);
        if (net_queued(dir))
            net_queue_toggle(dir, 0);
        sprintf(msg, "%.40s REMOVED.%s", name, elsewhere ? " ITS DISC ON THE CD STORAGE STAYS." : "");
    } else {
        sprintf(msg, "COULD NOT REMOVE ALL OF %.50s (A FILE IN USE?)", path);
    }
    scan_games();
    if (*sel >= game_count) *sel = game_count - 1;
    if (*sel < 0) *sel = 0;
    if (*top > *sel) *top = *sel;
    if (*top > game_count - 14) *top = game_count - 14;
    if (*top < 0) *top = 0;
    redraw(*sel, *top);
    ui_status(msg);
}

/* the lines that find NetDrive's letter: WAVEND if that is one, else the
   first of D: to H: that NETDRIVE STATUS accepts */
static void emit_nd_letter(FILE *f)
{
    const char *v = ini_global("netdrive");
    char l;
    if (v && v[0])
        fprintf(f, "set WAVEND=%c\n", toupper((unsigned char)v[0]));
    fprintf(f, "if \"%%WAVEND%%\"==\"\" goto ndfind\n"
               "NETDRIVE STATUS %%WAVEND%%: > NUL\n"
               "if not errorlevel 1 goto ndok\n"
               "set WAVEND=\n"
               ":ndfind\n");
    for (l = 'D'; l <= 'H'; l++)
        fprintf(f, "if \"%%WAVEND%%\"==\"\" NETDRIVE STATUS %c: > NUL\n"
                   "if \"%%WAVEND%%\"==\"\" if not errorlevel 1 set WAVEND=%c\n", l, l);
    fprintf(f, "if \"%%WAVEND%%\"==\"\" goto nond\n:ndok\n");
}

/* play a game straight off the server: the whole game as a NetDrive
   volume, attached, run, detached; nothing is copied */
static void net_play(int nsel)
{
    char exe[PATH_LEN + 16], msg[80], host[32], key[32];
    const NetGame *g = net_get(nsel);
    const char *dot = strrchr(g->exe, '.');
    const char *v = ini_global("netdrive_port");
    char *colon;
    FILE *f;

    if (!g->netplay || !g->exe[0])
        return;
    waveget_path(exe);
    strncpy(host, cfg_server, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    colon = strchr(host, ':');
    if (colon) *colon = 0;
    if (stricmp(g->src, "exodos") == 0) strcpy(key, g->dir);
    else sprintf(key, "%s:%s", g->src, g->dir);
    net_mark_view();
    f = fopen(RUNBAT, "w");
    if (!f)
        return;
    fprintf(f, "@echo off\nset WAVENDSRV=%s:%s\n", host, v && v[0] ? v : "2002");
    fprintf(f, "set WAVECDROM=CD\nset IMGMOUNT=SOFTWARE\n");   /* the disc is on the game disk */
    emit_nd_letter(f);
    fprintf(f, "%s DISK %s %s\n", exe, cfg_server, key);
    fprintf(f, "if errorlevel 1 goto nodisk\n");
    fprintf(f, "NETDRIVE C %%WAVENDSRV%% %s.DSK %%WAVEND%%:\n", g->dir);
    fprintf(f, "if errorlevel 1 goto nodisk\n");
    fprintf(f, "%%WAVEND%%:\ncd \\\n");
    fprintf(f, "%s%s\n", dot && stricmp(dot + 1, "BAT") == 0 ? "call " : "", g->exe);
    fprintf(f, "%%WAVEND%%:\ncd \\\nif exist IMGMOUNT.BAT call IMGMOUNT.BAT /U\n");
    fprintf(f, "%c:\nNETDRIVE D %%WAVEND%%:\ngoto done\n", launcher_dir[0]);
    fprintf(f, ":nond\necho No NetDrive letter: is NETDRIVE.SYS in CONFIG.SYS?\npause\ngoto done\n");
    fprintf(f, ":nodisk\necho The server could not provide the game disk.\npause\n");
    fprintf(f, ":done\n%c:\ncd %s\n", launcher_dir[0], launcher_dir);
    fclose(f);
    sprintf(msg, "WAVE86: Playing %s off the server ...", g->title);
    hand_off(msg);
}

/* M: when nothing happens, say why - no card, or no tracks and the
   folder they were looked for in */
static void music_key(void)
{
    char msg[80];
    if (!mus_present) {
        ui_status("NO ADLIB, SOUND BLASTER OR MPU-401 FOUND (SEE WAVE86 /DIAG).");
        return;
    }
    if (!mus_ntracks) {
        if (cfg_musicstyle)
            sprintf(msg, "NO %s TRACKS TO PLAY HERE. F4 CHOOSES ANOTHER FORMAT.",
                    cfg_musicstyle == 1 ? "MIDI" : cfg_musicstyle == 2 ? "MOD" : "ADLIB");
        else
            sprintf(msg, "NO TRACKS IN %s%sMUSIC", home_dir,
                    home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\");
        ui_status(msg);
        return;
    }
    mus_toggle();
    ui_status(NULL);
}

/*
 * F4: which tracks in MUSIC\ play - all of them, or one kind - in a box
 * like the menu's, each with how many there are for this machine's cards.
 * The choice goes into WAVE86.INI as musicstyle=. Returns what the status
 * line should say, or NULL.
 */
static char __far style_lbl[4][32];
static MenuItem __far style_items[4];

static const char *music_style(void)
{
    static const char *const what[4] = { "ALL", "MIDI (MPU-401)", "MOD (BLASTER)", "ADLIB (FM)" };
    static const char __far *const keys[4] = { "1", "2", "3", "4" };
    static char msg[64];
    unsigned char dim[4];
    char title[32], buf[40];
    int i, sel = cfg_musicstyle, count[4];

    if (!mus_present)
        return "NO ADLIB, SOUND BLASTER OR MPU-401 FOUND (SEE WAVE86 /DIAG).";
    for (i = 0; i < 4; i++) {
        count[i] = mus_style_count(i);
        if (count[i] < 0)
            sprintf(buf, "%-15sNO CARD", what[i]);
        else
            sprintf(buf, "%-15s%2d TRACK%s", what[i], count[i], count[i] == 1 ? "" : "S");
        _fstrcpy(style_lbl[i], buf);
        style_items[i].key = keys[i];
        style_items[i].label = style_lbl[i];
        style_items[i].code = '1' + i;
        dim[i] = (unsigned char)(count[i] <= 0);
    }
    sprintf(title, " MUSIC FORMAT: %s ", what[sel]);
    for (;;) {
        unsigned k;
        int pick = -1;
        ui_menu(title, style_items, 4, sel, dim);
        k = getkey();
        if (k == 0x1B || k == K_F4)
            return NULL;
        if (k >= '1' && k <= '4') pick = k - '1';
        else if (k == 0x0D) pick = sel;
        else if (k == K_UP) sel = (sel + 3) % 4;
        else if (k == K_DOWN) sel = (sel + 1) % 4;
        if (pick >= 0 && !dim[pick]) {
            mus_set_style(pick);
            ini_write_global("musicstyle", mus_style_name(pick));
            sprintf(msg, "MUSIC FORMAT %s: %d TRACK%s.", what[pick], mus_ntracks,
                    mus_ntracks == 1 ? "" : "S");
            return msg;
        }
        if (pick >= 0)
            sel = pick;         /* nothing to play there: the bar goes to it, no more */
    }
}

/* a download landed whole, but the scan found nothing to run in the folder */
static void net_arrived_notice(void)
{
    char msg[96];
    sprintf(msg, "%s ARRIVED BUT NOTHING IN IT RUNS. SEE WAVE86 /DIAG",
            net_pending_dir);
    ui_status(msg);
}

int main(int argc, char **argv)
{
    int sel = 0, top = 0;
    int i, nsel0 = 0;

    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "/dump") == 0) {
            opt_dump = 1;           /* optional: /dump DIR preselects a game */
            if (i + 1 < argc && argv[i + 1][0] != '/') {
                opt_dumpsel = argv[i + 1];
                if (stricmp(opt_dumpsel, "NET") == 0) {
                    opt_dumpnet = 1;
                    if (i + 2 < argc && argv[i + 2][0] != '/') opt_netsel = argv[i + 2];
                }
            }
        }
        if (stricmp(argv[i], "/nopal") == 0) opt_nopal = 1;
        if (stricmp(argv[i], "/keys") == 0 && i + 1 < argc) opt_keys = argv[i + 1];
        if (stricmp(argv[i], "/mustest") == 0) {
            opt_mustest = 5;            /* seconds, optional argument */
            if (i + 1 < argc && atoi(argv[i + 1]) > 0)
                opt_mustest = atoi(argv[i + 1]);
        }
        if (stricmp(argv[i], "/launch") == 0 && i + 1 < argc)
            opt_launch = argv[i + 1];   /* boot straight into a game */
        if (stricmp(argv[i], "/play") == 0 && i + 1 < argc)
            opt_play = argv[i + 1];     /* the same, off the server */
        if (stricmp(argv[i], "/name") == 0 && i + 2 < argc)
            opt_name = i;               /* /name DIR New Name Words */
        if (stricmp(argv[i], "/diag") == 0)
            opt_diag = 1;
        if (stricmp(argv[i], "/debug") == 0)
            opt_debug = 1;
    }

    remove(RUNBAT);
    remove(OLDBAT);
    getcwd(launcher_dir, PATH_LEN);
    guard_breaks(1);

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
        theme_select(cfg_theme);
        wmode_network = wmode_net();
        net_cdmode = cfg_netcd || wmode_network;
        {   /* a card mounts the discs itself: it can have them untouched */
            const char *m = mount_mode();
            net_rawcd = m && m[0] && stricmp(m, "SOFTWARE") != 0;
        }
    }
    if (getenv("WAVESRV")) {            /* make run: the emulator's host */
        strncpy(cfg_server, getenv("WAVESRV"), sizeof(cfg_server) - 1);
        cfg_server[sizeof(cfg_server) - 1] = 0;
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
    ini_moved = ini_games_file(gamedir);    /* the game sections live next to the games */

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
        printf("Games  : %d under %s%s\n", game_count, gamedir,
               scan_sizes_missing ? " (the listing gave no size for some programs: a redirector drive)" : "");
        printf("INI    : %s; the games' sections in %s\n", ini_file_path(), ini_games_path());
        scan_diag();
        {
            unsigned big, tot;
            if (xms_free(&big, &tot)) {
                int umb = umb_largest();
                printf("XMS    : %uK free, largest block %uK\n", tot, big);
                r.x.ax = 0x5802;        /* the UMB link: DOS=UMB */
                int86(0x21, &r, &r);
                if (umb < 0)
                    printf("Upper  : the XMS driver offers no UMBs (JEMM386 RAM, or JEMMEX, does)\n");
                else
                    printf("Upper  : largest free block %dK, %s%s\n", umb,
                           r.h.al ? "linked (DOS=UMB)" : "NOT linked: DOS=UMB is missing",
                           umb < 40 ? "  <- little: I=B000-B7FF in the JEMM386 line, or BIOS Setup's shadowing" : "");
            } else
                printf("XMS    : no driver (HIMEM.SYS or JEMMEX)\n");
        }
        {
            unsigned used, size;
            env_space(&used, &size);
            if (!size)
                printf("Environ: could not find the shell's block\n");
            else
                printf("Environ: %u of %u bytes used in the shell's block, %u free%s\n",
                       used, size, size - used,
                       size - used < 96 ? "  <- too little, see README" : "");
        }
        {   /* the disk: this program read back, and its folder walked,
               each for about a second, so a slow drive shows a number */
            unsigned long __far *ticks = (unsigned long __far *)MK_FP(0x40, 0x6C);
            const char *sep = home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\";
            char path[PATH_LEN + 16];
            char __far *buf = (char __far *)_fmalloc(8192);
            unsigned long t0, ms, bytes = 0;
            unsigned n, entries = 0, passes = 0;
            int h;
            struct find_t ft;
            sprintf(path, "%s%sWAVE86.EXE", home_dir, sep);
            t0 = *ticks;
            while (buf && passes < 8 && *ticks - t0 < 18) {
                if (_dos_open(path, O_RDONLY, &h) != 0) break;
                while (_dos_read(h, buf, 8192, &n) == 0 && n) bytes += n;
                _dos_close(h);
                passes++;
            }
            ms = (*ticks - t0) * 55;
            if (passes)
                printf("Disk   : WAVE86.EXE read %u time%s, %luK in %lu ms: %lu KB/s\n", passes, passes == 1 ? "" : "s",
                       bytes / 1024, ms, ms ? bytes / 1024 * 1000 / ms : 9999);
            sprintf(path, "%s%s*.*", home_dir, sep);
            t0 = *ticks;
            for (passes = 0; passes < 8 && *ticks - t0 < 18; passes++)
                if (_dos_findfirst(path, _A_NORMAL | _A_RDONLY | _A_ARCH | _A_SUBDIR, &ft) == 0) {
                    entries = 0;
                    do entries++; while (_dos_findnext(&ft) == 0);
                }
            ms = (*ticks - t0) * 55;
            printf("         the folder's %u entries listed %u time%s in %lu ms\n", entries, passes, passes == 1 ? "" : "s", ms);
            if (buf) _ffree(buf);
        }
        mus_init();
        mus_diag();
        mus_shutdown();
        return 0;
    }

    if (opt_launch) {
        for (i = 0; i < game_count; i++)
            if (stricmp(games[i].dir, opt_launch) == 0) {
                if (games[i].flags & GF_NETPEND) {  /* not all here: the rest first */
                    if (!cfg_server[0]) {
                        printf("WAVE86: %s is not all here yet, and WAVE86.INI has no server=.\n", games[i].name);
                        return 1;
                    }
                    net_continue(&games[i]);
                    return 0;
                }
                if (!games[i].exe[0]) {
                    printf("WAVE86: %s has no program to run (E in the launcher sets one).\n", games[i].name);
                    return 1;
                }
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
        midi_log = (unsigned char __far *)_fmalloc(60000U);
        midi_log_cap = midi_log ? 60000U : 0;
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
        mus_shutdown();                 /* its note-offs go in the log too */
        printf("midi: mpu=%d port=%X clk=%lu bytes=%u\n",
               mpu_present, mpu_port, midi_clk, midi_log_len);
        if (mod_dumpf) fclose(mod_dumpf);
        mod_dumpf = NULL;
        if (midi_log_len) {             /* MIDILOG.BIN: every byte to the MPU */
            int h;
            unsigned n;
            if (_dos_creat("MIDILOG.BIN", _A_NORMAL, &h) == 0) {
                _dos_write(h, midi_log, midi_log_len, &n);
                _dos_close(h);
            }
        }
        return 0;
    }

    mus_init();
    if (opt_dumpsel) {
        i = find_game(opt_dumpsel);
        if (i >= 0) { sel = i; top = sel > 13 ? sel - 13 : 0; }
    }
    /* back from a download or a list fetch? */
    i = net_apply_pending();
    if (i >= 0) { sel = i; top = sel > 13 ? sel - 13 : 0; }
    if (opt_play) {                     /* boot straight into a game off the server */
        int j;
        net_load();
        for (j = 0; j < net_count; j++)
            if (stricmp(net_get(j)->dir, opt_play) == 0) { net_play(j); break; }
        if (j == net_count) {
            printf("WAVE86: %s is not in the server list (press L in the menu).\n", opt_play);
            return 1;
        }
    }
    if (wmode_network && cfg_server[0] && !net_view_pending() && !opt_dumpnet) {
        if (net_load() == 0)            /* no list yet: WAVEGET fetches it, and we come back here */
            net_fetch_list();
        view = 1;
    } else if (net_view_pending() || opt_dumpnet) {
        net_load();
        view = 1;
        if (opt_netsel) {
            int j;
            for (j = 0; j < net_count; j++)
                if (stricmp(net_get(j)->dir, opt_netsel) == 0) { nsel0 = j; break; }
        }
    }
    vid_text_mode();
    vid_set_palette();
    if (view) net_redraw(nsel0, nsel0 > 13 ? nsel0 - 13 : 0); else redraw(sel, top);
    if (view && wmode_network)
        ui_status("NETWORK MODE: ENTER PLAYS A GAME OFF THE SERVER, I INSTALLS IT.");
    if (ini_moved) {                    /* the sections went to GAMES.INI just now: say so once */
        char m[PATH_LEN + 40];
        sprintf(m, "THE GAMES' SETTINGS NOW LIVE IN %s", ini_games_path());
        ui_status(m);
    }
    if (i == -2) net_arrived_notice();
    {   /* the SETs a game batch makes have to fit somewhere */
        unsigned used, size;
        env_space(&used, &size);
        if (size && size - used < 96)
            ui_status("THE SHELL'S ENVIRONMENT IS ALMOST FULL: GAMES THAT NEED A DISC WILL NOT MOUNT IT");
    }

    if (opt_dump && !opt_keys) {        /* with /keys, getkey() dumps when they run out */
        scr_dump("SCREEN.BIN", "FONT.BIN", "PAL.BIN");
        quit();
    }

    for (;;) {
        unsigned k = getkey();
        int old = sel;

        if (view == 1) {                /* ---- the eXoDOS list ---- */
            static int nsel = 0, ntop = 0;
            int nold = nsel;
            if (k == '?' || k == K_F1) {
                k = menu_pick(1, nsel);
                net_redraw(nsel, ntop);
                if (!k) continue;
            }
            if (k == 'm' || k == 'M') k = K_MUSIC;
            switch (k) {
            case K_MUSIC: music_key(); continue;
            case K_F4: {
                const char *m = music_style();
                net_redraw(nsel, ntop);
                if (m) ui_status(m);
                continue;
            }
            case K_UP:   nsel--; break;
            case K_DOWN: nsel++; break;
            case K_PGUP: case K_LEFT:  turn_page(&nsel, &ntop, net_count, -1); nold = -1; break;
            case K_PGDN: case K_RIGHT: turn_page(&nsel, &ntop, net_count, 1); nold = -1; break;
            case K_HOME: nsel = 0; break;
            case K_END:  nsel = net_count - 1; break;
            case 's': case 'S': case '/': case K_F3:
                if (net_count) {
                    int hit = find_title(1, nsel, k == K_F3);
                    if (hit == -1) net_status();
                    if (hit < 0 || hit == nsel) continue;
                    nsel = hit;
                    if (nsel < ntop || nsel >= ntop + PAGE) ntop = nsel > 3 ? nsel - 3 : 0;
                    net_status();
                    nold = -1;
                    break;
                }
                continue;
            case 0x1B: case 'n': case 'N':
                view = 0;
                net_free();
                redraw(sel, top);
                continue;
            case 'l': case 'L':
                net_fetch_list();
                nsel = ntop = 0;
                net_redraw(nsel, ntop);
                continue;
            case 'c': case 'C':             /* CDs: on the server, or downloaded */
                net_cdmode = !net_cdmode;
                net_redraw(nsel, ntop);
                continue;
            case ' ':                       /* into the install queue, or out */
                if (net_count) {
                    const NetGame *g = net_get(nsel);
                    if (net_queue_toggle(g->dir, net_size(g)) < 0)
                        ui_status("THE QUEUE IS FULL.");
                    else
                        net_redraw(nsel, ntop);
                }
                continue;
            case 'u': case 'U':             /* WAVE86 itself, from the server */
                net_update();
                net_redraw(nsel, ntop);
                continue;
            case 'p': case 'P':             /* play it off the server */
                if (net_count && net_get(nsel)->netplay) {
                    net_play(nsel);
                    net_redraw(nsel, ntop);     /* bare mode: back here */
                } else if (net_count) {
                    ui_status("NOTHING PLAYS OFF THIS SERVER: IT HAS NO NETDRIVE. ENTER INSTALLS IT.");
                }
                continue;
            case 0x0D:                      /* network mode: play it; otherwise as I */
                if (wmode_network && net_count && net_get(nsel)->netplay) {
                    net_play(nsel);
                    net_redraw(nsel, ntop);
                    continue;
                }
            case 'i': case 'I':             /* install the queue, or this one when nothing is queued */
                if (net_count)
                    net_install(nsel, net_qcount > 0, &sel, &top);
                continue;
            case 'q': case 'Q':             /* the queue, in a box */
                if (net_qcount) {
                    if (queue_modal())
                        net_install(nsel, 1, &sel, &top);
                    else
                        net_redraw(nsel, ntop);
                } else if (net_count) {
                    ui_status("NOTHING IS QUEUED. SPACE PUTS THE GAME UNDER THE BAR IN THE QUEUE.");
                }
                continue;
            case '+': case '=': mus_volume(1); ui_music_volshow(); continue;
            case '-': case '_': mus_volume(-1); ui_music_volshow(); continue;
            case '.': case '>': mus_skip(1); ui_status(NULL); continue;
            case ',': case '<': mus_skip(-1); ui_status(NULL); continue;
            default:
                /* letter jump: the index knows where each initial starts;
                   the same letter again steps to the next such title */
                if (k >= 'a' && k <= 'z') k -= 32;
                if (k >= 'A' && k <= 'Z' && net_count) {
                    int first = net_letter_first((char)k);
                    if (first >= 0) {
                        char c = net_get(nsel)->title[0];
                        if (c >= 'a' && c <= 'z') c -= 32;
                        if ((unsigned)c == k && nsel + 1 < net_count) {
                            char n = net_get(nsel + 1)->title[0];
                            if (n >= 'a' && n <= 'z') n -= 32;
                            nsel = ((unsigned)n == k) ? nsel + 1 : first;
                        } else {
                            nsel = first;
                        }
                    }
                }
                break;
            }
            if (net_count == 0) continue;
            if (nsel < 0) nsel = 0;
            if (nsel >= net_count) nsel = net_count - 1;
            if (nsel < ntop) ntop = nsel;
            if (nsel >= ntop + 14) ntop = nsel - 13;
            if (nsel != nold) {
                ui_net_list(nsel, ntop);
                ui_net_details(nsel);
            }
            continue;
        }

        if (k == '?' || k == K_F1) {
            k = menu_pick(0, sel);
            redraw(sel, top);
            if (!k) continue;
        }
        if (k == 'm' || k == 'M') k = K_MUSIC;
        switch (k) {
        case K_MUSIC:
            music_key();
            break;
        case K_F4: {
            const char *m = music_style();
            redraw(sel, top);
            if (m) ui_status(m);
            continue;
        }
        case K_UP:   sel--; break;
        case K_DOWN: sel++; break;
        case K_PGUP: case K_LEFT:  turn_page(&sel, &top, game_count, -1); old = -1; break;
        case K_PGDN: case K_RIGHT: turn_page(&sel, &top, game_count, 1); old = -1; break;
        case K_HOME: sel = 0; break;
        case K_END:  sel = game_count - 1; break;
        case '/': case K_F3:            /* S is setup here; the network list has it for this too */
            if (game_count) {
                int hit = find_title(0, sel, k == K_F3);
                if (hit == -1) ui_status(NULL);
                if (hit >= 0 && hit != sel) {
                    sel = hit;
                    if (sel < top || sel >= top + PAGE) top = sel > 3 ? sel - 3 : 0;
                    old = -1;
                    ui_status(NULL);
                }
            }
            break;
        case 0x3C00:                    /* F2: rename in place */
            if (game_count) {
                edit_name(&sel, &top);
                old = -1;               /* force a list refresh */
            }
            break;
        case K_DEL:                     /* off the disk */
            if (game_count)
                uninstall(&sel, &top);
            continue;
        case 'o': case 'O':             /* the game's options */
            if (game_count)
                game_options(&sel, &top);
            continue;
        case 'e': case 'E':             /* the game's program, setup, name... */
            if (game_count)
                edit_game(&sel, &top);
            continue;
        case 'p': case 'P': case 'd': case 'D':   /* the details instead of the picture, and back */
            ui_show_details = !ui_show_details;
            ui_details(sel);
            continue;
        case 0x1B:
            quit();
        case 0x0D:
            if (game_count && (games[sel].flags & GF_NETPEND)) {
                int i;                  /* not all here yet: the rest, then the list again */
                net_continue(&games[sel]);
                scan_games();
                i = net_apply_pending();
                if (i >= 0) { sel = i; top = sel > 13 ? sel - 13 : 0; }
                redraw(sel, top);
                if (i == -2) net_arrived_notice();
            } else if (game_count && !games[sel].exe[0]) {
                ui_status("NO PROGRAM TO RUN IN THIS FOLDER: E SETS ONE.");
            } else if (game_count) {
                launch(&games[sel], 0);
                redraw(sel, top);       /* back from a bare-mode run */
            }
            break;
        case 's': case 'S':
            if (game_count && (games[sel].flags & GF_NETPEND)) {
                ui_status("NOT ALL OF IT IS HERE YET: ENTER FETCHES THE REST.");
                break;
            }
            if (game_count && games[sel].setup[0]) {
                launch(&games[sel], 1);
                redraw(sel, top);
            }
            break;
        case 'a': case 'A':             /* the card's sound cards */
            if (access(PMINIT, 0) == 0) {
                sound_card_menu();
                redraw(sel, top);
            }
            break;
        case 'r': case 'R':
            scan_games();
            sel = 0; top = 0;
            redraw(sel, top);
            break;
        case 'n': case 'N':             /* the eXoDOS list */
            if (!cfg_server[0] && !ask_server()) {
                redraw(sel, top);
                break;
            }
            view = 1;
            if (net_load() == 0 && cfg_server[0])
                net_fetch_list();
            net_redraw(0, 0);
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
