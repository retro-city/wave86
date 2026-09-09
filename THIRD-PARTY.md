# Third-party work in this repository

WAVE86 itself (the launcher in `src/`, the tools in `tools/`, WAVEGET, DRVOFF,
the soundtrack in `music/`) is under the GPL, version 3 or later, in `LICENSE`.
It ships with other people's programs, each under its own terms:

| What | Where | Licence | Origin |
| --- | --- | --- | --- |
| mTCP 2025-01-10, Michael Brutman: the TCP/IP library WAVEGET and DHCP are built on, the DHCP client, NetDrive's DOS driver and utility (`NETDRIVE.SYS`, `NETDRIVE.EXE`) | `net/mtcp`, `net/dhcp`, `net/NETDRIVE.*` | GPL v3 (`net/mtcp/COPYING.TXT`) | http://www.brutman.com/mTCP/ ; `net/mtcp/TCPLIB/UTILS.CPP` is altered (the far-heap check is skipped, see the file) |
| mTCP NetDrive server (Go) | not in the repo; `make netdrive` fetches the official build into `build/` | GPL v3 | http://www.brutman.com/mTCP/mTCP_NetDrive.html |
| SHSUCD suite 3.09/3.01, Jason Hood: SHSUCDX (MSCDEX replacement) and SHSUCDHD (image file as a CD-ROM) | `cdrom/` | zlib-style (`cdrom/LICENSE.txt`) | https://github.com/adoxa/shsucd, commit 5ea0787, assembled with NASM |
| Crynwr NE2000 packet driver 11.4.3 | `net/NE2000.COM` | GPL | Crynwr Software; the binary is the one dosbox-x carries (`src/builtin/ne2000bin.cpp`); sources at http://crynwr.com/drivers/ |
| FreeDOS 1.3: kernel 2043 and FreeCOM 0.85a on a boot floppy | `dos/FREEDOS.IMG` | GPL v2 | https://github.com/FDOS/kernel, https://github.com/FDOS/freecom; the floppy is `144m/x86BOOT.img` of the Floppy Edition reduced to kernel and shell, see `dos/README.md` |
| FreeDOS CHOICE 4.4a | `dos/CHOICE.EXE` | GPL v2 | FreeDOS 1.3 package repository, `base/choice.zip` |
| CuteMouse 2.1b4 | `dos/CTMOUSE.EXE` | GPL v2 | FreeDOS 1.3 package repository, `base/ctmouse.zip` |
| JEMM 5.84 (JEMMEX), Japheth, Tom Ehlert, Michael Devore | `dos/JEMMEX.EXE` | Artistic License | FreeDOS 1.3 package repository, `base/jemm.zip` |

Not included, read from your own copies: the eXoDOS collection and the Total
DOS Collection (the server indexes them where you keep them), MS-DOS boot
disks (point `make dostest DOS=` at yours), and the Open Watcom V2 toolchain
(`toolchain/`, see the README).

The DRVOFF trick of reserving two NetDrive letters and freeing D: is
DivByZero's, from the NetDrive thread on VOGONS.
