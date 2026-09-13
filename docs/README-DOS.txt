WAVE86 - the DOS side
=====================

A game launcher for MS-DOS: 8086 and up, VGA best, AdLib or Sound
Blaster for the music. Full notes: https://github.com/retro-city/wave86

Setting up
----------
1. Copy this folder to the DOS machine, say C:\WAVE86, and put it on the
   PATH in AUTOEXEC.BAT.
2. Put each game in its own folder under C:\GAMES (gamedir= in WAVE86.INI
   changes that).
3. Type WAVE. Always start it that way, not WAVE86.EXE: the batch file is
   what hands a game every byte of memory.

WAVE86 /diag prints what it found when something looks wrong.

Games from a server on the LAN (PicoMem or any NE2000)
------------------------------------------------------
DOS's default environment (256 bytes) runs out once MTCPCFG joins PATH,
BLASTER and the rest ("Out of environment space"); give COMMAND.COM more
in CONFIG.SYS:
    SHELL=C:\COMMAND.COM C:\ /E:1024 /P

On the DOS machine, in AUTOEXEC.BAT:
    LH NE2000 0x60 5 0x300          (your card's interrupt and port)
    SET MTCPCFG=C:\WAVE86\MTCP.CFG
    C:\WAVE86\DHCP
and in WAVE86.INI:
    server=192.168.1.10:8086        (the machine running waveserve.py)
Then N in the menu lists the games there; I installs one, Enter plays
it straight off the server.

CD images kept on the server (NetDrive) also need, in CONFIG.SYS:
    DEVICE=C:\WAVE86\EXTRAS\JEMMEX.EXE     (or HIMEM.SYS plus EMM386.EXE)
    DOS=HIGH,UMB
    DEVICEHIGH=C:\WAVE86\NETDRIVE.SYS -d:2
and, first thing in AUTOEXEC.BAT:
    DRVOFF D:
That keeps D: for the disc and gives NetDrive E:.

Discs
-----
A game installed with its CD (LOCAL CD in the network view) keeps the
image in its own CD\ folder, and SHSUCDHD + SHSUCDX put it on D: while
the game runs. WAVE86.INI can name one folder for every disc instead,
and hand the mounting to a card that emulates a CD-ROM drive itself -
a PicoGUS, and a PicoMem 2 once its firmware can load an image:
    cdrom=W:                  (where the images go; W: here is the PicoGUS's)
    imgmount=PICOGUS          (SOFTWARE, the default, uses SHSUCDHD+SHSUCDX)
    cdletter=D                (the letter MSCDEX gave the card's drive)
    cdmount_picogus=C:\PICOGUS\PGUSINIT.EXE /cdload
The image is appended to that command; cdname=1 appends its bare file
name instead of the whole path. PGUSINIT.EXE /cdload is the default, so
cdmount_picogus= is only needed to give PGUSINIT's path.

Updating over the network
-------------------------
U in the network view fetches a fresh WAVE86.EXE, WAVEGET.EXE and
DRVOFF.EXE from the server and starts the new launcher - no floppy
shuffle when testing a build on the real machine. The same thing from
the prompt:
    WAVEGET UPDATE 192.168.1.10:8086 C:\WAVE86
Each file is renamed into place only once it has arrived whole, so a
dropped connection leaves what is already there alone. The server sends
what it built; anything else it should push (a WAVE86.INI of your own,
drivers) goes in its --update folder.

EXTRAS
------
FreeDOS programs a DOS install may lack: CHOICE.EXE (menus in some start
batches), CTMOUSE.EXE (a mouse driver), JEMMEX.EXE (memory manager).

Licences
--------
WAVE86 is free software under the GPL, version 3 or later (LICENSE.TXT).
The other programs here have their own terms: THIRDPTY.TXT lists them,
SHSUCD.TXT is the CD drivers' licence.
