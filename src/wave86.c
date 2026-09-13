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
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
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
void ui_net_static(void);
void ui_net_list(int sel, int top);
void ui_net_details(int sel);
void ui_net_keybar(void);

#define K_UP    0x4800
#define K_DOWN  0x5000
#define K_PGUP  0x4900
#define K_PGDN  0x5100
#define K_HOME  0x4700
#define K_END   0x4F00

static char launcher_dir[PATH_LEN];     /* cwd at start: where to return */
char home_dir[PATH_LEN];                /* EXE directory: INI and MUSIC\ */
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

static unsigned getkey(void)
{
    unsigned k;
    while (!_bios_keybrd(_KEYBRD_READY)) {
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
static void put_cmd(FILE *f, const char *var, const char *cmd)
{
    char buf[96];
    int n;
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    n = strlen(buf);
    if (n >= 4 && stricmp(buf + n - 4, "$ISO") == 0)
        n -= 4;
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        n--;
    buf[n] = 0;
    if (n)
        fprintf(f, "set %s=%s\n", var, buf);
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
    v = ini_global("imgmount");
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
                fprintf(f, "set WAVECDCMD=PGUSINIT.EXE /cdload\n");
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
/* Only a disc that stays on the server needs the NetDrive lines; telling
   a local one where the server is just eats environment space, which is
   in short supply on a machine with the default 256 bytes. */
static int bat_uses_netdrive(const Game *g)
{
    char path[PATH_LEN + 24];
    char buf[160];
    FILE *f;
    int found = 0;

    sprintf(path, "%s\\%s\\IMGMOUNT.BAT", gamedir, g->dir);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (!found && fgets(buf, sizeof(buf), f))
        if (strstr(buf, "NETDRIVE") != NULL)
            found = 1;
    fclose(f);
    return found;
}

static void emit_cd(FILE *f, const Game *g, int after)
{
    char iso[PATH_LEN + 32];
    const char *t, *sep, *hd, *cdx;
    const char *dot = strrchr(g->exe, '.');

    if ((g->flags & GF_CDBAT) && dot && stricmp(dot + 1, "BAT") == 0) {
        /* the start batch mounts the disc through its IMGMOUNT.BAT; we tell
           it where the NetDrive server is (the machine of server=, port
           netdrive_port=) and take everything down afterwards */
        if (after) {
            fprintf(f, "call IMGMOUNT.BAT /U\n");
        } else {
            char host[32];
            char *colon;
            const char *v;
            if (bat_uses_netdrive(g)) {
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

static void write_bat(const Game *g, int use_setup)
{
    FILE *f = fopen("RUNGAME.BAT", "w");
    const char *prog = use_setup ? g->setup : g->exe;
    const char *dot = strrchr(prog, '.');
    int is_bat = dot && stricmp(dot + 1, "BAT") == 0;
    int dbg = debug_mode();

    if (!f)
        return;
    if (dbg) {
        fprintf(f, "@echo off\ncopy RUNGAME.BAT RUNLAST.BAT > NUL\n");
        fprintf(f, "echo [WAVE86] %s, debug=1. Ctrl-C stops here.\n", g->dir);
        fprintf(f, "@echo on\n");
    } else {
        fprintf(f, "@echo off\n");
    }
    fprintf(f, "%c:\n", gamedir[0]);
    fprintf(f, "cd %s\\%s\n", gamedir, g->dir);
    ini_emit_extras(f, g->dir, 0);     /* sound mode, env, pre */
    emit_cd(f, g, 0);
    if (dbg) {
        fprintf(f, "@echo off\necho [WAVE86] the environment the game will see:\n");
        fprintf(f, "set\npause\n@echo on\n");
    }
    if (dbg && (g->flags & GF_CDBAT)) {
        /* The game's own batch calls IMGMOUNT.BAT, which starts with echo
           off and gives its drivers /Q, so the mount goes past in silence.
           Run it here on its own first, look at what it produced, and take
           it down again before the game does the same thing for real. */
        fprintf(f, "@echo off\necho [WAVE86] mounting the disc on its own first:\n@echo on\n");
        fprintf(f, "call IMGMOUNT.BAT\n");
        fprintf(f, "@echo off\necho [WAVE86] IMGMOUNT.BAT is done and says CD=%%CD%%\n");
        fprintf(f, "if \"%%CD%%\"==\"\" echo [WAVE86] it set no letter at all\n");
        fprintf(f, "dir %%CD%%:\\\n");
        fprintf(f, "pause\n");
        fprintf(f, "call IMGMOUNT.BAT /U\n");
        fprintf(f, "echo [WAVE86] taken down again; now the game mounts it itself\n");
        fprintf(f, "pause\n@echo on\n");
    }
    fprintf(f, "%s%s", is_bat ? "call " : "", prog);
    if (!use_setup && g->args[0])
        fprintf(f, " %s", g->args);
    fprintf(f, "\n");
    if (dbg) {                         /* the disc as it stands, before it goes */
        fprintf(f, "@echo off\npause\n");
        fprintf(f, "echo [WAVE86] the game has finished. CD=%%CD%%\n");
        fprintf(f, "dir %%CD%%:\\\n");
        fprintf(f, "pause\n@echo on\n");
    }
    emit_cd(f, g, 1);
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

static void net_redraw(int sel, int top)
{
    char msg[60];
    ui_net_static();
    ui_net_list(sel, top);
    ui_net_details(sel);
    ui_net_keybar();
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

/* run a command line (WAVEGET) through the same batch loop as a game */
static void run_command(const char *cmd, const char *what)
{
    FILE *f = fopen("RUNGAME.BAT", "w");
    if (!f)
        return;
    fprintf(f, "@echo off\n%s\n%c:\ncd %s\n", cmd, launcher_dir[0], launcher_dir);
    fclose(f);
    hand_off(what);
}

static void launch(const Game *g, int use_setup)
{
    char msg[80];
    write_bat(g, use_setup);
    if (use_setup)
        sprintf(msg, "WAVE86: Running %s for %s ...", g->setup, g->name);
    else
        sprintf(msg, "WAVE86: Running %s ...", g->name);
    hand_off(msg);
}

/* msg is what the user reads while the batch loop takes over */
static void hand_off(const char *msg)
{
    if (getenv("WAVE86")) {
        mus_shutdown();
        text_mode_plain();
        printf("%s\n", msg);
        exit(0);
    }

    mus_release();
    text_mode_plain();
    printf("%s\n", msg);
    printf("(type WAVE instead to give games all memory)\n");
    system("RUNGAME.BAT");
    remove("RUNGAME.BAT");

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
static void net_get_cmd(char *cmd, unsigned size, const NetGame *g, const char *exe)
{
    char key[32];
    if (stricmp(g->src, "exodos") == 0) strcpy(key, g->dir);
    else sprintf(key, "%s:%s", g->src, g->dir);
    if (g->cd && g->netcd)
        strcat(key, g->netcd && net_cdmode ? "?cd=net" : "?cd=local");
    sprintf(cmd, "%s GET %s %s %s %lu %s", exe, cfg_server, g->dir, gamedir, net_size(g), key);
    {
        const char *cdrom = cd_root();
        if (cdrom && cdrom[0] && strlen(cmd) + strlen(cdrom) + 2 < size) {
            strcat(cmd, " ");
            strcat(cmd, cdrom);
        }
    }
}

/*
 * Download the selected game, or the whole queue one after another, and
 * come back with the first of them selected in the games view. DOS runs
 * one program at a time and the launcher hands WAVEGET the machine while
 * it fetches, so nothing can come in behind your back; a queue is the
 * next best thing - mark a few games, walk away, find them installed.
 */
static void net_download(int nsel)
{
    char cmd[PATH_LEN * 2 + 96], exe[PATH_LEN + 16], msg[80];
    int n = net_qcount, k;
    FILE *f;

    waveget_path(exe);
    net_pending_reset();
    f = fopen("RUNGAME.BAT", "w");
    if (!f)
        return;
    fprintf(f, "@echo off\n");
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
    f = fopen("RUNGAME.BAT", "w");
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
        ui_status("NO ADLIB OR SOUND BLASTER FOUND (adlib=1 IN WAVE86.INI FORCES IT).");
        return;
    }
    if (!mus_ntracks) {
        sprintf(msg, "NO TRACKS IN %s%sMUSIC", home_dir,
                home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\");
        ui_status(msg);
        return;
    }
    mus_toggle();
    ui_status(NULL);
}

/* a download landed, but the scan found nothing to run in the folder */
static void net_arrived_notice(void)
{
    char msg[80];
    sprintf(msg, "%s ARRIVED WITH NOTHING TO RUN. THE SERVER HAD ONLY PART OF IT.",
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

    remove("RUNGAME.BAT");
    getcwd(launcher_dir, PATH_LEN);

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
        net_cdmode = cfg_netcd;
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
        printf("Games  : %d under %s\n", game_count, gamedir);
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
        mus_init();
        mus_diag();
        mus_shutdown();
        return 0;
    }

    if (opt_launch) {
        for (i = 0; i < game_count; i++)
            if (stricmp(games[i].dir, opt_launch) == 0) {
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
        mus_shutdown();
        if (mod_dumpf) fclose(mod_dumpf);
        mod_dumpf = NULL;
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
    if (net_view_pending() || opt_dumpnet) {
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
    if (i == -2) net_arrived_notice();
    {   /* the SETs a game batch makes have to fit somewhere */
        unsigned used, size;
        env_space(&used, &size);
        if (size && size - used < 96)
            ui_status("THE SHELL'S ENVIRONMENT IS ALMOST FULL: GAMES THAT NEED A DISC WILL NOT MOUNT IT");
    }

    if (opt_dump) {
        scr_dump("SCREEN.BIN", "FONT.BIN", "PAL.BIN");
        quit();
    }

    for (;;) {
        unsigned k = getkey();
        int old = sel;

        if (view == 1) {                /* ---- the eXoDOS list ---- */
            static int nsel = 0, ntop = 0;
            int nold = nsel;
            switch (k) {
            case K_UP:   nsel--; break;
            case K_DOWN: nsel++; break;
            case K_PGUP: nsel -= 14; break;
            case K_PGDN: nsel += 14; break;
            case K_HOME: nsel = 0; break;
            case K_END:  nsel = net_count - 1; break;
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
            case 0x0D:                      /* play it off the server */
                if (net_count && net_get(nsel)->netplay) {
                    net_play(nsel);
                    net_redraw(nsel, ntop);     /* bare mode: back here */
                }
                continue;
            case 'i': case 'I':             /* install: the queue, or this one */
                if (net_count) {
                    net_download(nsel);
                    /* bare mode only: back here with the game on disk */
                    scan_games();
                    i = net_apply_pending();
                    view = 0;
                    net_free();
                    if (i >= 0) { sel = i; top = sel > 13 ? sel - 13 : 0; }
                    redraw(sel, top);
                    if (i == -2) net_arrived_notice();
                }
                continue;
            case 'm': case 'M': music_key(); continue;
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

        switch (k) {
        case K_UP:   sel--; break;
        case K_DOWN: sel++; break;
        case K_PGUP: sel -= 14; break;
        case K_PGDN: sel += 14; break;
        case K_HOME: sel = 0; break;
        case K_END:  sel = game_count - 1; break;
        case 0x3C00:                    /* F2: rename in place */
            if (game_count) {
                edit_name(&sel, &top);
                old = -1;               /* force a list refresh */
            }
            break;
        case 0x1B:
            quit();
        case 0x0D:
            if (game_count) {
                launch(&games[sel], 0);
                redraw(sel, top);       /* back from a bare-mode run */
            }
            break;
        case 's': case 'S':
            if (game_count && games[sel].setup[0]) {
                launch(&games[sel], 1);
                redraw(sel, top);
            }
            break;
        case 'r': case 'R':
            scan_games();
            sel = 0; top = 0;
            redraw(sel, top);
            break;
        case 'n': case 'N':             /* the eXoDOS list */
            view = 1;
            if (net_load() == 0 && cfg_server[0])
                net_fetch_list();
            net_redraw(0, 0);
            break;
        case 'm': case 'M':
            music_key();
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
