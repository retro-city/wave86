# third-party/

The other people's programs the build needs, kept here so that building
WAVE86 or its PicoMem disk image does not depend on a retro-software
download site being up that day - they come and go. Each file's origin
and SHA-256 are in `SOURCES.txt`, kept by `tools/thirdparty.py`; the
terms each piece comes under are in `THIRD-PARTY.md` at the top of the
repository.

    picomem/    PM_D6_CONFIG.zip, the PicoMEM D6 release package, whole (the
                card's DOS tools in PICOMEM/, example CONFIG.SYS and
                AUTOEXEC.BAT, a DOSDRV/ folder of drivers and setup programs);
                drivers/, the card repository's drivers folder, whole (the
                tools, the Gravis UltraSound zip, SBCD/, TEST/); a program
                put straight into picomem/ (PMDFS.EXE, a fix from the author)
                replaces the package's copy of that name on the image
    mtcp/       Michael Brutman's mTCP client programs, the zip as released
    netdrive/   his mTCP NetDrive server, every platform in one zip
    cdmke/      CDMKE.SYS, the Panasonic/MKE CD-ROM driver (the PicoGUS copy)
    dosutils/   FreeDOS 1.3 packages: EDIT, MEM, MORE, XCOPY, DELTREE, ATTRIB,
                FIND, TREE, LABEL, HIMEMX, JEMM (JEMM386, JEMMEX, JLOAD), LBACACHE
    4dos/       4DOS 8.00, the official package
    edrdos/     the EDR-DOS kernel, SvarDOS's release zip
    slowdown/   SLOWDOWN 3.10, the COM and its DOC, from the FreeDOS package

The Makefile unpacks what a target needs into `build/` on first use
(`build/mtcp`, `build/4dos`, `build/picomem` = the D6 package's PICOMEM
folder plus `drivers/NE2000.COM`...), so a `make clean` costs no download.

Refreshing from upstream: `make update-third-party`, or one piece at a
time (`make update-mtcp`, `update-netdrive`, `update-picomem`,
`update-cdmke`, `update-gus`, `update-dosutils`, `update-4dos`,
`update-edrdos`, `update-slowdown`). A refresh that brings the same bytes
changes nothing; one that brings new ones replaces the file and its line
in `SOURCES.txt`, for you to look at and commit. A site that does not
answer leaves what is here, the other pieces are still tried, and make
ends with an error so you know. A new release usually has a new file name:
say `MTCP_VER=2026-01-01 make update-mtcp` (or `ND_VER=`, `EDR_VER=`,
`PICOMEM_REF=` for a branch or commit of the PicoMEM repository), then
delete the old archive. The PicoMEM tools are the exception: the D6
package is a release matched to the card's firmware, while
`update-picomem` takes the repository's `drivers/` folder, which can be
ahead of it (its PMINIT was Rev 1.0.2 when the D6 package's was 1.0.1);
a newer release package is put in `picomem/` by hand and recorded with
`tools/thirdparty.py add`. What is kept here is every file the
upstream folders hold; the terms of each are listed in `THIRD-PARTY.md`
as they stand, for review.

`make check-third-party` (CI runs it) says whether every recorded file is
there and unchanged, and names any file here that is not recorded.
