/*
 * midi.c - Standard MIDI Files (.MID) through an MPU-401 in UART mode.
 *
 * A file is read whole into far memory, one block per MTrk chunk
 * (formats 0 and 1; of a format 2 file the first song), and played from
 * music.c's 560 Hz timer interrupt: every tick moves the song clock on by
 * the MIDI ticks that much time is worth at the current tempo, and the
 * events that have come due go out of the MPU-401's data port in time
 * order across the tracks, each with its status byte.
 *
 * The bytes go out through a ring buffer. A real MPU-401 takes one about
 * every 320 microseconds, the speed of the MIDI cable, and waiting for it
 * inside the interrupt would stop the clock, the keyboard and the network
 * card for as long as a burst takes: the timer hands out no new events
 * while the ring is a quarter full, and sends what the MPU will take in a
 * few hundred status reads a tick.
 *
 * The MPU goes into UART mode for the first MIDI song and is reset on
 * every way out, as a program that finds it in UART mode gets no answer
 * to its own reset.
 *
 * No note is left hanging. Every Note On is remembered until its Note
 * Off, and midi_silence() - on pause, before every track change, and
 * from mus_shutdown(), so on every way out of the launcher and WAVEGET -
 * sends the Note Off for each note still sounding, then Sustain off and
 * All Notes Off on all sixteen channels.
 *
 * Volume scales each channel's Channel Volume (controller 7). The value
 * the song asked for is kept, so a change of volume goes out at once.
 */
#include <dos.h>
#include <conio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <i86.h>
#include "wave86.h"

#define MAX_MTRK   32
#define MAX_TRKLEN 65000U       /* one far block per track */
#define MAX_SYSEX  512          /* a longer dump is left out */
#define RING       1024U        /* bytes on their way to the MPU; a power of two */
#define RING_HOLD  256U         /* the timer hands out no new events above this */
#define MIN_TEMPO  1000UL       /* microseconds a quarter note; faster is broken */
#define DRR        0x40         /* status: set while the MPU cannot take a byte */
#define DSR        0x80         /* status: clear while it has a byte for us */

typedef struct {
    unsigned char __far *data;
    unsigned len, pos;
    unsigned long next;         /* the song tick of its next event */
    unsigned char rs;           /* running status */
    unsigned char done;
} MTrk;

static MTrk __far trk[MAX_MTRK];
static int ntrk = 0;
static unsigned file_div = 96;  /* the header's division, as it came */
static unsigned division = 96;  /* MIDI ticks per quarter note */
static int smpte = 0;           /* SMPTE time: ticks per second, no tempo */
static unsigned long tempo = 500000UL;  /* microseconds per quarter note */
static unsigned tick_us16;      /* one timer tick in 1/16 microseconds */
static unsigned long step;      /* MIDI ticks per timer tick, 16.16 */
unsigned long midi_clk = 0;     /* the song clock, in MIDI ticks (tests) */
static unsigned clk_frac = 0;
static unsigned long next_due;  /* the soonest event of any track */
static int next_trk = -1;       /* whose it is; -1 = the song is over */

static unsigned char __far notes[16][16];   /* sounding notes, a bit each */
static unsigned char __far cc7[16];         /* each channel's volume as the song set it */
static volatile int active = 0;             /* notes sounding */
static volatile unsigned char peak = 0;     /* loudest Note On since the VU looked */
static int dirty = 0;                       /* something went out since the last silence */

int mpu_present = 0;
unsigned mpu_port = 0x330;
int mpu_ack_reset = -1, mpu_ack_uart = -1;  /* /diag: the answers to FFh and 3Fh */
static int mpu_dead = 0;                    /* stopped taking bytes: send no more */
static int mpu_uart = 0;                    /* we put it in UART mode */

/* the ring: filled and emptied with interrupts off, by the timer and by
   the main loop alike */
static unsigned char __far ring[RING];
static unsigned rhead = 0, rtail = 0;       /* in at the head, out at the tail */
static unsigned stall = 0;                  /* status reads since the MPU last took a byte */
/* status reads a tick may spend on a busy MPU: about a fifth of the tick
   on any machine, as a read costs a microsecond on a 386's bus and four
   or five on an 8088 */
static unsigned isr_polls = 384;
#define RFILL() ((rhead - rtail) & (RING - 1))

/* a system exclusive message split over several events: never break into
   one, and when it had to be cut short drop what was left of it */
static int in_sysex = 0;
static int skip_cont = 0;

