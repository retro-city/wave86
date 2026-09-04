/*
 * mod.c - ProTracker MOD playback through a Sound Blaster.
 *
 * Hardware: any Sound Blaster (BLASTER env: A220 I5 D1). 8-bit mono DMA
 * in auto-init mode on DSP 2.00+, single-cycle re-armed from the IRQ on
 * SB 1.x. The IRQ handler only counts finished half-buffers; mixing is
 * done from the main loop (mod_poll) so DOS is never re-entered from an
 * interrupt.
 *
 * Mixer: 4 channels, 16.16 fixed point stepping, per-volume lookup table
 * in far memory, integer accumulate. Output rate depends on the CPU:
 * 22 kHz on 386+, 11 kHz on 286, and MODs are skipped on an 8086
 * (override with modrate= in WAVE86.INI).
 *
 * Sequencer: ProTracker semantics for the common effects (0-6, 9, A-D,
 * E1/E2/E5/E6/E9/EA/EB/EC/ED, F). 4-channel modules only (M.K., M!K!,
 * 4CHN, FLT4).
 */
#include <dos.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <i86.h>
#include "wave86.h"

#define HALF        2048            /* samples per DMA half-buffer */
#define NCH         4
#define NSMP        31
#define MAX_PAT     128

/* ----------------------------------------------------------- CPU */

/* 0xF000 -> 8086/88, 0 -> 286, anything else -> 386 or better */
unsigned cpu_flag_probe(void);
#pragma aux cpu_flag_probe = \
    "pushf"             \
    "pop  ax"           \
    "mov  cx,ax"        \
    "and  ax,0fffh"     \
    "push ax"           \
    "popf"              \
    "pushf"             \
    "pop  ax"           \
    "and  ax,0f000h"    \
    "mov  dx,ax"        \
    "mov  ax,cx"        \
    "or   ax,0f000h"    \
    "push ax"           \
    "popf"              \
    "pushf"             \
    "pop  ax"           \
    "and  ax,0f000h"    \
    "or   ax,dx"        \
    "push cx"           \
    "popf"              \
    value [ax] modify [cx dx];

int cpu_level(void)
{
    unsigned r = cpu_flag_probe();
    if (r == 0xF000) return 0;
    if (r == 0) return 2;
    return 3;
}

/* ----------------------------------------------------------- SB */

int sb_present = 0;
int sb_dsp_major = 0, sb_dsp_minor = 0;
unsigned sb_rate = 0;               /* actual output rate, 0 = MODs off */
static unsigned sb_base = 0x220;
static int sb_irq = 5, sb_dma = 1;
static int sb_autoinit = 0;
static int sb_hooked = 0;
static void (__interrupt __far *old_sb_isr)(void);
static unsigned char old_mask;

static unsigned char __far *dmabuf = NULL;   /* 2*HALF, no 64K crossing */
static unsigned char __far *dmaraw = NULL;   /* the block it lives in */
static unsigned long dmaphys;
static volatile unsigned irq_count = 0;
static unsigned mixed_count = 0;

static void dsp_write(unsigned char v)
{
    unsigned i;
    for (i = 0; i < 20000; i++)
        if (!(inp(sb_base + 0x0C) & 0x80))
            break;
    outp(sb_base + 0x0C, v);
}

static int dsp_read(void)
{
    unsigned i;
    for (i = 0; i < 20000; i++)
        if (inp(sb_base + 0x0E) & 0x80)
            return inp(sb_base + 0x0A);
    return -1;
}

static int dsp_reset(void)
{
    unsigned i;
    outp(sb_base + 0x06, 1);
    for (i = 0; i < 40; i++) inp(sb_base + 0x06);     /* > 3 us */
    outp(sb_base + 0x06, 0);
    for (i = 0; i < 2000; i++) {
        if (inp(sb_base + 0x0E) & 0x80) {
            if (inp(sb_base + 0x0A) == 0xAA)
                return 1;
        }
    }
    return 0;
}

