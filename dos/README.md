# Boot floppy for `make dostest`

`FREEDOS.IMG` is a 1.44 MB floppy that boots FreeDOS to a prompt. It is
the boot disk of the FreeDOS 1.3 Floppy Edition (`144m/x86BOOT.img`,
released 2022-02-20, from
https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip)
reduced to what a test needs:

    KERNEL.SYS      FreeDOS kernel 2043 (2021-05-14), GPL v2
    COMMAND.COM     FreeCOM 0.85a (2021-07-10), GPL v2
    FDCONFIG.SYS    FILES/BUFFERS/LASTDRIVE and the SHELL line
    AUTOEXEC.BAT    a prompt

The image was made with mtools: `mformat -C -f 1440 -B <boot sector of
x86BOOT.img>` and `mcopy` of the four files, so the FreeDOS boot sector
is the original and the rest of the disk is zeros. Sources:
https://github.com/FDOS/kernel and https://github.com/FDOS/freecom.

`tools/dostest.py` replaces AUTOEXEC.BAT and FDCONFIG.SYS on a copy of
this image for each run. MS-DOS is Microsoft's and stays out of the
repo; point `make dostest DOS=` at your own boot floppy to test on it.

`CHOICE.EXE` is FreeDOS's CHOICE 4.4 (GPL, from the FreeDOS 1.3 package
repository, `base/choice.zip`). Neither boot floppy carries one, and the
eXoDOS start batches use it for their menus, so the harness puts it in
`C:\WAVE86`, which is on the PATH. A real DOS install has its own.

`CTMOUSE.EXE` is CuteMouse, FreeDOS's mouse driver (GPL, from the same
package repository, `base/ctmouse.zip`). The harness loads it from the
AUTOEXEC of both images, so games in `make dosrun` have a mouse; dosbox-x
gives it a PS/2 one.

`JEMMEX.EXE` is JEMM 5.84's all-in-one memory manager (XMS, EMS and
upper memory; Artistic License, from the same repository, `base/jemm.zip`).
The harness loads it first in CONFIG.SYS with `DOS=HIGH,UMB`, puts
NetDrive up with DEVICEHIGH and the packet driver, the mouse and the CD
drivers with LH, so the games get the conventional memory back. It works
under MS-DOS and FreeDOS alike.
