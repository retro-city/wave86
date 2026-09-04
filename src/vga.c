/*
 * vga.c - video detection, synthwave palette, direct text output.
 * All BIOS calls, no VGA register magic except the DAC (via BIOS too),
 * so this stays friendly to old cards.
 */
#include <i86.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>
#include "wave86.h"

int vid_is_vga = 0;
int vid_is_color = 1;
int opt_nopal = 0;

static unsigned short __far *screen(void)
{
    return (unsigned short __far *)MK_FP(vid_is_color ? 0xB800 : 0xB000, 0);
}

void vid_detect(void)
{
    union REGS r;

    /* VGA check: int 10h AX=1A00h, supported => AL=1Ah */
    r.x.ax = 0x1A00;
    int86(0x10, &r, &r);
    if (r.h.al == 0x1A) {
        unsigned char code = r.h.bl;
        if (code == 7 || code == 8 || code == 0x0A || code == 0x0B ||
            code == 0x0C)
            vid_is_vga = 1;
    }

    /* current mode: 7 = MDA/Hercules text */
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    vid_is_color = (r.h.al != 7);
}

/*
 * Synthwave palette: replace the 16 text colors with a neon sunset set.
 * Attribute palette registers are remapped 1:1 onto DAC entries 0-15,
 * then the DAC entries get our colors. VGA only; EGA/CGA keep stock
 * colors (which are already conveniently magenta/cyan flavored).
 */
static const unsigned char synth_pal[16][3] = {
    {  2,  0,  8 },  /* 0 black  -> void purple           */
    {  9,  4, 24 },  /* 1 blue   -> deep indigo           */
    {  0, 34, 24 },  /* 2 green  -> sea teal              */
    {  0, 46, 52 },  /* 3 cyan   -> neon cyan             */
    { 52,  6, 22 },  /* 4 red    -> laser crimson         */
    { 44,  0, 42 },  /* 5 magenta-> ultraviolet           */
    { 58, 24,  2 },  /* 6 brown  -> sunset orange         */
    { 36, 32, 46 },  /* 7 lgray  -> lavender gray         */
    { 15,  9, 26 },  /* 8 dgray  -> dusk purple           */
    { 27, 24, 60 },  /* 9 lblue  -> periwinkle            */
    { 18, 60, 40 },  /* A lgreen -> mint glow             */
    { 28, 63, 63 },  /* B lcyan  -> electric cyan         */
    { 63, 26, 34 },  /* C lred   -> coral flare           */
    { 63, 24, 56 },  /* D lmag   -> hot pink              */
    { 63, 54, 16 },  /* E yellow -> chrome gold           */
    { 62, 58, 63 },  /* F white  -> starlight             */
};

void vid_set_palette(void)
{
    union REGS r;
    int i;

    if (!vid_is_vga || opt_nopal)
        return;

    for (i = 0; i < 16; i++) {
        /* map attribute register i -> DAC index i */
        r.x.ax = 0x1000;
        r.h.bl = (unsigned char)i;
        r.h.bh = (unsigned char)i;
        int86(0x10, &r, &r);
        /* program DAC entry i */
        r.x.ax = 0x1010;
        r.x.bx = i;
        r.h.dh = synth_pal[i][0];
        r.h.ch = synth_pal[i][1];
        r.h.cl = synth_pal[i][2];
        int86(0x10, &r, &r);
    }
}

void vid_text_mode(void)
{
    union REGS r;
    r.x.ax = vid_is_color ? 0x0003 : 0x0007;
    int86(0x10, &r, &r);
    /* hide the blinking cursor */
    r.h.ah = 0x01;
    r.x.cx = 0x2000;
    int86(0x10, &r, &r);
}

void scr_put(int x, int y, unsigned char ch, unsigned char attr)
{
    screen()[y * 80 + x] = (unsigned short)((attr << 8) | ch);
}

void scr_puts(int x, int y, const char *s, unsigned char attr)
{
    unsigned short __far *p = screen() + y * 80 + x;
    while (*s)
        *p++ = (unsigned short)((attr << 8) | (unsigned char)*s++);
}

void scr_fill(int x, int y, int w, int h, unsigned char ch, unsigned char attr)
{
    int i, j;
    for (j = 0; j < h; j++) {
        unsigned short __far *p = screen() + (y + j) * 80 + x;
        for (i = 0; i < w; i++)
            *p++ = (unsigned short)((attr << 8) | ch);
    }
}

void scr_hline(int x, int y, int w, unsigned char ch, unsigned char attr)
{
    scr_fill(x, y, w, 1, ch, attr);
}

/*
 * Self-test dump: text buffer, the active 8x16 BIOS font and the palette,
 * so the host build system can render a pixel-perfect screenshot.
 */
unsigned scr_dump(const char *scrfile, const char *fontfile,
                  const char *palfile)
{
    FILE *f;
    union REGPACK rp;
    unsigned short fseg, foff;
    static unsigned char buf[512];
    unsigned i, chunk;

    f = fopen(scrfile, "wb");
    if (!f) return 1;
    {
        unsigned short __far *s = screen();
        for (i = 0; i < 4000 / sizeof(buf); i++) {
            unsigned k;
            for (k = 0; k < sizeof(buf) / 2; k++)
                ((unsigned short *)buf)[k] = s[i * (sizeof(buf) / 2) + k];
            fwrite(buf, 1, sizeof(buf), f);
        }
        /* 4000 bytes = 7*512 + 416 */
        {
            unsigned rem = 4000 % sizeof(buf);
            unsigned base = (4000 / sizeof(buf)) * (sizeof(buf) / 2);
            unsigned k;
            for (k = 0; k < rem / 2; k++)
                ((unsigned short *)buf)[k] = s[base + k];
            fwrite(buf, 1, rem, f);
        }
    }
    fclose(f);

    /* int 10h AX=1130h BH=6 -> ES:BP = 8x16 font */
    memset(&rp, 0, sizeof(rp));
    rp.w.ax = 0x1130;
    rp.w.bx = 0x0600;
    intr(0x10, &rp);
    fseg = rp.w.es;
    foff = rp.w.bp;

    f = fopen(fontfile, "wb");
    if (!f) return 1;
    for (i = 0; i < 4096; i += chunk) {
        unsigned char __far *fp = (unsigned char __far *)MK_FP(fseg, foff + i);
        unsigned k;
        chunk = 512;
        for (k = 0; k < chunk; k++)
            buf[k] = fp[k];
        fwrite(buf, 1, chunk, f);
    }
    fclose(f);

    f = fopen(palfile, "wb");
    if (!f) return 1;
    if (vid_is_vga && !opt_nopal) {
        fwrite(synth_pal, 1, 48, f);
    } else {
        /* standard CGA/EGA colors, 6-bit */
        static const unsigned char std_pal[16][3] = {
            { 0, 0, 0}, { 0, 0,42}, { 0,42, 0}, { 0,42,42},
            {42, 0, 0}, {42, 0,42}, {42,21, 0}, {42,42,42},
            {21,21,21}, {21,21,63}, {21,63,21}, {21,63,63},
            {63,21,21}, {63,21,63}, {63,63,21}, {63,63,63},
        };
        fwrite(std_pal, 1, 48, f);
    }
    fclose(f);
    return 0;
}

/* largest free DOS memory block, in KB */
unsigned dos_free_kb(void)
{
    union REGS r;
    r.h.ah = 0x48;
    r.x.bx = 0xFFFF;            /* deliberately too big: BX <- largest */
    int86(0x21, &r, &r);
    return (unsigned)(((unsigned long)r.x.bx * 16UL) / 1024UL);
}