static void __interrupt __far sb_isr(void)
{
    inp(sb_base + 0x0E);            /* acknowledge 8-bit transfer */
    if (!sb_autoinit) {             /* SB 1.x: re-arm the next half */
        dsp_write(0x14);
        dsp_write((HALF - 1) & 0xFF);
        dsp_write((HALF - 1) >> 8);
    }
    irq_count++;
    if (sb_irq >= 8)
        outp(0xA0, 0x20);
    outp(0x20, 0x20);
}

static void parse_blaster(void)
{
    const char *e = getenv("BLASTER");
    if (!e) return;
    while (*e) {
        char c = *e++;
        if (c == 'A' || c == 'a') sb_base = (unsigned)strtol(e, NULL, 16);
        else if (c == 'I' || c == 'i') sb_irq = atoi(e);
        else if (c == 'D' || c == 'd') sb_dma = atoi(e);
        while (*e && *e != ' ') e++;
        while (*e == ' ') e++;
    }
}

/* returns 1 when a usable DSP is found and the CPU can carry a mixer */
int sb_init(void)
{
    int major, minor, lvl;
    unsigned tc;

    parse_blaster();
    if (sb_dma > 3 || sb_irq > 15)
        return 0;
    if (!dsp_reset())
        return 0;
    dsp_write(0xE1);
    major = dsp_read();
    minor = dsp_read();
    if (major < 0)
        return 0;
    sb_present = 1;
    sb_dsp_major = major;
    sb_dsp_minor = minor < 0 ? 0 : minor;
    sb_autoinit = (major >= 2);

    lvl = cpu_level();
    if (cfg_modrate >= 0)
        sb_rate = (unsigned)cfg_modrate;
    else
        sb_rate = lvl >= 3 ? 22050 : (lvl == 2 ? 11025 : 0);
    if (sb_rate == 0)
        return 0;
    if (sb_rate < 5000) sb_rate = 5000;
    if (sb_rate > 22050) sb_rate = 22050;   /* plain 8-bit DSP ceiling */
    tc = 256 - 1000000UL / sb_rate;
    sb_rate = (unsigned)(1000000UL / (256 - tc));  /* what the DSP does */

    /* DMA buffer that does not cross a 64K physical page */
    if (!dmaraw)
        dmaraw = (unsigned char __far *)_fmalloc(4 * HALF);
    if (!dmaraw) {
        sb_rate = 0;
        return 0;
    }
    dmabuf = dmaraw;
    dmaphys = ((unsigned long)FP_SEG(dmabuf) << 4) + FP_OFF(dmabuf);
    if ((dmaphys & 0xFFFFUL) + 2 * HALF > 0x10000UL) {
        dmaphys = (dmaphys | 0xFFFFUL) + 1;
        dmabuf = (unsigned char __far *)MK_FP((unsigned)(dmaphys >> 4),
                                              (unsigned)(dmaphys & 15));
    }
    (void)minor;
    return 1;
}

static void dma_start(void)
{
    static const unsigned char pageport[4] = { 0x87, 0x83, 0x81, 0x82 };
    unsigned off = (unsigned)dmaphys;
    unsigned len = 2 * HALF - 1;

    outp(0x0A, 0x04 | sb_dma);              /* mask channel */
    outp(0x0C, 0);                          /* clear flip-flop */
    outp(0x0B, 0x58 | sb_dma);              /* single, auto-init, read */
    outp(sb_dma * 2, off & 0xFF);
    outp(sb_dma * 2, off >> 8);
    outp(pageport[sb_dma], (unsigned char)(dmaphys >> 16));
    outp(0x0C, 0);
    outp(sb_dma * 2 + 1, len & 0xFF);
    outp(sb_dma * 2 + 1, len >> 8);
    outp(0x0A, sb_dma);                     /* unmask */
}