/* /mustest: every byte sent, when the test gives a buffer */
unsigned char __far *midi_log = NULL;
unsigned midi_log_len = 0, midi_log_cap = 0;

/* ------------------------------------------------------------ MPU-401 */

/* wait until the MPU takes a byte; what it has for us (MIDI in, an ack)
   is read and dropped, as some cards stop taking bytes until it is */
static int mpu_ready(void)
{
    unsigned n;
    for (n = 0; n < 0x8000U; n++) {
        unsigned char s = (unsigned char)inp(mpu_port + 1);
        if (!(s & DRR))
            return 1;
        if (!(s & DSR))
            inp(mpu_port);
    }
    return 0;
}

/* send what the MPU will take, spending at most `polls` status reads on
   it while it is busy; interrupts off */
static void drain(unsigned polls)
{
    while (rhead != rtail) {
        unsigned char s = (unsigned char)inp(mpu_port + 1);
        if (!(s & DRR)) {
            unsigned char b = ring[rtail];
            outp(mpu_port, b);
            rtail = (rtail + 1) & (RING - 1);
            stall = 0;
            if (midi_log && midi_log_len < midi_log_cap)
                midi_log[midi_log_len++] = b;
            continue;
        }
        if (!(s & DSR))
            inp(mpu_port);
        if (++stall >= 0x8000U) {
            mpu_dead = 1;       /* it stopped taking bytes: gone */
            rtail = rhead;
            return;
        }
        if (!polls--)
            return;
    }
}

/* into the ring; interrupts off */
static void mpu_put(unsigned char b)
{
    if (mpu_dead || RFILL() == RING - 1)
        return;
    ring[rhead] = b;
    rhead = (rhead + 1) & (RING - 1);
    dirty = 1;
}

/* from the main loop: wait until n more bytes fit, sending meanwhile */
static void room(unsigned n)
{
    while (!mpu_dead && RING - 1 - RFILL() < n) {
        _disable();
        drain(64);
        _enable();
    }
}

/* from the main loop: everything queued out to the MPU */
static void flush(void)
{
    while (!mpu_dead && rhead != rtail) {
        _disable();
        drain(64);
        _enable();
    }
}

/* a command, and whether the MPU acknowledged it (FEh) */
static int mpu_cmd(unsigned char c)
{
    unsigned n;
    if (!mpu_ready())
        return 0;
    outp(mpu_port + 1, c);
    for (n = 0; n < 0x8000U; n++)
        if (!(inp(mpu_port + 1) & DSR) && (unsigned char)inp(mpu_port) == 0xFE)
            return 1;
    return 0;
}

/*
 * Is there an MPU-401: does it acknowledge a reset? A reset is sent twice
 * when the first goes unanswered, as an MPU left in UART mode may not
 * acknowledge the one that takes it out. It stays out of UART mode until
 * a MIDI song plays. The port is mpuport= in the INI, else the P of
 * BLASTER, else 330h; mpu=0 leaves MIDI out, mpu=1 plays to the port
 * whatever it answers.
 */
int mpu_init(void)
{
    mpu_present = 0;
    mpu_dead = 0;
    if (cfg_mpu == 0)
        return 0;
    mpu_port = 0x330;
    if (cfg_mpuport)
        mpu_port = cfg_mpuport;
    else {
        const char *bl0 = getenv("BLASTER"), *bl;
        for (bl = bl0; bl && *bl; bl++)
            if ((*bl == 'P' || *bl == 'p') && (bl == bl0 || bl[-1] == ' ')) {
                unsigned p = (unsigned)strtoul(bl + 1, NULL, 16);
                if (p)
                    mpu_port = p;
                break;
            }
    }
    if ((unsigned char)inp(mpu_port + 1) == 0xFF && cfg_mpu != 1) {
        mpu_ack_reset = mpu_ack_uart = 0;       /* nothing on the bus there */
        return 0;
    }
    mpu_ack_reset = mpu_cmd(0xFF);
    if (!mpu_ack_reset)
        mpu_ack_reset = mpu_cmd(0xFF);
    mpu_present = mpu_ack_reset || cfg_mpu == 1;
    mpu_dead = 0;
    mpu_uart = 0;
    rhead = rtail = 0;
    stall = 0;
    dirty = 0;
    return mpu_present;
}

/*
 * UART mode on, for the first MIDI song; off again on every way out, once
 * the last bytes have had a BIOS tick to leave the MPU's own buffer (a
 * reset may cut them short), so the next program finds the MPU as it
 * expects one.
 */
