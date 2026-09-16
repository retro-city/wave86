/*
 * WAVE86 - synthwave game launcher for MS-DOS
 * Target: 8086 real mode, small model (Open Watcom)
 */
#ifndef WAVE86_H
#define WAVE86_H

#include <stdio.h>

#define VERSION_STR "0.4"

#define MAX_GAMES   128
#define NAME_LEN    40
#define FN_LEN      13      /* 8.3 + NUL */
#define PATH_LEN    80

typedef struct {
    char dir[FN_LEN];       /* subdirectory name under gamedir */
    char name[NAME_LEN];    /* display name */
    char exe[FN_LEN];       /* main executable (or .BAT) */
    char setup[FN_LEN];     /* setup/config program, "" if none */
    char args[32];          /* extra command line args */
    char sound[8];          /* sound= mode name (PicoGUS etc.) */
    char cdimg[20];         /* CD image mounted while it runs: CD\X.ISO, or cd= */
    unsigned char flags;
} Game;

#define GF_DOS4GW  0x01     /* needs 386+ (DOS/4GW extender present) */
#define GF_HIDE    0x02     /* hidden via ini */
#define GF_INI     0x04     /* has a [section] in WAVE86.INI */
#define GF_EXODOS  0x08     /* source=exodos: came from the eXoDOS server */
#define GF_TDC     0x10     /* source=tdc: from the Total DOS Collection */
#define GF_CDBAT   0x20     /* has IMGMOUNT.BAT: its start batch mounts the CD itself */
#define GF_NETCD   0x40     /* ... and that batch fetches the disc from the server */

/* --- the looks (theme.c) --- */
typedef struct {
    const char *name;
    const char *logo[5];        /* '#' = block, 5 rows */
    unsigned char logo_clr[5];  /* colour per row */
    unsigned char shadow;       /* drop shadow colour, 0 = none */
    const char *tagline;
    unsigned char band[6];      /* divider gradient */
    unsigned char pal[16][3];   /* VGA DAC, 6-bit */
} Theme;
extern const Theme *theme;
void theme_select(const char *name);
extern char cfg_theme[16];      /* ini theme= */

/* --- video (vga.c) --- */
extern int vid_is_vga;      /* 1 if VGA/MCGA detected */
extern int vid_is_color;    /* 0 = MDA/mono text segment */
extern int opt_nopal;       /* skip DAC reprogramming */

void vid_detect(void);
void vid_set_palette(void);         /* synthwave DAC colors (VGA only) */
void vid_text_mode(void);           /* mode 3 (or 7 on mono) + clear */
void vid_font512(void);            /* VGA: two font banks via attr bit 3 */
void vid_load_glyphs(int block, unsigned char first, unsigned count,
                     const unsigned char *bits);
void scr_put(int x, int y, unsigned char ch, unsigned char attr);
void scr_puts(int x, int y, const char *s, unsigned char attr);
void scr_fill(int x, int y, int w, int h, unsigned char ch, unsigned char attr);
void scr_hline(int x, int y, int w, unsigned char ch, unsigned char attr);
unsigned scr_dump(const char *scrfile, const char *fontfile,
                  const char *palfile);

/* --- scanning (scan.c) --- */
extern Game games[MAX_GAMES];
extern int game_count;
extern char gamedir[PATH_LEN];      /* absolute path to games root */
extern char home_dir[PATH_LEN];     /* where WAVE86.EXE lives: INI, MUSIC\ */

int scan_games(void);

/* --- config (ini.c) --- */
void ini_load(const char *fname);   /* reads gamedir= */
void ini_apply(void);               /* per-game [sections] onto games[] */
int ini_write_name(const char *dir, const char *name); /* set/add name= */
int ini_write_key(const char *dir, const char *key, const char *value);
void ini_emit_extras(FILE *bat, const char *dir, int after); /* env/pre/post/sound */
const char *ini_global(const char *key);   /* value of a global key, or NULL */
void sort_games(void);
int find_game(const char *dir);