static void sb_start(void)
{
    unsigned char tc = (unsigned char)(256 - 1000000UL / sb_rate);
    int vec = sb_irq < 8 ? 8 + sb_irq : 0x70 + sb_irq - 8;

    if (!sb_hooked) {
        old_sb_isr = _dos_getvect(vec);
        _dos_setvect(vec, sb_isr);
        if (sb_irq < 8) {
            old_mask = inp(0x21);
            outp(0x21, old_mask & ~(1 << sb_irq));
        } else {
            old_mask = inp(0xA1);
            outp(0xA1, old_mask & ~(1 << (sb_irq - 8)));
            outp(0x21, inp(0x21) & ~0x04);  /* cascade */
        }
        sb_hooked = 1;
    }
    irq_count = 0;
    dma_start();
    dsp_write(0xD1);                        /* speaker on */
    dsp_write(0x40);
    dsp_write(tc);
    if (sb_autoinit) {
        dsp_write(0x48);
        dsp_write((HALF - 1) & 0xFF);
        dsp_write((HALF - 1) >> 8);
        dsp_write(0x1C);
    } else {
        dsp_write(0x14);
        dsp_write((HALF - 1) & 0xFF);
        dsp_write((HALF - 1) >> 8);
    }
}

static void sb_stop(void)
{
    if (!sb_present) return;
    dsp_write(0xDA);                        /* exit auto-init */
    dsp_write(0xD0);                        /* pause DMA */
    dsp_reset();
    dsp_write(0xD3);                        /* speaker off */
    outp(0x0A, 0x04 | sb_dma);              /* mask DMA channel */
}

void sb_shutdown(void)
{
    int vec = sb_irq < 8 ? 8 + sb_irq : 0x70 + sb_irq - 8;
    sb_stop();
    if (sb_hooked) {
        if (sb_irq < 8)
            outp(0x21, (inp(0x21) & ~(1 << sb_irq)) | (old_mask & (1 << sb_irq)));
        else
            outp(0xA1, (inp(0xA1) & ~(1 << (sb_irq - 8))) |
                       (old_mask & (1 << (sb_irq - 8))));
        _dos_setvect(vec, old_sb_isr);
        sb_hooked = 0;
    }
}

static signed char __far *voltab = NULL;    /* [65][256] */

void sb_release(void)
{
    if (dmaraw) { _ffree(dmaraw); dmaraw = NULL; dmabuf = NULL; }
    if (voltab) { _ffree(voltab); voltab = NULL; }
}

/* ----------------------------------------------------------- module */

typedef struct {
    unsigned char __far *data;
    unsigned len, loopstart, looplen;   /* bytes; looplen 0 = one shot */
    unsigned char vol;
    signed char finetune;
} Sample;

typedef struct {
    /* mixer side */
    unsigned char __far *data;
    unsigned long pos, step;
    unsigned end, looplen;
    int active;
    unsigned char effvol;
    /* sequencer side */
    unsigned char smp, vol;
    signed char finetune;
    unsigned period, target;
    unsigned char effect, param;
    unsigned char portaspeed, vibspeed, vibdepth, vibpos;
    unsigned offset;
    unsigned delaynote;
    unsigned char looprow, loopcnt;
} Chan;

static Sample smp[NSMP + 1];
static unsigned char __far *pat[MAX_PAT];
static int npat = 0;
static unsigned char orders[128];
static int songlen = 0, restart = 0;
static Chan ch[NCH];

static int speed = 6, bpm = 125, tick = 0, row = 0, order = 0;
static unsigned spt = 441, tick_left = 0;   /* samples per tick */
static int jump_order = -1, break_row = -1, loop_row = -1;

int mod_playing = 0;
int mod_vu = 0;                             /* peak of the last mixed half, 0..128 */
static int vu_peak = 0;
int mod_done = 0;
int mod_order = 0, mod_row = 0;
FILE *mod_dumpf = NULL;                     /* self-test: mixed output */

static int acc[HALF];

