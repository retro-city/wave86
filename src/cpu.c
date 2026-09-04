/*
 * cpu.c - what are we running on? Vendor, model, clock and memory,
 * found from real mode with flag tests, CPUID and RDTSC where they
 * exist, the BIOS tick for timing, and the DMI table if the BIOS keeps
 * one. The 386+ instructions are written as byte sequences so the rest
 * of the program stays pure 8086 code; they only run after the flag
 * probe has shown the CPU can take them.
 */
#include <dos.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <i86.h>
#include "wave86.h"

char cpu_desc[32] = "";
int cpu_lvl = 0;

/* --------------------------------------------------- flag probes */

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

/*
 * Try to flip a bit in EFLAGS; returns the bits that actually changed.
 * Bit 18 (AC) flips on a 486, bit 21 (ID) flips when CPUID exists.
 *   shl edx,16 / movzx eax,ax / or eax,edx / mov ebx,eax
 *   pushfd / pop eax / mov ecx,eax / xor eax,ebx / push eax / popfd
 *   pushfd / pop eax / xor eax,ecx / and eax,ebx / push ecx / popfd
 *   mov edx,eax / shr edx,16
 */
unsigned long eflags_toggle(unsigned long mask);
#pragma aux eflags_toggle = \
    0x66 0xC1 0xE2 0x10 \
    0x66 0x0F 0xB7 0xC0 \
    0x66 0x09 0xD0      \
    0x66 0x89 0xC3      \
    0x66 0x9C           \
    0x66 0x58           \
    0x66 0x89 0xC1      \
    0x66 0x31 0xD8      \
    0x66 0x50           \
    0x66 0x9D           \
    0x66 0x9C           \
    0x66 0x58           \
    0x66 0x31 0xC8      \
    0x66 0x21 0xD8      \
    0x66 0x51           \
    0x66 0x9D           \
    0x66 0x89 0xC2      \
    0x66 0xC1 0xEA 0x10 \
    parm [dx ax] value [dx ax] modify [ax bx cx dx];

/*
 * CPUID leaf -> r[0..3] = eax, ebx, ecx, edx
 *   shl edx,16 / movzx eax,ax / or eax,edx / xor ecx,ecx / cpuid
 *   mov [di],eax / mov [di+4],ebx / mov [di+8],ecx / mov [di+12],edx
 */
void cpuid_raw(unsigned long leaf, unsigned long *r);
#pragma aux cpuid_raw = \
    0x66 0xC1 0xE2 0x10 \
    0x66 0x0F 0xB7 0xC0 \
    0x66 0x09 0xD0      \
    0x66 0x31 0xC9      \
    0x0F 0xA2           \
    0x66 0x89 0x05      \
    0x66 0x89 0x5D 0x04 \
    0x66 0x89 0x4D 0x08 \
    0x66 0x89 0x55 0x0C \
    parm [dx ax] [di] modify [ax bx cx dx];

/* rdtsc / mov [di],eax / mov [di+4],edx */
void rdtsc_read(unsigned long *lohi);
#pragma aux rdtsc_read = \
    0x0F 0x31           \
    0x66 0x89 0x05      \
    0x66 0x89 0x55 0x04 \
    parm [di] modify [ax dx];

/* 32 dependent "add ax,bx" then dec cx / jnz: a known cycle cost */
void calib_loop(unsigned iters);
#pragma aux calib_loop = \
    0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 \
    0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 \
    0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 \
    0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 0x01 0xD8 \
    0x49 0x75 0xBD      \
    parm [cx] modify [ax cx];

/* Cyrix leaves the flags alone after DIV; everyone else changes them */
unsigned char cyrix_probe(void);
#pragma aux cyrix_probe = \
    "xor  ax,ax"        \
    "sahf"              \
    "mov  ax,5"         \
    "mov  bx,2"         \
    "div  bl"           \
    "lahf"              \
    "mov  al,ah"        \
    value [al] modify [ax bx];

/* --------------------------------------------------- timing */

#define TICKS_4  219698UL           /* 4 BIOS ticks, in microseconds */

static volatile unsigned long __far *bios_ticks =
    (volatile unsigned long __far *)MK_FP(0x40, 0x6C);

