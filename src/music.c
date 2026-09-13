/*
 * music.c - AdLib (OPL2, port 388h) background music.
 *
 * Plays id Software IMF music (reg, val, delay16 records) from a hooked
 * timer interrupt: the PIT is reprogrammed to the id-standard 560 Hz
 * (700 Hz for Wolf3D .WLF files) and the BIOS handler is chained at its
 * usual 18.2 Hz via a 16-bit accumulator. Both IMF variants play:
 * type-0 (raw records, Keen style) and type-1 (16-bit length header).
 *
 * Volume scales only carrier operator levels (modulators keep the
 * timbre), using shadow copies of the stream's 40h/C0h writes so the
 * level can be re-applied when the user changes it.
 */
#include <dos.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <i86.h>
#include "wave86.h"

#define OPL_INDEX  0x388
#define OPL_DATA   0x389
#define DIV_560HZ  2131         /* 1193182 / 560: id IMF standard rate */
#define DIV_700HZ  1704         /* 1193182 / 700: Wolf3D .WLF rate */
#define SEG_BYTES  60000U       /* per far-memory segment (multiple of 4) */
#define MAX_SEGS   4            /* up to 240K per tune */
#define MAX_TRACKS 16

int mus_present = 0;
int mus_on = 0;
int mus_vol = 8;                /* 0..10 */
int mus_ntracks = 0;
int mus_kind = 0;               /* 0 = IMF on AdLib, 1 = MOD on Sound Blaster */
char mus_track[9] = "";

static int opl_present = 0;
static int sb_usable = 0;
static char tracks[MAX_TRACKS][FN_LEN];
static int cur_track = 0;
static volatile int track_done = 0;

static unsigned char __far *segs[MAX_SEGS];
static unsigned seglen[MAX_SEGS];
static int nsegs = 0;               /* segments used by current track */
volatile int mus_cseg = 0;          /* play position (visible for tests) */
volatile unsigned mus_coff = 0;
#define cseg mus_cseg
#define coff mus_coff
static volatile unsigned mdelay = 0;
static volatile int playing = 0;

static void (__interrupt __far *old_int8)(void);
static int hooked = 0;
static unsigned acc = 0;
static unsigned pit_div = DIV_560HZ;
int mus_loops = 0;              /* completed track traversals (tests) */

/* shadows of stream state, for volume scaling and pausing */
static unsigned char sh_tl[0x16];
static unsigned char sh_b0[9];
static unsigned char sh_c0[9];
static unsigned char sh_bd = 0;

/* operator cell -> channel, and whether the op is the channel's op2 */
static const signed char op_ch[0x16] = {
    0, 1, 2, 0, 1, 2, -1, -1,
    3, 4, 5, 3, 4, 5, -1, -1,
    6, 7, 8, 6, 7, 8
};
static const signed char op_2nd[0x16] = {
    0, 0, 0, 1, 1, 1, -1, -1,
    0, 0, 0, 1, 1, 1, -1, -1,
    0, 0, 0, 1, 1, 1
};

/*
 * A YM3812 needs 3.3 us to itself after an index write and 23 us after a
 * data write, and the way everyone waits is to read its status port,
 * each read being one ISA bus cycle. Six and thirty-five reads is right
 * for a real chip on a real bus, about a microsecond each. A card that
 * emulates the OPL in firmware answers far more slowly - the bus waits
 * for it - so those same reads can take ten times as long and the
 * soundtrack starts costing a third of the machine, which a download
 * notices. opl_calibrate() times one read and keeps the delays to what
 * the chip is actually owed; it can only shorten them, never stretch.
 */
static int opl_d1 = 6, opl_d2 = 35;

static void opl_out(unsigned char reg, unsigned char val)
{
    int i;
    outp(OPL_INDEX, reg);
    for (i = 0; i < opl_d1; i++) inp(OPL_INDEX);
    outp(OPL_DATA, val);
    for (i = 0; i < opl_d2; i++) inp(OPL_INDEX);
}

/* tenths of a microsecond per status read, as measured */
static unsigned opl_read_ns = 0;