static const unsigned char vibsine[32] = {
      0, 24, 49, 74, 97,120,141,161,180,197,212,224,235,244,250,253,
    255,253,250,244,235,224,212,197,180,161,141,120, 97, 74, 49, 24
};
/* 4096 * 2^(ft/96), ft = -8..7 */
static const unsigned ftmult[16] = {
    3866,3894,3922,3951,3979,4008,4037,4067,
    4096,4126,4156,4186,4216,4247,4278,4309
};
/* 4096 * 2^(-n/12) */
static const unsigned arpmult[16] = {
    4096,3866,3649,3444,3251,3069,2896,2734,
    2580,2436,2299,2170,2048,1933,1825,1722
};

static void set_tempo(void)
{
    spt = (unsigned)((unsigned long)sb_rate * 5 / (2 * (unsigned long)bpm));
    if (spt < 32) spt = 32;
}

static void set_step(Chan *c, unsigned period)
{
    unsigned long q, step;
    if (period < 28) period = 28;
    if (period > 3424) period = 3424;
    q = 56750320UL / period;                /* Amiga clock * 16 / period */
    step = ((q / sb_rate) << 12) + (((q % sb_rate) << 12) / sb_rate);
    if (c->finetune)
        step = (step * ftmult[c->finetune + 8]) >> 12;
    c->step = step;
}

static void trigger(Chan *c, unsigned period, unsigned offset)
{
    Sample *s;
    if (!c->smp || c->smp > NSMP) { c->active = 0; return; }
    s = &smp[c->smp];
    if (!s->data || !s->len) { c->active = 0; return; }
    c->period = period;
    c->data = s->data;
    if (s->looplen) {
        c->end = s->loopstart + s->looplen;
        c->looplen = s->looplen;
    } else {
        c->end = s->len;
        c->looplen = 0;
    }
    if (offset >= c->end) { c->active = 0; return; }
    c->pos = (unsigned long)offset << 16;
    c->active = 1;
    set_step(c, period);
}

static void vol_slide(Chan *c, unsigned char p)
{
    int v = c->vol + (p >> 4) - (p & 15);
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    c->vol = (unsigned char)v;
}

static void tone_porta(Chan *c)
{
    if (!c->target) return;
    if (c->period < c->target) {
        c->period += c->portaspeed;
        if (c->period > c->target) c->period = c->target;
    } else if (c->period > c->target) {
        if (c->period - c->target < c->portaspeed) c->period = c->target;
        else c->period -= c->portaspeed;
    }
    set_step(c, c->period);
}

static void vibrato(Chan *c)
{
    int delta = (int)vibsine[c->vibpos & 31] * c->vibdepth / 128;
    unsigned p = c->period;
    if (c->vibpos & 32) p -= delta; else p += delta;
    set_step(c, p);
    c->vibpos = (c->vibpos + c->vibspeed) & 63;
}

