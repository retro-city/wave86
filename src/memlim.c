/*
 * MEMLIM - hide extended memory from a game that cannot cope with all
 * of it (Aladdin compares XMS requests against the largest free block
 * with a signed 16-bit jump, so anything from 32 MB up "fails").
 *
 *   MEMLIM 24      allocate XMS until the largest free block, and the
 *                  total, are 24 MB or less; the handles go to
 *                  MEMLIM.DAT next to this program
 *   MEMLIM /FREE   give them back (nothing to do is fine)
 *
 * WAVE86 runs the first before a game whose section says memlimit=24
 * and the second after it - and at the top of every batch, for a game
 * that never got to its own end. XMS 2.0 calls only, so it runs on an
 * 8086; a driver that reports at most 65535 KB is handled by asking
 * again. MEMLIM.DAT keeps each handle with the size of its block, and
 * /FREE gives back only a handle that still has a block of that size
 * and no lock on it: handle numbers are reused from one boot to the
 * next, and a file left by a game that took the machine down must not
 * free what SMARTDRV or a RAM disk holds now. Exit code 0, or 1 without
 * an XMS driver, 2 when a block could not be taken. Part of WAVE86,
 * GPL v3 or later.
 */
#include <dos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void (__far *xms)(void);
static unsigned r_ax, r_dx, r_bl, r_bh;

static int xms_find(void)
{
    union REGS r;
    struct SREGS s;
    r.x.ax = 0x4300;
    int86(0x2F, &r, &r);
    if (r.h.al != 0x80)
        return 0;
    r.x.ax = 0x4310;
    segread(&s);
    int86x(0x2F, &r, &r, &s);
    xms = (void (__far *)(void))MK_FP(s.es, r.x.bx);
    return 1;
}

static void xms_call(unsigned char fn, unsigned kb)
{
    _asm {
        mov ah, fn
        mov dx, kb
        call dword ptr [xms]
        mov r_ax, ax
        mov r_dx, dx
        mov al, bh
        xor ah, ah
        mov r_bh, ax
        mov al, bl
        mov r_bl, ax
    }
}

static void dat_path(char *dst, const char *argv0)
{
    const char *e = strrchr(argv0, '\\');
    if (!e) e = strrchr(argv0, ':');
    if (e) {
        unsigned n = (unsigned)(e - argv0) + 1;
        memcpy(dst, argv0, n);
        dst[n] = 0;
    } else
        dst[0] = 0;
    strcat(dst, "MEMLIM.DAT");
}

static int do_free(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[40];
    int n = 0, stale = 0;
    if (!f)
        return 0;
    while (fgets(buf, sizeof(buf), f)) {
        unsigned h, kb = 0;
        int got = sscanf(buf, "%u %u", &h, &kb);
        if (got < 1)
            continue;
        xms_call(0x0E, h);              /* handle information: lock count in BH, its size in DX */
        if (r_ax != 1 || r_bh != 0 || (got == 2 && r_dx != kb)) {
            stale++;                    /* not the block we took: a reboot has been */
            continue;
        }
        xms_call(0x0A, h);
        if (r_ax == 1)
            n++;
    }
    fclose(f);
    remove(path);
    printf("MEMLIM: %d block%s of extended memory given back%s\n", n, n == 1 ? "" : "s",
           stale ? " (a note from before a reboot: left alone)" : "");
    return 0;
}

int main(int argc, char **argv)
{
    char path[128];
    unsigned long limit;
    unsigned handles[16], sizes[16];
    int n = 0, i;
    FILE *f;

    if (argc < 2) {
        puts("MEMLIM <MB>   hide extended memory beyond MB from the next program\n"
             "MEMLIM /FREE  give it back");
        return 0;
    }
    if (!xms_find()) {
        puts("MEMLIM: no XMS driver (HIMEM, JEMMEX)");
        return 1;
    }
    dat_path(path, argv[0]);
    if (argv[1][0] == '/' || argv[1][0] == '-')
        return do_free(path);
    do_free(path);                      /* a leftover from a game that crashed */
    limit = (unsigned long)atoi(argv[1]) * 1024UL;
    while (n < 16) {
        unsigned long largest, total, take;
        xms_call(0x08, 0);
        largest = r_ax;
        total = r_dx;
        if (largest > limit)
            take = largest - limit;
        else if (total > limit)
            take = total - limit < largest ? total - limit : largest;
        else
            break;
        if (take == 0)
            break;
        if (take > 65535UL)
            take = 65535UL;
        xms_call(0x09, (unsigned)take);
        if (r_ax != 1) {
            printf("MEMLIM: could not take %luK (XMS error %02Xh)\n", take, r_bl);
            break;
        }
        handles[n] = r_dx;
        sizes[n++] = (unsigned)take;
    }
    if (n) {
        f = fopen(path, "w");
        if (f) {
            for (i = 0; i < n; i++)
                fprintf(f, "%u %u\n", handles[i], sizes[i]);
            fclose(f);
        }
    }
    xms_call(0x08, 0);
    printf("MEMLIM: the largest free block of extended memory is now %uK, %luK in all (%d block%s taken)\n",
           r_ax, (unsigned long)r_dx, n, n == 1 ? "" : "s");
    return r_ax > limit ? 2 : 0;
}
