/*
 * WAVE86 - synthwave game launcher for MS-DOS
 * Target: 8086 real mode, small model (Open Watcom)
 */
#ifndef WAVE86_H
#define WAVE86_H

#define VERSION_STR "0.1"

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
    unsigned char flags;
} Game;

#define GF_DOS4GW  0x01     /* needs 386+ (DOS/4GW extender present) */
#define GF_HIDE    0x02     /* hidden via ini */

/* --- video (vga.c) --- */
extern int vid_is_vga;      /* 1 if VGA/MCGA detected */
extern int vid_is_color;    /* 0 = MDA/mono text segment */
extern int opt_nopal;       /* skip DAC reprogramming */

void vid_detect(void);
void vid_set_palette(void);         /* synthwave DAC colors (VGA only) */
void vid_text_mode(void);           /* mode 3 (or 7 on mono) + clear */
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
void mus_diag(void);            /* /diag report */

/* --- mod.c: Sound Blaster + ProTracker --- */
extern int sb_present;
extern int sb_dsp_major, sb_dsp_minor;
extern unsigned sb_rate;
extern int mod_playing, mod_done, mod_order, mod_row;
extern int mod_vu;              /* peak of the last mixed buffer */
int cpu_level(void);            /* 0 = 8086, 2 = 286, 3 = 386+ */
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