static void process_row(void)
{
    unsigned char __far *n;
    int i;

    if (!pat[orders[order]]) return;
    n = pat[orders[order]] + row * 16;
    for (i = 0; i < NCH; i++, n += 4) {
        Chan *c = &ch[i];
        unsigned char smpno = (n[0] & 0xF0) | (n[2] >> 4);
        unsigned period = ((unsigned)(n[0] & 0x0F) << 8) | n[1];
        unsigned char eff = n[2] & 0x0F, prm = n[3];

        c->effect = eff;
        c->param = prm;
        if (smpno && smpno <= NSMP) {
            c->smp = smpno;
            c->vol = smp[smpno].vol;
            c->finetune = smp[smpno].finetune;
        }
        if (eff == 0xE && (prm >> 4) == 5)
            c->finetune = (signed char)((prm & 15) > 7 ? (prm & 15) - 16
                                                     : (prm & 15));
        if (period) {
            if (eff == 3 || eff == 5) {
                c->target = period;
            } else if (eff == 0xE && (prm >> 4) == 0xD) {
                c->delaynote = period;
            } else {
                unsigned off = 0;
                if (eff == 9) {
                    if (prm) c->offset = (unsigned)prm << 8;
                    off = c->offset;
                }
                trigger(c, period, off);
                c->vibpos = 0;
            }
        }
        switch (eff) {
        case 0x3: if (prm) c->portaspeed = prm; break;
        case 0x4:
            if (prm & 0xF0) c->vibspeed = prm >> 4;
            if (prm & 0x0F) c->vibdepth = prm & 15;
            break;
        case 0xB: jump_order = prm; break;
        case 0xC: c->vol = prm > 64 ? 64 : prm; break;
        case 0xD:
            break_row = (prm >> 4) * 10 + (prm & 15);
            if (break_row > 63) break_row = 0;
            break;
        case 0xE:
            switch (prm >> 4) {
            case 0x1: if (c->period > 113 + (prm & 15)) c->period -= prm & 15;
                      set_step(c, c->period); break;
            case 0x2: c->period += prm & 15;
                      if (c->period > 856) c->period = 856;
                      set_step(c, c->period); break;
            case 0x6:
                if ((prm & 15) == 0) {
                    c->looprow = (unsigned char)row;
                } else {
                    if (c->loopcnt == 0) c->loopcnt = prm & 15;
                    else c->loopcnt--;
                    if (c->loopcnt) loop_row = c->looprow;
                }
                break;
            case 0xA: vol_slide(c, (unsigned char)((prm & 15) << 4)); break;
            case 0xB: vol_slide(c, (unsigned char)(prm & 15)); break;
            case 0xC: if ((prm & 15) == 0) c->vol = 0; break;
            }
            break;
        case 0xF:
            if (prm == 0) break;
            if (prm <= 32) speed = prm;
            else { bpm = prm; set_tempo(); }
            break;
        }
    }
}

static void process_tick(void)
{
    int i;
    for (i = 0; i < NCH; i++) {
        Chan *c = &ch[i];
        unsigned char prm = c->param;
        switch (c->effect) {
        case 0x0:
            if (prm) {
                int n = tick % 3;
                unsigned p = c->period;
                if (n == 1) p = (unsigned)(((unsigned long)p * arpmult[prm >> 4]) >> 12);
                else if (n == 2) p = (unsigned)(((unsigned long)p * arpmult[prm & 15]) >> 12);
                set_step(c, p);
            }
            break;
        case 0x1:
            if (c->period > 113 + prm) c->period -= prm; else c->period = 113;
            set_step(c, c->period);
            break;
        case 0x2:
            c->period += prm;
            if (c->period > 856) c->period = 856;
            set_step(c, c->period);
            break;
        case 0x3: tone_porta(c); break;
        case 0x4: vibrato(c); break;
        case 0x5: tone_porta(c); vol_slide(c, prm); break;
        case 0x6: vibrato(c); vol_slide(c, prm); break;
        case 0xA: vol_slide(c, prm); break;
        case 0xE:
            switch (prm >> 4) {
            case 0x9:
                if ((prm & 15) && tick % (prm & 15) == 0)
                    trigger(c, c->period, 0);
                break;
            case 0xC: if (tick == (prm & 15)) c->vol = 0; break;
            case 0xD:
                if (tick == (prm & 15) && c->delaynote) {
                    trigger(c, c->delaynote, 0);
                    c->delaynote = 0;
                }
                break;
            }
            break;
        }
    }
}

static void seq_tick(void)
{
    if (tick == 0)
        process_row();
    else
        process_tick();

    tick++;
    if (tick >= speed) {
        tick = 0;
        if (loop_row >= 0) {
            row = loop_row;
            loop_row = -1;
        } else if (jump_order >= 0 || break_row >= 0) {
            if (jump_order >= 0) order = jump_order; else order++;
            row = break_row >= 0 ? break_row : 0;
            jump_order = break_row = -1;
        } else {
            row++;
            if (row >= 64) { row = 0; order++; }
        }
        if (order >= songlen) {
            order = restart < songlen ? restart : 0;
            mod_done = 1;
        }
        mod_order = order;
        mod_row = row;
    }
}