/* --- music (music.c) --- */
extern int mus_present;         /* AdLib or Sound Blaster usable */
extern int mus_on;              /* playing (M toggles) */
extern int mus_vol;             /* 0..10 */
extern int mus_ntracks;
extern int mus_kind;            /* 0 = IMF on AdLib, 1 = MOD on SB */
extern char mus_track[9];       /* current track base name */
extern int cfg_modrate;         /* ini modrate=: -1 auto, 0 off, Hz */
extern int cfg_adlib;           /* ini adlib=: -1 auto, 0 off, 1 force */
extern int cfg_music;           /* ini music=: 1 = autoplay at startup */
extern int cfg_netcd;           /* ini netcd=: 1 = NET CD instead of LOCAL CD */

/* --- net.c: the eXoDOS list and downloads (WAVEGET.EXE does the TCP) --- */
typedef struct {
    char dir[9];
    char title[33];
    char exe[13];
    unsigned year;
    unsigned long kb;
    char cd;                    /* 1 = the game wants its CD */
    char partial;               /* 1 = the server has only part of it yet */
    char netcd;                 /* 1 = the server can also keep the CD (NetDrive) */
    char netplay;               /* 1 = can be played off the server (NetDrive) */
    unsigned long cdkb;         /* the CD image's share of kb */
    unsigned long rawkb;        /* the discs as they came (cue/bin, audio kept) */
    char src[8];                /* "exodos" or "tdc" */
} NetGame;
extern int net_count;
extern char cfg_server[32];     /* ini server=a.b.c.d:port */
extern char net_pending_dir[9]; /* folder of the last download, after net_apply_pending */
extern int net_cdmode;          /* 1 = leave CDs on the server (NET CD), 0 = download them */
extern int net_rawcd;           /* 1 = a card mounts the discs: fetch cue/bin untouched */
unsigned long net_size(const NetGame *g);   /* the download in the current mode */
int net_load(void);             /* offsets into NETLIST.TXT; count */
const NetGame *net_get(int i);  /* reads that line; valid until the next call */
int net_letter_first(char c);   /* first title starting with c, or -1 */
void net_free(void);
void net_mark_pending(const NetGame *g);    /* appends to NETGAME.TXT */
void net_pending_reset(void);   /* forget what was on its way in */
extern int net_qcount;          /* games in the install queue */
int net_queued(const char *dir);               /* is that folder queued? */
int net_queue_toggle(const char *dir, unsigned long kb);  /* 1 in, 0 out, -1 full */
int net_queue_add(const char *dir, unsigned long kb);
void net_queue_clear(void);
unsigned long net_queue_kb(void);
void net_mark_view(void);
int net_view_pending(void);
int net_apply_pending(void);    /* index, -1 nothing arrived, -2 arrived but nothing runs */
void mus_diag(void);            /* /diag report */

/* --- mod.c: Sound Blaster + ProTracker --- */
extern int sb_present;
extern int sb_dsp_major, sb_dsp_minor;
extern unsigned sb_rate;
extern int mod_playing, mod_done, mod_order, mod_row;
extern int mod_vu;              /* peak of the last mixed buffer */
/* --- cpu.c --- */
extern char cpu_desc[32];       /* "AMD K6-2 400MHZ 64MB" */
extern int cpu_lvl;
int cpu_level(void);            /* 0 = 8086, 2 = 286, 3 = 386+ */
void cpu_identify(void);        /* fills cpu_desc; takes ~0.25 s */
int sb_init(void);
void sb_shutdown(void);
void sb_release(void);          /* free DMA buffer + tables */
int mod_load(const char *path);
void mod_free(void);
void mod_start(void);
void mod_stop(void);
void mod_poll(void);
unsigned mod_irqs(void);

void mus_init(void);            /* detect, hook timer, start playlist */
void mus_shutdown(void);        /* restore timer + silence (atexit-safe) */
void mus_release(void);         /* shutdown + free every far buffer */
void mus_poll(void);            /* call between keys: playlist advance */
void mus_toggle(void);
void mus_volume(int delta);
int mus_vu(void);               /* live level, 0..10 bar cells */
void mus_skip(int dir);         /* +1 next / -1 previous track */

/* --- misc --- */
unsigned dos_free_kb(void);

#endif