void mpu_uart_mode(int on)
{
    unsigned n;
    if (!mpu_present)
        return;
    if (on) {
        if (mpu_uart || mpu_dead)
            return;
        mpu_ack_uart = mpu_cmd(0x3F);
        mpu_uart = 1;           /* answered or not: a forced one may not */
        stall = 0;
        return;
    }
    if (!mpu_uart)
        return;
    flush();
    if (!mpu_dead) {
        unsigned long __far *ticks = (unsigned long __far *)MK_FP(0x40, 0x6C);
        unsigned long t0 = *ticks;
        for (n = 0; n < 0xFFFFU && *ticks == t0; n++)
            inp(mpu_port + 1);
        if (mpu_ready()) {
            outp(mpu_port + 1, 0xFF);
            for (n = 0; n < 200; n++)       /* its answer, if it gives one */
                if (!(inp(mpu_port + 1) & DSR))
                    inp(mpu_port);
        }
    }
    mpu_uart = 0;
    rhead = rtail = 0;
}

static unsigned char scaled(unsigned char v)
{
    return (unsigned char)((unsigned)v * mus_vol / 10);
}

/* one channel message; from the main loop it must not be split by the
   timer's own, so interrupts are off for the two or three bytes */
static void send3(unsigned char st, unsigned char a, unsigned char b)
{
    unsigned char hi = st & 0xF0;
    mpu_put(st);
    mpu_put(a);
    if (hi != 0xC0 && hi != 0xD0)
        mpu_put(b);
}

static void send3_main(unsigned char st, unsigned char a, unsigned char b)
{
    room(4);
    _disable();
    if (in_sysex) {             /* never into the middle of one: end it, drop its rest */
        mpu_put(0xF7);
        in_sysex = 0;
        skip_cont = 1;
    }
    send3(st, a, b);
    _enable();
}

/* ------------------------------------------------------------ the file */

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}

static int dos_skip(int fd, unsigned long n)
{
    union REGS r;
    r.x.ax = 0x4201;            /* seek from here */
    r.x.bx = fd;
    r.x.cx = (unsigned)(n >> 16);
    r.x.dx = (unsigned)n;
    intdos(&r, &r);
    return r.x.cflag ? -1 : 0;
}

void midi_free(void)
{
    int i;
    for (i = 0; i < ntrk; i++)
        if (trk[i].data) {
            _ffree(trk[i].data);
            trk[i].data = NULL;
        }
    ntrk = 0;
    next_trk = -1;
}

int midi_load(const char *path)
{
    unsigned char h[14];
    unsigned long hlen;
    unsigned n;
    int fd, fmt, want;

    midi_free();
    if (_dos_open(path, O_RDONLY, &fd) != 0)
        return 1;
    if (_dos_read(fd, h, 14, &n) || n != 14 || memcmp(h, "MThd", 4))
        goto bad;
    hlen = be32(h + 4);
    fmt = (h[8] << 8) | h[9];
    want = (h[10] << 8) | h[11];
    file_div = (h[12] << 8) | h[13];
    if (hlen < 6 || (hlen > 6 && dos_skip(fd, hlen - 6)))
        goto bad;
    if ((file_div & 0x8000) ? (file_div & 0xFF) == 0 : file_div == 0)
        goto bad;               /* no ticks at all: the clock would never move */
    if (fmt == 2)
        want = 1;               /* separate songs: the first */
    if (want > MAX_MTRK)
        want = MAX_MTRK;
    while (ntrk < want) {
        unsigned long len;
        MTrk __far *t;
        if (_dos_read(fd, h, 8, &n) || n != 8)
            break;              /* fewer tracks than it says: play those */
        len = be32(h + 4);
        if (memcmp(h, "MTrk", 4)) {
            if (dos_skip(fd, len))
                break;
            continue;           /* a chunk of some other kind */
        }
        if (len > MAX_TRKLEN)
            goto bad;
        t = &trk[ntrk++];
        t->len = (unsigned)len;
        t->data = len ? (unsigned char __far *)_fmalloc(t->len) : NULL;
        if (len && !t->data)
            goto bad;
        if (len && _dos_read(fd, t->data, t->len, &n))
            goto bad;
        if (len && n != t->len)
            t->len = n;         /* cut short: what is there */
    }
    _dos_close(fd);
    return ntrk ? 0 : 1;
bad:
    _dos_close(fd);
    midi_free();
    return 1;
}