static void mix_chunk(unsigned char __far *dst, unsigned n)
{
    unsigned i, k;
    int mvol = mus_vol;

    for (i = 0; i < n; i++)
        acc[i] = 0;

    for (k = 0; k < NCH; k++) {
        Chan *c = &ch[k];
        unsigned char __far *s;
        const signed char __far *vt;
        union { unsigned long l; struct { unsigned lo, hi; } w; } p;
        unsigned long step;
        unsigned end, looplen, cnt;
        int *a = acc;

        if (!c->active) continue;
        c->effvol = (unsigned char)((c->vol * mvol) / 10);
        if (c->effvol == 0) {           /* silent: just advance position */
            unsigned long adv = c->step * n;
            p.l = c->pos + adv;
            if (c->looplen) {
                while (p.w.hi >= c->end) p.w.hi -= c->looplen;
            } else if (p.w.hi >= c->end) {
                c->active = 0;
            }
            c->pos = p.l;
            continue;
        }
        s = c->data;
        vt = voltab + (unsigned)c->effvol * 256;
        p.l = c->pos;
        step = c->step;
        end = c->end;
        looplen = c->looplen;
        cnt = n;
        while (cnt--) {
            if (p.w.hi >= end) {
                if (looplen) {
                    do { p.w.hi -= looplen; } while (p.w.hi >= end);
                } else {
                    c->active = 0;
                    break;
                }
            }
            *a++ += vt[s[p.w.hi]];
            p.l += step;
        }
        c->pos = p.l;
    }

    for (i = 0; i < n; i++) {
        int v = acc[i] >> 1;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        dst[i] = (unsigned char)(v + 128);
        if (v < 0) v = -v;
        if (v > vu_peak) vu_peak = v;
    }
}

static void mix_half(int half)
{
    unsigned char __far *dst = dmabuf + half * HALF;
    unsigned n = HALF;

    if (!mus_on || !mod_playing) {
        unsigned i;
        for (i = 0; i < HALF; i++) dst[i] = 0x80;
        mod_vu = 0;
        return;
    }
    vu_peak = 0;
    while (n) {
        unsigned chunk;
        if (tick_left == 0) {
            seq_tick();
            tick_left = spt;
        }
        chunk = n < tick_left ? n : tick_left;
        mix_chunk(dst, chunk);
        dst += chunk;
        n -= chunk;
        tick_left -= chunk;
    }
    mod_vu = vu_peak;
    if (mod_dumpf) {
        static unsigned char tmp[256];
        unsigned i, k;
        dst = dmabuf + half * HALF;
        for (i = 0; i < HALF; i += 256) {
            for (k = 0; k < 256; k++) tmp[k] = dst[i + k];
            fwrite(tmp, 1, 256, mod_dumpf);
        }
    }
}

/* main loop: keep the DMA ring two halves ahead of the card */
void mod_poll(void)
{
    if (!mod_playing) return;
    while ((unsigned)(mixed_count - irq_count) < 2) {
        mix_half(mixed_count & 1);
        mixed_count++;
    }
}

void mod_free(void)
{
    int i;
    for (i = 0; i < MAX_PAT; i++)
        if (pat[i]) { _ffree(pat[i]); pat[i] = NULL; }
    for (i = 0; i <= NSMP; i++)
        if (smp[i].data) { _ffree(smp[i].data); smp[i].data = NULL; }
    npat = 0;
}

static int read_far(FILE *f, unsigned char __far *dst, unsigned len)
{
    static unsigned char buf[1024];
    unsigned got = 0;
    while (got < len) {
        unsigned want = len - got > sizeof(buf) ? sizeof(buf) : len - got;
        unsigned n = fread(buf, 1, want, f);
        unsigned k;
        if (n == 0) return 0;
        for (k = 0; k < n; k++) dst[got + k] = buf[k];
        got += n;
    }
    return 1;
}

