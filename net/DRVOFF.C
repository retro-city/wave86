/*
 * DRVOFF X: - take a drive letter out of use.
 *
 * DOS hands block device drivers their letters in CONFIG.SYS order, so
 * mTCP NetDrive always lands on the first letter after the hard disk,
 * D:, which is where CD games expect their disc. The way round it (an
 * idea from DivByZero on VOGONS): let NETDRIVE.SYS reserve two letters
 * (-d:2, so D: and E:), then mark D: invalid with this before SHSUCDX or
 * MSCDEX runs, and the disc gets D: while NetDrive uses E:.
 *
 * The letter's entry in DOS's Current Directory Structure gets its flags
 * cleared, which is what "not a valid drive" means to DOS 4 and later,
 * MS-DOS 6.22 and FreeDOS included. Built like the launcher: 8086, small
 * model.
 */
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int main(int argc, char **argv)
{
    union REGS r;
    struct SREGS s;
    unsigned char __far *sysvars;
    unsigned char __far *cds;
    unsigned char __far *entry;
    unsigned lastdrive, size;
    int d;

    if (argc < 2 || argv[1][1] != ':' || !isalpha((unsigned char)argv[1][0])) {
        puts("DRVOFF X:  - take drive letter X out of use (for a NetDrive letter the CD needs)");
        return 1;
    }
    if (_osmajor < 4) {
        puts("DRVOFF needs DOS 4 or later.");
        return 1;
    }
    d = toupper((unsigned char)argv[1][0]) - 'A';
    segread(&s);
    r.h.ah = 0x52;                          /* get list of lists: ES:BX */
    intdosx(&r, &r, &s);
    sysvars = (unsigned char __far *)MK_FP(s.es, r.x.bx);
    cds = *(unsigned char __far * __far *)(sysvars + 0x16);
    lastdrive = sysvars[0x21];
    size = 0x58;                            /* CDS entry, DOS 4+ */
    if ((unsigned)d >= lastdrive) {
        printf("%c: is beyond LASTDRIVE.\n", 'A' + d);
        return 1;
    }
    entry = cds + d * size;
    if ((*(unsigned __far *)(entry + 0x43) & 0xC000) == 0) {
        printf("%c: is not in use.\n", 'A' + d);
        return 0;
    }
    *(unsigned __far *)(entry + 0x43) = 0;  /* flags: neither physical nor network */
    printf("%c: taken out of use.\n", 'A' + d);
    return 0;
}