static unsigned long wait_tick(void)
{
    unsigned long t = *bios_ticks;
    while (*bios_ticks == t)
        ;
    return *bios_ticks;
}

static unsigned tsc_mhz(void)
{
    unsigned long a[2], b[2], t0, d;
    t0 = wait_tick();
    rdtsc_read(a);
    while (*bios_ticks - t0 < 4)
        ;
    rdtsc_read(b);
    d = b[0] - a[0];                /* low dword is plenty below 19 GHz */
    return (unsigned)((d + TICKS_4 / 2) / TICKS_4);
}

/* count calibration loops over 4 ticks; cycles = cost of one iteration */
static unsigned loop_mhz(unsigned cycles)
{
    unsigned long t0 = wait_tick(), batches = 0;
    while (*bios_ticks - t0 < 4) {
        calib_loop(1000);
        batches++;
    }
    return (unsigned)((batches * cycles * 1000UL + TICKS_4 / 2) / TICKS_4);
}

/* an estimate within 6% of a clock that was actually sold becomes it */
static unsigned snap_mhz(unsigned m)
{
    static const unsigned common[] = {
        8, 10, 12, 16, 20, 25, 33, 40, 50, 60, 66, 75, 80, 90, 100, 120,
        133, 150, 166, 180, 200, 233, 266, 300, 333, 350, 366, 400, 450,
        500, 550, 600, 650, 700, 750, 800, 850, 900, 1000, 0
    };
    int i;
    for (i = 0; common[i]; i++) {
        unsigned c = common[i], tol = c * 6 / 100 + 1;
        if (m + tol >= c && m <= c + tol)
            return c;
    }
    return m;
}

/* --------------------------------------------------- DMI */

static int dmi_probe(char *name, unsigned namelen, unsigned *mhz)
{
    unsigned off;
    unsigned char __far *rom = (unsigned char __far *)MK_FP(0xF000, 0);
    unsigned long tbl = 0;
    unsigned len = 0;
    unsigned char __far *p, __far *end;

    for (off = 0; off < 0xFFF0; off += 16) {
        if (rom[off] == '_' && rom[off + 1] == 'S' && rom[off + 2] == 'M' &&
            rom[off + 3] == '_') {
            tbl = *(unsigned long __far *)(rom + off + 0x18);
            len = *(unsigned short __far *)(rom + off + 0x16);
            break;
        }
        if (rom[off] == '_' && rom[off + 1] == 'D' && rom[off + 2] == 'M' &&
            rom[off + 3] == 'I' && rom[off + 4] == '_') {
            tbl = *(unsigned long __far *)(rom + off + 0x08);
            len = *(unsigned short __far *)(rom + off + 0x06);
            break;
        }
    }
    if (!tbl || tbl >= 0x100000UL || !len)
        return 0;                   /* none, or beyond real mode's reach */

    p = (unsigned char __far *)MK_FP((unsigned)(tbl >> 4), (unsigned)(tbl & 15));
    end = p + len;
    while (p + 4 <= end) {
        unsigned char type = p[0], slen = p[1];
        unsigned char __far *q;
        if (type == 127 || slen < 4)
            break;
        q = p + slen;
        if (type == 4 && slen >= 0x18) {
            unsigned speed = *(unsigned short __far *)(p + 0x16);
            unsigned char idx = p[0x10];
            unsigned char __far *s = q;
            unsigned char n = 1;
            name[0] = 0;
            while (idx && *s) {
                if (n == idx) {
                    unsigned k = 0;
                    while (s[k] && k < namelen - 1) { name[k] = s[k]; k++; }
                    name[k] = 0;
                    break;
                }
                while (*s) s++;
                s++;
                n++;
            }
            *mhz = speed;
            return 1;
        }
        while (!(q[0] == 0 && q[1] == 0) && q + 1 < end)
            q++;
        p = q + 2;
    }
    return 0;
}

/* --------------------------------------------------- naming */