static void opl_calibrate(void)
{
    unsigned long __far *ticks = (unsigned long __far *)MK_FP(0x40, 0x6C);
    unsigned long t0, per10;
    int i;

    t0 = *ticks;
    while (*ticks == t0)                /* line up with a tick */
        t0 = *ticks - 0;
    t0 = *ticks;
    for (i = 0; i < 1000; i++)
        inp(OPL_INDEX);
    /* tenths of a microsecond for one read: ticks are 54925 us */
    per10 = (*ticks - t0) * 549250UL / 1000;
    if (!per10)
        per10 = 5;                      /* faster than we can measure */
    opl_read_ns = (unsigned)per10;
    opl_d1 = (int)((33 + per10 - 1) / per10);        /* 3.3 us */
    opl_d2 = (int)((230 + per10 - 1) / per10);       /* 23 us */
    if (opl_d1 < 1) opl_d1 = 1;
    if (opl_d2 < 1) opl_d2 = 1;
    if (opl_d1 > 6) opl_d1 = 6;
    if (opl_d2 > 35) opl_d2 = 35;
}

static void opl_reset(void)
{
    unsigned r;
    for (r = 0xB0; r <= 0xB8; r++)
        opl_out((unsigned char)r, 0);       /* key off first */
    opl_out(0xBD, 0);
    for (r = 0x20; r <= 0xF5; r++)
        opl_out((unsigned char)r, 0);
    opl_out(0x01, 0x20);                    /* enable waveform select */
    memset(sh_tl, 0, sizeof(sh_tl));
    memset(sh_b0, 0, sizeof(sh_b0));
    memset(sh_c0, 0, sizeof(sh_c0));
}

static unsigned char opl_diag_a = 0xFF, opl_diag_b = 0xFF;
static int opl_diag_result = -1;

/*
 * Classic AdLib timer test, but patient: a real YM3812 flags timer 1
 * after 80 us, while firmware emulators (PicoGUS and friends) can take
 * milliseconds, so the status port is polled for up to ~30 ms.
 */
static int opl_detect(void)
{
    unsigned char a, b = 0;
    unsigned i;

    opl_out(0x04, 0x60);        /* reset both timers */
    opl_out(0x04, 0x80);        /* reset IRQ */
    a = (unsigned char)(inp(OPL_INDEX) & 0xE0);
    opl_out(0x02, 0xFF);        /* timer 1: shortest period */
    opl_out(0x04, 0x21);        /* start timer 1 */
    for (i = 0; i < 30000; i++) {
        b = (unsigned char)(inp(OPL_INDEX) & 0xE0);
        if (b & 0x40)           /* timer 1 expired */
            break;
    }
    opl_out(0x04, 0x60);
    opl_out(0x04, 0x80);
    opl_diag_a = a;
    opl_diag_b = b;
    opl_diag_result = (a == 0x00 && (b & 0x40)) ? 1 : 0;
    return opl_diag_result;
}

static unsigned char scaled_tl(int o)
{
    unsigned char raw = sh_tl[o];
    unsigned tl = raw & 0x3F;
    unsigned att;
    /* carriers get scaled; in rhythm mode all ch6-8 ops are voices */
    int carrier = op_2nd[o] || (sh_c0[(int)op_ch[o]] & 1) ||
                  (o >= 0x10 && (sh_bd & 0x20));

    if (!carrier)
        return raw;
    att = (mus_vol == 0) ? 63 : (unsigned)(10 - mus_vol) * 5;
    tl += att;
    if (tl > 63) tl = 63;
    return (unsigned char)((raw & 0xC0) | tl);
}

/* apply one stream event, keeping shadows current */
static void stream_write(unsigned char reg, unsigned char val)
{
    if (reg >= 0x40 && reg <= 0x55) {
        int o = reg - 0x40;
        if (op_ch[o] >= 0) {
            sh_tl[o] = val;
            val = scaled_tl(o);
        }
    } else if (reg >= 0xB0 && reg <= 0xB8) {
        sh_b0[reg - 0xB0] = val;
    } else if (reg >= 0xC0 && reg <= 0xC8) {
        sh_c0[reg - 0xC0] = val;
    } else if (reg == 0xBD) {
        sh_bd = val;
    }
    opl_out(reg, val);
}

static void __interrupt __far timer_isr(void)
{
    if (playing) {
        while (mdelay == 0) {
            unsigned char __far *rec;
            if (cseg >= nsegs || coff >= seglen[cseg]) {
                playing = 0;
                track_done = 1;
                break;
            }
            rec = segs[cseg] + coff;
            stream_write(rec[0], rec[1]);
            mdelay = (unsigned)rec[2] | ((unsigned)rec[3] << 8);
            coff += 4;
            if (coff >= seglen[cseg]) {
                cseg++;
                coff = 0;
            }
        }
        if (mdelay)
            mdelay--;
    }
    acc += pit_div;
    if (acc < pit_div)          /* wrapped: 18.2 Hz boundary */
        _chain_intr(old_int8);
    outp(0x20, 0x20);
}

