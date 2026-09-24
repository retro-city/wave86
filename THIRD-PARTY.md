# Third-party work in this repository

WAVE86 itself (the launcher in `src/`, the tools in `tools/`, WAVEGET, DRVOFF,
the soundtrack in `music/`) is under the GPL, version 3 or later, in `LICENSE`.
It ships with other people's programs, each under its own terms:

| What | Where | Licence | Origin |
| --- | --- | --- | --- |
| mTCP 2025-01-10, Michael Brutman: the TCP/IP library WAVEGET and DHCP are built on, the DHCP client, NetDrive's DOS driver and utility (`NETDRIVE.SYS`, `NETDRIVE.EXE`) | `net/mtcp`, `net/dhcp`, `net/NETDRIVE.*` | GPL v3 (`net/mtcp/COPYING.TXT`) | http://www.brutman.com/mTCP/ ; `net/mtcp/TCPLIB/UTILS.CPP` is altered (the far-heap check is skipped, see the file) |
| mTCP NetDrive server (Go) | not in the repo; `make netdrive` fetches the official build into `build/` | GPL v3 | http://www.brutman.com/mTCP/mTCP_NetDrive.html |
| SHSUCD suite 3.09/3.01, Jason Hood: SHSUCDX (MSCDEX replacement) and SHSUCDHD (image file as a CD-ROM) | `cdrom/` | zlib-style (`cdrom/LICENSE.txt`) | https://github.com/adoxa/shsucd, commit 5ea0787, assembled with NASM |
| SLOWDOWN 3.10, Bret Johnson: slows the machine for a game that runs too fast (`slowdown=` in a game's section) | not in the repo; `make slowdown` fetches `SLOWDOWN.COM` and `SLOWDOWN.DOC` into `build/` (and `dist/`), from the FreeDOS package | freeware: the COM and its DOC together and unmodified, no fee (`SLOWDOWN.DOC`, "A word from the sponsor"); bundling with another program wants the author's word, hence fetched, not kept | https://bretjohnson.us/ (slodn310.zip); https://gitlab.com/FreeDOS/util/slowdown |
| EDR-DOS (Enhanced DR-DOS, the SvarDOS kernel), for `make picomem-image KERNEL=edrdos` | not in the repo; the build fetches the release zip into `build/edrdos` | a 2022 grant from DRDOS, Inc. ("use, distribute, modify... CP/M and its derivatives") published in the repository, with Caldera's 1997 OpenDOS terms (redistribution for non-commercial purposes) still in the tree; not OSI-approved, not for commercial use | https://github.com/SvarDOS/edrdos/releases |
| 4DOS 8.00, JP Software / Rex Conn: the shell on the PicoMem disk image | not in the repo; `make picomem-image` fetches the official package into `build/4dos` and ships `LICENSE.TXT` with it in `C:\4DOS` | JP Software's 2004 notice licence (use, copy, modify, distribute for any purpose without fee, the notice kept with all copies and changes noted); not OSI-approved | https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/util/user/4dos/4dos800.zip ; https://4dos.info/ |
| FreeDOS utilities (EDIT, LBACACHE, MEM, MORE, XCOPY, DELTREE, ATTRIB, FIND, TREE, LABEL) for the PicoMem disk image's C:\DOS | not in the repo; `make picomem-image` fetches the packages into `build/dosutils` | GPL (each package's `DOC/` folder) | https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/ |
| Crynwr NE2000 packet driver 11.4.3 | `net/NE2000.COM` | GPL | Crynwr Software; the binary is the one dosbox-x carries (`src/builtin/ne2000bin.cpp`); sources at http://crynwr.com/drivers/ |
| FreeDOS 1.3: kernel 2043 and FreeCOM 0.85a on a boot floppy | `dos/FREEDOS.IMG` | GPL v2 | https://github.com/FDOS/kernel, https://github.com/FDOS/freecom; the floppy is `144m/x86BOOT.img` of the Floppy Edition reduced to kernel and shell, see `dos/README.md` |
| FreeDOS CHOICE 4.4a | `dos/CHOICE.EXE` | GPL v2 | FreeDOS 1.3 package repository, `base/choice.zip` |
| CuteMouse 2.1b4 | `dos/CTMOUSE.EXE` | GPL v2 | FreeDOS 1.3 package repository, `base/ctmouse.zip` |
| JEMM 5.84 (JEMMEX), Japheth, Tom Ehlert, Michael Devore | `dos/JEMMEX.EXE` | Artistic License | FreeDOS 1.3 package repository, `base/jemm.zip` |
| FreeDOS kernel boot sector, FAT16 build of `boot/boot.asm` (Svante Frey and the kernel's authors) | `dos/FAT16.BS`, with its source `dos/boot.asm` and `dos/magic.mac` (the latter public domain, E. C. Masloch) | GPL v2 | https://github.com/FDOS/kernel, commit d6791ad; assembled with NASM, see `dos/README.md` |
| PicoMEM DOS tools (PMINIT, PMDFS, PMDFS3, PM2000, NE2000, PMMOUSE, PMEMM, ASTCLOCK, PICOMEM.EXE), FreddyV | not in the repo; `make picomem-image` fetches them into `build/picomem` for the disk image | mixed: the repository's LICENSE is GPL v2 for FreddyV's own programs, but NE2000.COM is Crynwr's packet driver, PMMOUSE is CuteMouse-derived and PMDFS is EtherDFS-derived, each under its own terms | https://github.com/FreddyVRetro/ISA-PicoMEM, `drivers/` |
| mTCP 2025-01-10 client programs (DHCP, FTP, HTGet, NetDrive, Ping, Telnet...) | not in the repo; `make picomem-image` fetches the zip into `build/mtcp` | GPL v3 | https://www.brutman.com/mTCP/ |
| CDMKE.SYS 4.12, the Panasonic/MKE CD-ROM driver (Matsushita-Kotobuki Electronics) | not in the repo; `make picomem-image` fetches it into `build/cdmke` unless `CDMKE_URL=` is empty | proprietary, passed around by the PicoGUS project | https://picogus.com/drivers/cdmke.zip |
| Gravis UltraSound software 4.11 (ULTRASND.INI, ULTRINIT, ULTRAMID, the MIDI patches...), Advanced Gravis | not in the repo; `make picomem-image` fetches the PicoMEM repository's `ultrasnd.zip` into `build/ultrasnd` unless `GUS_URL=` is empty (`GUS_DIR=` uses your own copy) | proprietary, passed around by the PicoMEM and PicoGUS projects | https://github.com/FreddyVRetro/ISA-PicoMEM, `drivers/ultrasnd.zip` |

Not included, read from your own copies: the eXoDOS collection and the Total
DOS Collection (the server indexes them where you keep them, or reads the
collection's torrent), MS-DOS or PC DOS disks (`make dostest DOS=` boots
yours, `make picomem-image DOS_DISKS=` builds the disk image on them), and
the Open Watcom V2 toolchain (`toolchain/`, see the README).

The DRVOFF trick of reserving two NetDrive letters and freeing D: is
DivByZero's, from the NetDrive thread on VOGONS.
