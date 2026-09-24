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

# Boot sector for `make picomem-image`

`FAT16.BS` is the FreeDOS FAT16 boot sector that `tools/mkimage.py`
writes onto the hard-disk image it builds, and `boot.asm` and `magic.mac`
are its source: `boot/boot.asm` and `boot/magic.mac` of
https://github.com/FDOS/kernel at commit d6791ad (2026-07-01; boot.asm
itself last changed 2024-02-18), GPL v2 like the kernel (magic.mac is
public domain, E. C. Masloch). `make bootsector` reassembles it with
`nasm -dISFAT16 -f bin`; the FAT12 build on FREEDOS.IMG is a different
binary (the FAT chain walk differs), so it could not simply be reused.
dosbox-x's IMGMAKE, which makes the image, writes a partition with a
"not bootable" stub; mkimage.py puts this sector over it, keeping the
BPB IMGMAKE wrote (bytes 0x0B-0x3D), with OEM name `FRDOS5.1` and BIOS
drive 80h, which is what the kernel's SYS.COM does. The sector finds
KERNEL.SYS by searching the root directory and reads through INT 13h
extensions when the BIOS has them, CHS from the BPB otherwise, which is
what the PicoMem's BIOS (DOSBox's, CHS only) will get.