static void pit_set(unsigned divisor)
{
    _disable();
    outp(0x43, 0x36);
    outp(0x40, divisor & 0xFF);
    outp(0x40, (divisor >> 8) & 0xFF);
    _enable();
}

static void notes_off(void)
{
    int ch;
    _disable();
    for (ch = 0; ch < 9; ch++)
        opl_out((unsigned char)(0xB0 + ch),
                (unsigned char)(sh_b0[ch] & ~0x20));
    opl_out(0xBD, 0);
    _enable();
}

static int loaded = 0;              /* a track is resident in memory */

/* hook INT 8 and run the PIT at the track's rate; done lazily so a
   silent launcher leaves the timer alone */
static void ensure_hooked(void)
{
    if (!hooked) {
        old_int8 = _dos_getvect(8);
        _dos_setvect(8, timer_isr);
        hooked = 1;
    }
    pit_set(pit_div);
}

static int imf_load(int idx)
{
    char path[PATH_LEN];
    FILE *f;
    static unsigned char buf[1024];
    unsigned n;
    int si = 0;
    unsigned got = 0;
    unsigned long limit, total = 0;
    long fsize;

    playing = 0;
    track_done = 0;
    notes_off();

    sprintf(path, "MUSIC\\%s", tracks[idx]);
    f = fopen(path, "rb");
    if (!f)
        return 1;

    /* type-1 IMF starts with a 16-bit data length; type-0 is raw */
    fseek(f, 0L, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0L, SEEK_SET);
    limit = 0xFFFFFFFFUL;
    if (fread(buf, 1, 2, f) == 2) {
        unsigned w = (unsigned)buf[0] | ((unsigned)buf[1] << 8);
        if (w >= 4 && (w & 3) == 0 && (long)w <= fsize - 2)
            limit = w;                  /* type-1: skip header */
        else
            fseek(f, 0L, SEEK_SET);     /* type-0: rewind */
    }

    while (si < MAX_SEGS && total < limit) {
        unsigned want = SEG_BYTES;
        if (limit - total < (unsigned long)want)
            want = (unsigned)(limit - total);
        if (!segs[si])
            segs[si] = (unsigned char __far *)_fmalloc(SEG_BYTES);
        if (!segs[si])
            break;                      /* keep what we have */
        got = 0;
        while (got < want &&
               (n = fread(buf, 1,
                          got + sizeof(buf) <= want ?
                          sizeof(buf) : want - got, f)) > 0) {
            unsigned k;
            for (k = 0; k < n; k++)
                segs[si][got + k] = buf[k];
            got += n;
        }
        seglen[si] = got & ~3U;         /* whole records only */
        total += got;
        si++;
        if (got < want)
            break;                      /* end of file */
    }
    fclose(f);

    /* .WLF plays at Wolf3D's 700 Hz, plain .IMF at the standard 560 */
    {
        const char *dot = strrchr(tracks[idx], '.');
        pit_div = (dot && stricmp(dot + 1, "WLF") == 0) ? DIV_700HZ
                                                        : DIV_560HZ;
        ensure_hooked();            /* first IMF track hooks the timer */
    }

    {
        char base[9], *dot;
        strncpy(base, tracks[idx], 8);
        base[8] = 0;
        dot = strchr(base, '.');
        if (dot) *dot = 0;
        strcpy(mus_track, base);
    }

    _disable();
    nsegs = si;
    cseg = 0;
    coff = 0;
    mdelay = 0;
    playing = mus_on;
    _enable();
    return 0;
}

static int cmp_names(const void *a, const void *b)
{
    return stricmp((const char *)a, (const char *)b);
}

static int track_is_mod(int idx)
{
    const char *dot = strrchr(tracks[idx], '.');
    return dot && stricmp(dot + 1, "MOD") == 0;
}

/* MUSIC\ lives next to the EXE, wherever the user started us from */
static void music_path(char *dst, const char *name)
{
    sprintf(dst, "%s%sMUSIC\\%s", home_dir,
            home_dir[strlen(home_dir) - 1] == '\\' ? "" : "\\", name);
}

/*
 * Load and start track idx on whichever engine its type needs. A track
 * that fails to load (bad file, out of memory) is skipped, so a broken
 * file cannot wedge the playlist.
 */