static const char *vendor_short(const char *v)
{
    if (!strcmp(v, "GenuineIntel")) return "INTEL";
    if (!strcmp(v, "AuthenticAMD")) return "AMD";
    if (!strcmp(v, "CyrixInstead")) return "CYRIX";
    if (!strcmp(v, "CentaurHauls")) return "IDT";
    if (!strcmp(v, "NexGenDriven")) return "NEXGEN";
    if (!strcmp(v, "UMC UMC UMC ")) return "UMC";
    if (!strcmp(v, "RiseRiseRise")) return "RISE";
    if (!strcmp(v, "GenuineTMx86")) return "TRANSMETA";
    if (!strcmp(v, "SiS SiS SiS ")) return "SIS";
    if (!strcmp(v, "Geode by NSC")) return "GEODE";
    if (!strncmp(v, "VIA", 3)) return "VIA";
    return "";
}

static const char *model_name(const char *v, int family, int model)
{
    if (!strcmp(v, "GenuineIntel")) {
        if (family == 4) switch (model) {
            case 0: case 1: return "486DX";
            case 2: return "486SX";
            case 3: case 7: return "486DX2";
            case 4: return "486SL";
            case 5: return "486SX2";
            case 8: case 9: return "486DX4";
        }
        if (family == 5) return model >= 4 ? "PENTIUM MMX" : "PENTIUM";
        if (family == 6) switch (model) {
            case 0: case 1: return "PENTIUM PRO";
            case 3: case 5: return "PENTIUM II";
            case 6: return "CELERON";
            case 7: case 8: case 10: case 11: return "PENTIUM III";
            case 9: case 13: return "PENTIUM M";
        }
        if (family == 15) return "PENTIUM 4";
    } else if (!strcmp(v, "AuthenticAMD")) {
        if (family == 4) switch (model) {
            case 3: case 7: return "AM486DX2";
            case 8: case 9: return "AM486DX4";
            case 14: case 15: return "AM5X86";
        }
        if (family == 5) switch (model) {
            case 0: case 1: case 2: case 3: return "K5";
            case 6: case 7: return "K6";
            case 8: return "K6-2";
            case 9: return "K6-III";
            case 13: return "K6-2+";
        }
        if (family == 6) return model == 3 ? "DURON" : "ATHLON";
        if (family == 15) return "ATHLON 64";
    } else if (!strcmp(v, "CyrixInstead")) {
        if (family == 4) return model == 9 ? "5X86" : "CX486";
        if (family == 5) return model == 4 ? "MEDIAGX" : "6X86";
        if (family == 6) return "6X86MX";
    } else if (!strcmp(v, "CentaurHauls")) {
        if (family == 5) return model >= 8 ? "WINCHIP 2" : "WINCHIP C6";
    }
    return NULL;
}

