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