int mod_load(const char *path)
{
    static unsigned char hdr[1084];
    FILE *f;
    int i;
    unsigned long skip[NSMP + 1];

    mod_free();
    if (!voltab) {
        int v, b;
        voltab = (signed char __far *)_fmalloc(65U * 256U);
        if (!voltab) return 1;
        for (v = 0; v <= 64; v++)
            for (b = 0; b < 256; b++)
                voltab[v * 256 + b] =
                    (signed char)(((int)(signed char)b * v) >> 6);
    }

    f = fopen(path, "rb");
    if (!f) return 1;
    if (fread(hdr, 1, 1084, f) != 1084) { fclose(f); return 1; }
    if (memcmp(hdr + 1080, "M.K.", 4) && memcmp(hdr + 1080, "M!K!", 4) &&
        memcmp(hdr + 1080, "4CHN", 4) && memcmp(hdr + 1080, "FLT4", 4)) {
        fclose(f);
        return 1;                       /* not a 4-channel PT module */
    }

    songlen = hdr[950];
    restart = hdr[951];
    if (songlen == 0 || songlen > 128) songlen = 1;
    memcpy(orders, hdr + 952, 128);
    npat = 0;
    for (i = 0; i < 128; i++)
        if (orders[i] + 1 > npat) npat = orders[i] + 1;

    for (i = 1; i <= NSMP; i++) {
        unsigned char *h = hdr + 20 + (i - 1) * 30;
        unsigned long len = ((unsigned long)((h[22] << 8) | h[23])) * 2;
        unsigned ls = (((unsigned)h[26] << 8) | h[27]) * 2;
        unsigned ll = (((unsigned)h[28] << 8) | h[29]) * 2;
        Sample *s = &smp[i];
        skip[i] = 0;
        /* keep sample + block header inside one segment */
        if (len > 65000UL) { skip[i] = len - 65000UL; len = 65000UL; }
        s->len = (unsigned)len;
        s->finetune = (signed char)((h[24] & 15) > 7 ? (h[24] & 15) - 16
                                                    : (h[24] & 15));
        s->vol = h[25] > 64 ? 64 : h[25];
        if (ll <= 2 || ls >= s->len) { ls = 0; ll = 0; }
        else if ((unsigned long)ls + ll > s->len) ll = s->len - ls;
        s->loopstart = ls;
        s->looplen = ll;
    }

    for (i = 0; i < npat; i++) {
        pat[i] = (unsigned char __far *)_fmalloc(1024);
        if (!pat[i] || !read_far(f, pat[i], 1024)) {
            fclose(f); mod_free(); return 1;
        }
    }
    for (i = 1; i <= NSMP; i++) {
        Sample *s = &smp[i];
        if (!s->len) continue;
        s->data = (unsigned char __far *)_fmalloc(s->len);
        if (!s->data || !read_far(f, s->data, s->len)) {
            fclose(f); mod_free(); return 1;
        }
        if (skip[i]) fseek(f, (long)skip[i], SEEK_CUR);
    }
    fclose(f);
    return 0;
}

void mod_start(void)
{
    int i;
    memset(ch, 0, sizeof(ch));
    for (i = 0; i < NCH; i++) ch[i].vol = 64;
    speed = 6; bpm = 125; tick = 0; row = 0; order = 0;
    jump_order = break_row = loop_row = -1;
    mod_done = 0;
    mod_order = mod_row = 0;
    set_tempo();
    tick_left = 0;
    mod_playing = 1;
    irq_count = 0;
    mixed_count = 0;
    mix_half(0);
    mix_half(1);
    mixed_count = 2;
    sb_start();
}

void mod_stop(void)
{
    if (!mod_playing) return;
    mod_playing = 0;
    sb_stop();
}

unsigned mod_irqs(void)
{
    return irq_count;
}
