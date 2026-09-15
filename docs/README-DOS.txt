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
    cdrom_storage=W:          (where the images go; W: here is the PicoGUS's)
    imgmount=PICOGUS          (SOFTWARE, the default, uses SHSUCDHD+SHSUCDX)
    cdrom_letter=D            (the letter MSCDEX gave the card's drive)
    cdmount_picogus=C:\PICOGUS\PGUSINIT.EXE /cdloadname
The image is appended to that command - /cdloadname takes a name, while
/cdload takes the number of an image - and cdrom_name=1 appends the bare
file name instead of the whole path. The card needs a moment before the
disc it has been handed can be read, so the batch waits for a key after
loading it; cdrom_pause=0 in the INI skips the wait.

A game with sound= switches the PicoGUS's mode first, and pgusinit /mode
reloads the card's firmware: an image handed over in the next breath is
refused. If the load command fails, the batch says so and asks for a key
to try again.

With a card configured, games are installed with their discs as they
came - a cue sheet and its image, SETTLR2G.CUE and SETTLR2G.BIN - because
the card plays the CD audio tracks an ISO would have lost. Software mode
still gets an ISO, which is all SHSUCDHD can read.

WAVE86 writes each game's IMGMOUNT.BAT itself, from these settings, every
time it starts that game: the paths, the command and the letter go into
the batch as they are, so nothing depends on the environment having room
for them, and a change here reaches games that are already installed. A
game whose disc stays on the server keeps the batch the server sent. PGUSINIT.EXE /cdload is the default, so
cdmount_picogus= is only needed to give PGUSINIT's path.

Space marks a game for the install queue and I then fetches every
marked game, one after another - the launcher cannot download in the
background (DOS runs one program at a time), but it can work through a
list while you are elsewhere. A download that stops half way - Esc, a
dropped line, even the power going - leaves a note in the game's folder
and carries on next time from inside the very file it stopped in, so a
large disc image is not fetched again from the start, and
whatever the queue did not get to is still queued when WAVE86 comes
back up.

Every file is checked against a CRC32 from the server as it is written,
so a transfer that says "checksums good" arrived intact; a file that did
not is named and fetched again next time. That guards against a bad
cable or a tired card, not against anything on the network pretending to
be your server - the protocol is plain HTTP with no authentication.

While a game comes in, WAVEGET shows a progress screen - the bar, the
rate, the files as they land - and plays the soundtrack from MUSIC\ on
the AdLib. M, +, -, < and > work there as they do in the menu, Esc
stops the download. netmusic=0 in WAVE86.INI keeps it quiet.

When a game will not start
--------------------------
debug=1 in WAVE86.INI (or WAVE /debug) makes the batch that runs a game
show every line and stop at each step: the environment it hands the
game, the mount command as it will run, whether the image is where it
should be, the disc mounted on its own, then the game itself. It leaves
the batch behind as WAVELAST.BAT.

That batch is WAVERUN.BAT (it was RUNGAME.BAT; it runs WAVEGET as often
as it runs a game). WAVE.BAT cannot be replaced over the network, since
the shell is reading it at the time, so until you copy the new one the
launcher also leaves a one-line RUNGAME.BAT that calls it.

"Out of environment space" (4DOS: "Out of environment/alias space") from
any of those SET lines is the usual cause of a disc that will not mount:
the settings never reach the game's batch, so it looks for the image in
the wrong place. Give the shell more room in CONFIG.SYS -
    SHELL=C:\COMMAND.COM C:\ /E:1024 /P
(4DOS: /E:2048 on its SHELL line, or EnvironmentSize in 4DOS.INI) - and
WAVE86 /diag will tell you how much of it is left.

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