/* ------------------------------------------------------------ playing */

/* MIDI ticks per timer tick at this tempo, 16.16: tick_us * division /
   tempo, in three steps so no product passes 32 bits */
static void set_step(void)
{
    unsigned long num = (unsigned long)tick_us16 * division;
    unsigned long t = tempo, r, s;
    s = (num / t) << 12;                /* whole ticks: 65536 / 16 */
    r = (num % t) << 6;
    s += (r / t) << 6;
    r = (r % t) << 6;
    s += r / t;
    step = s;
}

static unsigned char getb(MTrk __far *t)
{
    if (t->pos < t->len)
        return t->data[t->pos++];
    t->done = 1;
    return 0;
}

static unsigned long vlq(MTrk __far *t)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        unsigned char b = getb(t);
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return v;
    }
    t->done = 1;                /* longer than MIDI allows: broken */
    return 0;
}

static void find_next(void)
{
    int i;
    next_trk = -1;
    for (i = 0; i < ntrk; i++)
        if (!trk[i].done && (next_trk < 0 || trk[i].next < next_due)) {
            next_trk = i;
            next_due = trk[i].next;
        }
}

static void channel_msg(unsigned char st, unsigned char a, unsigned char b)
{
    unsigned char hi = st & 0xF0, ch = st & 0x0F;
    unsigned char __far *row = notes[ch];
    unsigned char bit;

    a &= 0x7F;
    b &= 0x7F;
    bit = (unsigned char)(1 << (a & 7));
    if (hi == 0x90 && b) {
        if (!(row[a >> 3] & bit)) {
            row[a >> 3] |= bit;
            active++;
        }
        if (b > peak)
            peak = b;
    } else if (hi == 0x80 || hi == 0x90) {
        if (row[a >> 3] & bit) {
            row[a >> 3] &= (unsigned char)~bit;
            active--;
        }
    } else if (hi == 0xB0) {
        if (a == 7) {
            cc7[ch] = b;
            b = scaled(b);
        } else if (a == 120 || a == 123) {     /* the song's own all-off */
            int i;
            for (i = 0; i < 16; i++) {
                unsigned char m;
                for (m = row[i]; m; m &= (unsigned char)(m - 1))
                    active--;
                row[i] = 0;
            }
        }
    }
    send3(st, a, b);
}

static void play_event(MTrk __far *t)
{
    unsigned char st = getb(t), a = 0, b = 0;

    if (st < 0x80) {            /* running status: that was the first data byte */
        if (!t->rs) {
            t->done = 1;
            return;
        }
        a = st;
        st = t->rs;
    } else if (st < 0xF0) {
        t->rs = st;
        a = getb(t);
    }
    if (st < 0xF0) {
        if ((st & 0xE0) != 0xC0)    /* Cx and Dx have one data byte */
            b = getb(t);
        if (!t->done)
            channel_msg(st, a, b);
    } else if (st == 0xFF) {        /* meta: tempo and end of track matter */
        unsigned char type = getb(t);
        unsigned long len = vlq(t);
        if (type == 0x2F) {
            t->done = 1;
            return;
        }
        if (type == 0x51 && len == 3 && !smpte) {
            unsigned long v = getb(t);
            v = (v << 8) | getb(t);
            v = (v << 8) | getb(t);
            tempo = v < MIN_TEMPO ? MIN_TEMPO : v;
            set_step();
            len = 0;
        }
        if (len > (unsigned long)(t->len - t->pos))
            t->done = 1;
        else
            t->pos += (unsigned)len;
    } else if (st == 0xF0 || st == 0xF7) {      /* system exclusive, or escaped bytes */
        unsigned long len = vlq(t);
        if (len > (unsigned long)(t->len - t->pos)) {
            t->done = 1;
        } else {
            unsigned n = (unsigned)len;
            int ends = n && t->data[t->pos + n - 1] == 0xF7;
            int send = n <= MAX_SYSEX;
            if (st == 0xF0) {
                skip_cont = !send && !ends;     /* too long to send: its continuations too */
                in_sysex = send && !ends;
                if (send)
                    mpu_put(0xF0);
            } else if (skip_cont) {
                send = 0;
                if (ends)
                    skip_cont = 0;
            } else if (send && in_sysex && ends) {
                in_sysex = 0;
            }
            if (send)
                while (n--)
                    mpu_put(t->data[t->pos++]);
            else
                t->pos += n;
        }
    } else {
        t->done = 1;            /* F1..FE have no place in a file */
    }
    if (!t->done)
        t->next += vlq(t);
}