static int mus_load(int idx)
{
    int tries;
    for (tries = 0; tries < mus_ntracks; tries++) {
        int i = (idx + tries) % mus_ntracks;
        int rc;

        playing = 0;                    /* stop both engines */
        if (opl_present)
            notes_off();
        mod_stop();
        mod_free();

        if (track_is_mod(i)) {
            char path[PATH_LEN + 24];
            music_path(path, tracks[i]);
            rc = sb_usable ? mod_load(path) : 1;
            if (rc == 0) {
                mus_kind = 1;
                mod_start();
            }
        } else {
            rc = opl_present ? imf_load(i) : 1;
            if (rc == 0)
                mus_kind = 0;
        }
        if (rc == 0) {
            char base[9], *dot;
            strncpy(base, tracks[i], 8);
            base[8] = 0;
            dot = strchr(base, '.');
            if (dot) *dot = 0;
            strcpy(mus_track, base);
            cur_track = i;
            loaded = 1;
            return 0;
        }
    }
    mus_track[0] = 0;
    loaded = 0;
    return 1;
}

static void scan_pattern(const char *pattern)
{
    struct find_t ft;
    unsigned rc = _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_ARCH,
                                 &ft);
    while (rc == 0 && mus_ntracks < MAX_TRACKS) {
        strncpy(tracks[mus_ntracks], ft.name, FN_LEN - 1);
        tracks[mus_ntracks][FN_LEN - 1] = 0;
        mus_ntracks++;
        rc = _dos_findnext(&ft);
    }
}

static void scan_tracks(void)
{
    char pattern[PATH_LEN + 24];
    mus_ntracks = 0;
    if (opl_present) {
        music_path(pattern, "*.IMF");
        scan_pattern(pattern);
        music_path(pattern, "*.WLF");
        scan_pattern(pattern);
    }
    if (sb_usable) {
        music_path(pattern, "*.MOD");
        scan_pattern(pattern);
    }
    qsort(tracks, mus_ntracks, FN_LEN, cmp_names);
}

void mus_init(void)
{
    sb_usable = sb_init();
    if (cfg_adlib == 0)
        opl_present = 0;
    else if (cfg_adlib == 1)
        opl_present = 1;                /* forced by WAVE86.INI */
    else {
        opl_present = opl_detect();
        /* every Sound Blaster carries an OPL; trust the DSP if the
           timer test was inconclusive */
        if (!opl_present && sb_present)
            opl_present = 1;
    }
    if (opl_present) {
        opl_calibrate();                /* how slow is this card's bus? */
        opl_reset();
    }
    mus_present = opl_present || sb_usable;
    if (!mus_present)
        return;
    scan_tracks();
    if (!mus_ntracks)
        return;

    {
        static int registered = 0;
        if (!registered) {
            atexit(mus_shutdown);
            registered = 1;
            mus_on = cfg_music;     /* off unless music=1; M turns it on;
                                       later re-inits keep the M state */
        }
    }
    /* silent start: nothing loaded, no timer hook, no DMA until M */
    if (mus_on)
        mus_load(cur_track < mus_ntracks ? cur_track : 0);
}

/* /diag: print what the sound detection saw */
void mus_diag(void)
{
    const char *bl = getenv("BLASTER");
    if (opl_diag_result < 0)
        opl_detect();
    printf("AdLib  : status after reset %02Xh, after timer %02Xh -> %s",
           opl_diag_a, opl_diag_b, opl_diag_result ? "found" : "no reply");
    if (cfg_adlib >= 0)
        printf(" (ini adlib=%d)", cfg_adlib);
    printf("\n");
    printf("BLASTER: %s\n", bl ? bl : "(not set)");
    printf("SB DSP : %s", sb_present ? "answered reset" : "no reply");
    if (sb_present)
        printf(", version %d.%02d, MOD mixer %s at %u Hz",
               sb_dsp_major, sb_dsp_minor,
               sb_rate ? "on" : "off", sb_rate);
    printf("\n");
    printf("CPU    : %s\n", cpu_desc);
    /*
     * How long this machine takes over one OPL register write. It is not
     * the CPU that decides: every write is an ISA bus cycle plus the delay
     * loop the chip needs, and a card that emulates the OPL in firmware
     * can be slower again. The IMF player writes a few of these on every
     * timer tick at 560 Hz, so the number below is what the soundtrack
     * costs a download - netmusic=0 in the INI turns it off for WAVEGET.
     */
    if (opl_present) {
        unsigned long __far *ticks = (unsigned long __far *)MK_FP(0x40, 0x6C);
        unsigned long t0, us;
        int i;
        t0 = *ticks;
        while (*ticks == t0)            /* line up with a tick */
            t0 = *ticks - 0;
        t0 = *ticks;
        for (i = 0; i < 2000; i++)
            opl_out(0x01, 0x20);        /* waveform select enable: harmless */
        us = (*ticks - t0) * 54925UL / 2000;
        printf("OPL    : %lu us per register write (%u.%u us per status read, %d+%d of them),\n",
               us, opl_read_ns / 10, opl_read_ns % 10, opl_d1, opl_d2);
        printf("         so the player costs about %lu%% of the CPU\n", us * 560 * 3 / 10000);
    }
    printf("Tracks : %d in MUSIC\\ (IMF/WLF need FM, MOD needs a DSP)\n",
           mus_ntracks);
}

