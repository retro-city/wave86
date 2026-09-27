WAVE86 on a PicoMEM 2 - the ready-made disk
===========================================

pmwave-edrdos.img is a 512 MB hard-disk image for a PC with a PicoMEM 2
card. It boots EDR-DOS straight into the WAVE86 launcher, with the card's
sound cards, its SD card as drive W:, its emulated CD-ROM and its Wi-Fi
network set up.

On the SD card
--------------
The SD card (SDHC; FAT16, FAT32 or exFAT), from its root:

    HDD\PMWAVE-EDRDOS.IMG   this image (the name before .img may have 13
                            characters at most)
    EXODOS\                 the games: WAVE86 installs them here
                            (gamedir=W:\EXODOS)
    CDROM\                  the disc images (cdrom_storage=W:\CDROM)
    wifi.txt                the Wi-Fi: the network's name on line 1, its
                            password on line 2

Then in the PicoMEM BIOS Setup (S at the card's "Press S for Setup"
prompt): Disk menu, HDD0 = this image, and PicoMEM Boot Code on; Other
menu, NE2000 on at port 300, IRQ 3. The jumpers on the board: IRQ 7 (the
card's own) and DMA 1 (Sound Blaster, GUS).

The boot menu
-------------
Ten seconds, then 1:

    1  PicoMEM - Standard Mode (CD, DFS and Network)
    2  PicoMEM - Local Mode (CD and DFS)
    3  PicoMEM - Network Mode
    4  PicoMEM - Memory Optimized
    5  Safe Mode

1 loads everything: HIMEMX and JEMM386, the sound cards (PMINIT), the SD
card as W: (PMDFS), the CD-ROM as D:, the mouse, the network (PM2000 and
DHCP) and NetDrive. 2 leaves the network out. 3 is for playing games
straight off the server: the launcher opens on the network view, Enter
plays a game off the server, and discs stay on the server. 4 gives the
most memory to a game: no drivers but the sound cards and W:. 5 loads
nothing and stops at a prompt.

EDR-DOS shows no countdown; the menu says the default and the time.

The server
----------
The games come from a WAVE86 server on the same network - the
wave86-x.x-server.zip from the same release, with its own README. In
C:\WAVE86\WAVE86.INI, server= names that machine:

    server=192.168.1.20:8086

or press N in the launcher and type the address when it asks.

Also on the image
-----------------
    C:\WAVE86      the launcher, WAVEGET, and the INI for this machine
    C:\PICOMEM     the card's DOS tools (PMINIT, PMDFS, PM2000...) and
                   CDMKE.SYS
    C:\MTCP        Michael Brutman's mTCP programs (FTP, Telnet, Ping...)
    C:\ULTRASND    the Gravis UltraSound software
    C:\DOS         FreeDOS utilities (EDIT, MEM, XCOPY...) and the memory
                   managers

CONFIG.SYS and AUTOEXEC.BAT follow the ones the card's author ships with
the card's D6 release, and every number (IRQs, ports) is written into the
command that uses it.

THIRD-PARTY.md lists every program on the image that is not WAVE86's,
with where it came from and under what terms.