/*
 * The timer interrupt's share, once per tick while a MIDI track plays:
 * the clock moves on, what has come due goes into the ring, and what the
 * MPU will take goes out. Returns 1 when the song is over. At most 64
 * events a tick. While the ring is a quarter full - a burst of system
 * exclusive the cable takes seconds over - the song's clock waits with
 * it, so the music slows for a moment rather than the interrupt holding
 * the machine, or the notes due meanwhile all going out at once after.
 */
int midi_tick(void)
{
    int guard = 64;
    if (RFILL() < RING_HOLD) {
        unsigned long f = (unsigned long)clk_frac + (unsigned)(step & 0xFFFFU);
        midi_clk += (step >> 16) + (f >> 16);
        clk_frac = (unsigned)(f & 0xFFFFU);
        while (next_trk >= 0 && next_due <= midi_clk && RFILL() < RING_HOLD && guard--) {
            play_event(&trk[next_trk]);
            find_next();
        }
    }
    drain(isr_polls);
    return next_trk < 0;
}

/* back to the start of the loaded song, every channel reset: controllers,
   pitch bend, program 0, and the volume at 100 as scaled. Called with the
   timer's MIDI share stopped. */
void midi_start(unsigned pit_div)
{
    int i;
    tick_us16 = (unsigned)((unsigned long)pit_div * 134097UL / 10000UL);
    in_sysex = skip_cont = 0;
    i = cpu_level();
    isr_polls = i >= 3 ? 384 : (i == 2 ? 192 : 80);
    if (file_div & 0x8000) {        /* SMPTE: frames a second x ticks a frame */
        unsigned fps = 256 - (file_div >> 8);
        division = fps * (file_div & 0xFF);
        tempo = 1000000UL;          /* a "quarter note" of one second */
        smpte = 1;
    } else {
        division = file_div;
        tempo = 500000UL;           /* 120 beats a minute until the song says */
        smpte = 0;
    }
    set_step();
    midi_clk = 0;
    clk_frac = 0;
    for (i = 0; i < ntrk; i++) {
        MTrk __far *t = &trk[i];
        t->pos = 0;
        t->rs = 0;
        t->done = t->len == 0;
        t->next = 0;
        if (!t->done)
            t->next = vlq(t);
    }
    find_next();
    for (i = 0; i < 16; i++) {
        unsigned char ch = (unsigned char)i;
        cc7[i] = 100;
        send3_main((unsigned char)(0xB0 | ch), 121, 0);     /* reset all controllers */
        send3_main((unsigned char)(0xB0 | ch), 7, scaled(100));
        send3_main((unsigned char)(0xB0 | ch), 10, 64);     /* pan to the middle */
        send3_main((unsigned char)(0xE0 | ch), 0, 64);      /* pitch bend to the middle */
        send3_main((unsigned char)(0xC0 | ch), 0, 0);       /* program 0 */
    }
}

/* every note off, and the pedal up: pause, track change, and every exit */
void midi_silence(void)
{
    int ch, i, k;
    if (!mpu_present || !dirty)
        return;
    for (ch = 0; ch < 16; ch++) {
        unsigned char c = (unsigned char)ch;
        send3_main((unsigned char)(0xB0 | c), 64, 0);       /* sustain off */
        for (i = 0; i < 16; i++) {
            if (!notes[ch][i])
                continue;
            for (k = 0; k < 8; k++)
                if (notes[ch][i] & (1 << k))
                    send3_main((unsigned char)(0x80 | c), (unsigned char)(i * 8 + k), 64);
            notes[ch][i] = 0;
        }
        send3_main((unsigned char)(0xB0 | c), 123, 0);      /* all notes off */
    }
    flush();                    /* out now: the timer may not be there to send them */
    active = 0;
    dirty = 0;
}

/* the volume changed: each channel's volume again, scaled anew */
void midi_volume(void)
{
    int ch;
    for (ch = 0; ch < 16; ch++)
        send3_main((unsigned char)(0xB0 | ch), 7, scaled(cc7[ch]));
}

/* for the VU: 0..1000 from the notes sounding and the loudest new one */
int midi_level(void)
{
    int v = peak * 5 + active * 30;
    peak = 0;
    return v > 1000 ? 1000 : v;
}