/* brand/DMI strings come as "AMD-K6(tm) 3D processor" and such */
static void clean_name(char *dst, unsigned dstlen, const char *src)
{
    static const char *junk[] = { "(tm)", "(TM)", "(r)", "(R)", "processor",
                                  "Processor", "PROCESSOR", "CPU", NULL };
    char tmp[64];
    char *s, *w;
    int i;

    strncpy(tmp, src, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (i = 0; junk[i]; i++) {
        while ((s = strstr(tmp, junk[i])) != NULL)
            memmove(s, s + strlen(junk[i]), strlen(s + strlen(junk[i])) + 1);
    }
    /* drop "800MHz"-style tokens: we measure that ourselves */
    for (s = tmp; *s; ) {
        char *e = s;
        while (*e && *e != ' ') e++;
        if (e - s > 3 && (!strncmp(e - 3, "MHz", 3) || !strncmp(e - 3, "GHz", 3) ||
                          !strncmp(e - 3, "MHZ", 3))) {
            memmove(s, e, strlen(e) + 1);
        } else {
            s = e;
        }
        while (*s == ' ') s++;
    }
    /* squeeze spaces, uppercase */
    for (s = tmp, w = dst; *s && (unsigned)(w - dst) < dstlen - 1; s++) {
        if (*s == ' ' && (w == dst || w[-1] == ' '))
            continue;
        *w++ = (char)toupper((unsigned char)*s);
    }
    while (w > dst && w[-1] == ' ') w--;
    *w = 0;
}

/* --------------------------------------------------- memory */

static unsigned memory_mb(int lvl)
{
    union REGS r;
    unsigned long kb = 0;
    int got = 0;

    if (lvl >= 3) {
        r.x.ax = 0xE801;
        int86(0x15, &r, &r);
        if (!r.x.cflag) {
            unsigned lo = r.x.ax ? r.x.ax : r.x.cx;
            unsigned hi = r.x.ax ? r.x.bx : r.x.dx;
            if (lo || hi) {
                kb = (unsigned long)lo + (unsigned long)hi * 64UL;
                got = 1;
            }
        }
    }
    if (!got && lvl >= 2) {
        r.h.ah = 0x88;
        int86(0x15, &r, &r);
        if (!r.x.cflag && r.x.ax != 0xFFFF)
            kb = r.x.ax;
    }
    return (unsigned)((kb + 1024UL + 512UL) / 1024UL);   /* plus the base megabyte */
}

/* --------------------------------------------------- the answer */

void cpu_identify(void)
{
    char name[40] = "";
    char vendor[13] = "";
    unsigned mhz = 0, dmi_mhz = 0;
    int exact = 0, lvl, family = 0, model = 0, has_tsc = 0;
    unsigned long r[4];
    unsigned mem;
    char dmi_name[40] = "";
    int have_dmi;

    lvl = cpu_level();
    cpu_lvl = lvl;

    if (lvl == 0) {
        strcpy(name, "8086/8088");
    } else if (lvl == 2) {
        strcpy(name, "80286");
        mhz = snap_mhz(loop_mhz(74));
    } else {
        int is486 = eflags_toggle(0x40000UL) != 0;
        int has_cpuid = eflags_toggle(0x200000UL) != 0;
        family = is486 ? 4 : 3;

        if (has_cpuid) {
            unsigned long maxleaf;
            cpuid_raw(0, r);
            maxleaf = r[0];
            memcpy(vendor, &r[1], 4);
            memcpy(vendor + 4, &r[3], 4);
            memcpy(vendor + 8, &r[2], 4);
            vendor[12] = 0;
            if (maxleaf >= 1) {
                cpuid_raw(1, r);
                family = (int)((r[0] >> 8) & 0xF);
                model = (int)((r[0] >> 4) & 0xF);
                if (family == 15)
                    family += (int)((r[0] >> 20) & 0xFF);
                has_tsc = (r[3] & 0x10) != 0;
            }
            cpuid_raw(0x80000000UL, r);
            if (r[0] >= 0x80000004UL) {
                char brand[49];
                int i;
                for (i = 0; i < 3; i++) {
                    cpuid_raw(0x80000002UL + i, r);
                    memcpy(brand + i * 16, r, 16);
                }
                brand[48] = 0;
                clean_name(name, sizeof(name), brand);
            }
            if (!name[0]) {
                const char *m = model_name(vendor, family, model);
                if (m)
                    sprintf(name, "%s %s", vendor_short(vendor), m);
                else
                    sprintf(name, "%s FAMILY %d", vendor_short(vendor), family);
            }
        } else if (is486) {
            strcpy(name, cyrix_probe() == 2 ? "CYRIX 486" : "80486");
        } else {
            strcpy(name, "80386");
        }

        have_dmi = dmi_probe(dmi_name, sizeof(dmi_name), &dmi_mhz);
        if (have_dmi && !has_cpuid && dmi_name[0])
            clean_name(name, sizeof(name), dmi_name);

        if (has_tsc) {
            mhz = tsc_mhz();
            exact = 1;
        } else if (have_dmi && dmi_mhz && dmi_mhz < 2000) {
            mhz = dmi_mhz;
            exact = 1;
        } else {
            mhz = snap_mhz(loop_mhz(family >= 4 ? 36 : 74));
        }
    }

    mem = memory_mb(lvl);
    if (mhz)
        sprintf(cpu_desc, "%s %s%uMHZ", name, exact ? "" : "~", mhz);
    else
        strcpy(cpu_desc, name);
    if (mem && strlen(cpu_desc) + 6 < sizeof(cpu_desc))
        sprintf(cpu_desc + strlen(cpu_desc), " %uMB", mem);
    cpu_desc[30] = 0;
}