/* everything off and every far buffer returned, for running a game
   in place from a bare start */
void mus_release(void)
{
    int i;
    mus_shutdown();
    for (i = 0; i < MAX_SEGS; i++)
        if (segs[i]) { _ffree(segs[i]); segs[i] = NULL; }
    nsegs = 0;
    mod_free();
    sb_release();
    mus_present = 0;
    mus_ntracks = 0;
    mus_track[0] = 0;
    loaded = 0;
}

void mus_shutdown(void)
{
    mod_stop();
    if (sb_present)
        sb_shutdown();
    if (hooked) {
        playing = 0;
        pit_set(0);             /* divisor 0 = 65536 = 18.2 Hz */
        _dos_setvect(8, old_int8);
        hooked = 0;
    }
    if (opl_present)
        opl_reset();
}

/* called from the main loop between keys: mix, advance the playlist */
void mus_poll(void)
{
    if (mus_kind == 1) {
        mod_poll();
        if (mod_done) {
            mod_done = 0;
            mus_loops++;
            mus_load((cur_track + 1) % mus_ntracks);
        }
    } else if (track_done) {
        track_done = 0;
        mus_loops++;
        mus_load((cur_track + 1) % mus_ntracks);
    }
}

void mus_toggle(void)
{
    if (!mus_present || !mus_ntracks)
        return;
    mus_on = !mus_on;              /* the MOD mixer reads this directly */
    if (mus_on && !loaded) {
        mus_load(cur_track < mus_ntracks ? cur_track : 0);
        return;
    }
    if (mus_kind == 0) {
        if (mus_on) {
            playing = 1;
        } else {
            playing = 0;
            notes_off();
        }
    }
}

void mus_skip(int dir)
{
    if (!mus_present || !mus_ntracks)
        return;
    cur_track = (cur_track + dir + mus_ntracks) % mus_ntracks;
    mus_on = 1;                 /* picking a track means: play it */
    mus_load(cur_track);
}

void mus_volume(int delta)
{
    int o;
    if (!mus_present)
        return;
    mus_vol += delta;
    if (mus_vol < 0) mus_vol = 0;
    if (mus_vol > 10) mus_vol = 10;
    if (!opl_present)
        return;                     /* the MOD mixer reads mus_vol live */
    _disable();
    for (o = 0; o < 0x16; o++)
        if (op_ch[o] >= 0)
            opl_out((unsigned char)(0x40 + o), scaled_tl(o));
    _enable();
}

/*
 * VU level in bar cells, 0..10. MOD: peak of the last mixed buffer.
 * IMF: the OPL has no audio to look at, so sum the keyed-on voices'
 * carrier levels plus any rhythm-mode drum hits, scaled by the master
 * volume. Fast attack, slow decay; call at the BIOS tick rate.
 */
int mus_vu(void)
{
    static const unsigned char op2[9] = { 3, 4, 5, 11, 12, 13, 19, 20, 21 };
    static int level = 0;               /* 0..1000 */
    int target = 0;

    if (!mus_on || !loaded) {
        level = 0;
        return 0;
    }
    if (mus_kind == 1) {
        target = mod_vu * 10;           /* 100 = full scale */
    } else {
        int ch, w = 0;
        unsigned char d;
        for (ch = 0; ch < 9; ch++)
            if (sh_b0[ch] & 0x20)
                w += 63 - (sh_tl[op2[ch]] & 0x3F);
        if (sh_bd & 0x20)
            for (d = sh_bd & 0x1F; d; d >>= 1)
                if (d & 1) w += 40;
        target = (int)((long)w * 1000 / 220 * mus_vol / 10);
    }
    if (target > 1000) target = 1000;
    if (target > level)
        level = target;
    else
        level -= (level - target) / 3 + (level > target);
    return (level + 50) / 100;
}
