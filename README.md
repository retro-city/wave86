# WAVE86

A game launcher for MS-DOS that can fetch its games from an eXoDOS
collection on your LAN, or play them straight off it, with a look of
its own and its own soundtrack.

It is a plain 16-bit real-mode program, so it runs on anything from an
XT to a 486 and beyond. I build it on a Mac with Open Watcom and try it
in dosbox-x, and on a real DOS booted in dosbox-x, before it goes onto
the real machine, a 486 with a PicoGUS. Version 0.2: the network side
has now been through real hardware, but it is still young.

![WAVE86 running in dosbox-x](docs/screenshot.png)

## What it does

- Lists the games in `C:\GAMES` (one folder per game), works out which
  file starts each one and finds the setup program if there is one.
- Runs a game with all of conventional memory free. The launcher does not
  stay resident under the game: it writes a small batch file and quits,
  and `WAVE.BAT` runs the game and brings the menu back afterwards.
- Plays music while you browse: AdLib tunes in id Software's IMF format
  and ProTracker MODs through a Sound Blaster. It starts silent, `M`
  turns it on. The header shows a VU meter, which becomes the volume bar
  while you adjust it.
- VGA gets a custom palette. EGA, CGA and mono cards get the standard
  colours and the layout still holds up. The default look is the eXoDOS
  one: a block-letter logo in the colours of the eXoDOS icon (violet, a
  red D, an orange S) with a drop shadow and matching DOS colours;
  `theme=wave86` in the INI brings back the synthwave look.
- Tells you what it is running on. The header line comes from CPUID and
  a clock measurement (exact via RDTSC on Pentium-class CPUs, estimated
  from a timed loop and marked with `~` on 486s and older) plus the
  BIOS memory count, so a K6-2 shows up as `AMD-K6 3D 400MHZ 64MB` and a
  DX2 as `80486 ~66MHZ 8MB`.

## Keys

| Key | Does |
| --- | --- |
| Up/Down, PgUp/PgDn, Home/End | move (a letter jumps to the next game starting with it) |
| Enter | run the game |
| S | run its setup program |
| F2 | rename the selected game (saved to the INI) |
| M | music on/off |
| + / - | volume |
| < / > | previous / next track |
| R | rescan the games folder |
| N | the games on the server (Enter plays it off the server, I installs it, Space queues it, L refreshes, C: disc with the game or on the server, U updates WAVE86 itself) |
| Esc | back to DOS |

## Putting it on the DOS machine

Copy these into a folder on the DOS box, say `C:\WAVE86`:

    build\WAVE86.EXE   WAVE.BAT   WAVE86.INI   MUSIC\   THUMBS\
    (and for downloads: WAVEGET.EXE  DHCP.EXE  MTCP.CFG)

Put each game in its own folder under `C:\GAMES` (or change `gamedir=`
in the INI). Add `C:\WAVE86` to your PATH and type `WAVE`. To boot
straight into the menu, put `CALL C:\WAVE86\WAVE.BAT` at the end of
`AUTOEXEC.BAT`.

Start it with `WAVE`, not `WAVE86.EXE`. Both work, but only the batch
file hands the game every byte of memory; the bare EXE keeps itself
resident and runs the game from there.

When something looks wrong, `WAVE86 /diag` prints what it found: DOS
version, free memory, video card, the AdLib test, `BLASTER`, DSP version,
CPU, and how many tunes it sees. `WAVE86 /launch KEEN4` starts a game
directly, which is handy for a boot-into-game AUTOEXEC.

## WAVE86.INI

Lives next to the EXE. Example:

    gamedir=C:\GAMES
    music=0          ; 1 = play at startup
    theme=exodos     ; or wave86: the synthwave look
    ; adlib=1        ; force FM on (0 = off); normally auto-detected
    ; modrate=11025  ; MOD mixer rate; 0 turns MODs off

    [KEEN4]
    name=Commander Keen 4

    [XCOM]
    name=UFO: Enemy Unknown
    exe=UFO.BAT

Section names are game folder names. The launcher adds a section for
every new folder it finds, so the file always lists your collection;
press `F2` in the menu to give a game a proper name (or run
`WAVE86 /name KEEN4 Commander Keen 4` from the prompt). Per game you can
set `name`, `exe`, `setup`, `args`, `cd` and `hide=1`; `source=exodos` or
`source=tdc` (set by the launcher after a download) shows where the game
came from in the details. Anything you leave out is detected: the launcher prefers an EXE named like the folder, then
`START`/`PLAY`/`GO` batch files, and ignores the usual `SETUP`,
`INSTALL`, `DOS4GW` and friends.

### Per-game setup: PicoGUS modes, environment, extra commands

A game's section can also carry things that go into the batch file
around the game:

    [DOOM]
    sound=gus
    env=ULTRASND=240,1,1,5,5
    pre=C:\UTILS\SLOWDOWN.EXE
    post=C:\UTILS\SLOWDOWN.EXE /off

`env=` becomes a `SET`, `pre=` runs before the game and `post=` after
it. `sound=` names a mode; the command for each mode is defined once at
the top of the INI, and `sound=` up there is the default for games that
don't say:

    soundcmd_sb=C:\PICOGUS\PGUSINIT.EXE /mode sb
    soundcmd_gus=C:\PICOGUS\PGUSINIT.EXE /mode gus
    sound=sb

With that, a PicoGUS switches to GUS for Doom and back to Sound Blaster
for everything else, on every launch, whether you start from the menu
or with `WAVE86 /launch DOOM`. The details pane shows the mode.

## Music

Everything in `MUSIC\` next to the EXE is the playlist.

**IMF** is the AdLib format id Software used in Commander Keen and
Wolfenstein 3D: a stream of OPL2 register writes with delays. `.IMF`
plays at 560 Hz, `.WLF` at Wolf3D's 700 Hz, and both the raw and the
length-header variants are fine. Tunes up to 240 KB are loaded into far
memory and freed before any game runs. It plays on an AdLib or on the FM
side of any Sound Blaster.

**MOD** is 4-channel ProTracker (`M.K.`, `M!K!`, `4CHN`, `FLT4`), mixed
in software and pushed through the Sound Blaster's DMA. The mixer picks
its rate from the CPU: 22 kHz on a 386 or better, 11 kHz on a 286, and
on an 8086 MODs are skipped. The usual effects are there (arpeggio,
portamento, vibrato, offset, volume slides, jumps and breaks, pattern
loop, fine slides, retrigger, cut, delay, speed and tempo). FastTracker
XM is not supported; OpenMPT can export a 4-channel XM to MOD.

The four synthwave tracks and `WAVE86.MOD` are generated by
`tools/makemusic.py` and `tools/makemod.py` (`make music` rebuilds them).
They are the only tunes in the repo. Anything else you drop into
`music/` gets copied into the build but stays out of git, since scene
tunes belong to their authors.

To use AdLib music from the demoscene, build `tools/scene2imf` (needs
`brew install adplug`). It plays a RAD, D00, HSC, A2M, SA2 or CFF file
through libadplug and records the register writes as an IMF:

    c++ -O2 -std=c++17 -I/opt/homebrew/include -I/opt/homebrew/include/libbinio \
        -L/opt/homebrew/lib -ladplug -lbinio -o tools/scene2imf tools/scene2imf.cpp
    tools/scene2imf ~/Downloads/tune.rad music/TUNE.IMF

Keep to 8.3 filenames. `tools/imf2wav` renders an IMF to WAV so you can
listen on the Mac.

### Sound cards

Detection is the classic AdLib timer test, but it waits up to 30 ms for
the flag because emulated cards such as the PicoGUS raise it late. If a
Sound Blaster DSP answers, FM is assumed present anyway, since every SB
has one. `adlib=` and `modrate=` in the INI override all of this.

## Building

You need Open Watcom V2 in `toolchain/` (it is not in git). Download
`ow-snapshot.tar.xz` from the `Current-build` release of
github.com/open-watcom/open-watcom-v2 and extract the host tools for
your machine (`armo64` on an Apple silicon Mac, `bino64` on an Intel
one, `binl64` on Linux) plus `h` and `lib286` into `toolchain/`.

    make          builds build/WAVE86.EXE and copies WAVE.BAT, the INI and MUSIC\
    make run      opens it in dosbox-x, with sound and networking (see below)
    make test     renders the UI headlessly to build/screen.png
    make dist     the DOS side as dist/wave86-<version>-dos.zip, ready to copy over
    make music    regenerates the soundtrack
    make clean

The Makefile picks the host tools for the machine it runs on (`armo64`,
`bino64` or `binl64` under `toolchain/`); the GitHub workflow in
`.github/workflows/build.yml` does the same on Linux and attaches the
zip to every build, and to a release for a tag like `v0.1`.

The code is compiled for the 8086 instruction set with the small memory
model, and comes out around 40 KB.

## Screenshots in the details pane

A game with a `THUMBS\<DIR>.THM` next to the EXE shows a small
screenshot under its details. It is still text mode: a VGA's character
shapes live in RAM, and in 512-character mode bit 3 of the attribute
picks one of two fonts. The second font starts as a copy of the first,
so the bright UI text is unchanged, and both banks lend the CP437 codes
the UI never uses, about 280 glyphs. The picture is 26 by 7 cells
(close to 4:3 on a CRT), each cell two palette colours plus a 1-bit
pattern loaded as a custom glyph; flat cells and cells that look like
standard block characters borrow those, and busy pictures get
near-identical patterns merged until they fit. VGA only.

    python3 tools/makethumb.py screenshot.png THUMBS/KEEN4.THM    # needs ffmpeg

The cells are matched to a theme's colours, so a picture belongs to a
theme: `THUMBS\KEEN4.THM` is for the default look, `THUMBS\wave86\` holds
the ones made with `--theme wave86`, and the launcher looks in the
folder named after its theme first.

## Games from eXoDOS or the Total DOS Collection over the network

Press `N` in the menu and the launcher shows the games in your
collection, served from a machine on the LAN; I installs one straight
into `C:\GAMES` and it appears in the games list, named and with its
executable set, and Enter plays it off the server without copying. Nothing is unzipped on the DOS side: the server
streams plain files.

On the machine with the collection (Mac, Linux, a NAS with Python):

    python3 tools/waveserve.py ~/Downloads/eXoDOS --port 8086 --cd
    python3 tools/waveserve.py --tdc ~/Downloads/1981-1992 --port 8086
    python3 tools/waveserve.py ~/Downloads/eXoDOS --tdc ~/Downloads/TDC --port 8086

For eXoDOS it indexes the game zips, reads each game's `dosbox.conf`
for the program that starts it and takes genre and description from the
platform XML. `--cd` includes the CD games: the server turns the cue/bin
in the zip into a plain ISO while it streams (the 2048 data bytes of
each sector, cut at the volume size; audio tracks are dropped), so
Syndicate Plus arrives as its files plus `CD\SYNDICAT.ISO`, and the
menu marks such games "CD IMAGE".

The Total DOS Collection is plain folders, `1992/Title (1992)(Publisher)
[Genre]/`, one game each, so there is no manifest to read: the title,
year, publisher and genre come from the folder name, and the launcher
works out the executable once the files are on disk. The details pane
says which collection a game is from, and the games list tags downloaded
games with eXoDOS or TDC. Both serve from one port; the server addresses
TDC games as `tdc:DIR`.

A collection that is still coming in over BitTorrent is handled: folders
with nothing downloaded yet are left out, and a game whose files are
partly placeholders (0 bytes, dated the day the torrent started) is
listed as "INCOMPLETE" and shipped without them. If what arrives has
nothing to run, the launcher says so on the status line instead of
listing an empty folder. The server re-indexes every five minutes.

On the DOS machine, copy `WAVEGET.EXE`, `DHCP.EXE` and `MTCP.CFG` from
`build/` next to the launcher, put the server's address in the INI:

    server=192.168.1.10:8086

give COMMAND.COM a bigger environment in `CONFIG.SYS`, since DOS's
256-byte default runs out once `MTCPCFG` joins PATH and the sound
variables ("Out of environment space"):

    SHELL=C:\COMMAND.COM C:\ /E:1024 /P

and get the network card up in `AUTOEXEC.BAT` (PicoMem or any NE2000):

    LH NE2000 0x60 5 0x300
    SET MTCPCFG=C:\WAVE86\MTCP.CFG
    C:\WAVE86\DHCP

`LH` wants a memory manager providing upper memory (HIMEM plus EMM386,
or `dos\JEMMEX.EXE` with `DOS=HIGH,UMB`); without one it just loads
low. The CD drivers the launcher's batches load use `LH` too.

`make run` does all of this inside dosbox-x: it turns on the NE2000
emulation (slirp backend), loads the packet driver from `Z:\SYSTEM`,
runs DHCP and points the launcher at `10.0.2.2:8086`, which is the Mac
as the emulator sees it, through the `WAVESRV` environment variable
(it overrides `server=`). Start `waveserve.py` first.

`NE2000.COM` in `net/` is the Crynwr packet driver (GPL) for the card,
the same one dosbox-x carries on its Z:. `NETDRIVE.SYS` and
`NETDRIVE.EXE` are mTCP NetDrive's DOS side (GPL, source in
`net/mtcp/APPS/NETDRV`). `WAVEGET.EXE` is a separate program built on mTCP (GPL v3, sources in
`net/`), so the launcher itself stays a small 8086 program with no
network code; downloads run through the same batch hand-off as games,
with all memory free. Esc aborts a download. The list is read from disk
as you scroll, with only a table of line offsets in memory, so a
collection of 16,000 games fits in 64 KB; a letter key jumps through the
titles starting with it. The server addresses games by folder name, so
a list that is a few minutes old still fetches the right one.

## CD images

A game with an ISO in its `CD\` folder (or `cd=` in its INI section)
gets the image mounted as D: for as long as it runs. The lines go into
the same batch file as the game, so it works from the menu and from
`WAVE86 /launch`:

    SHSUCDHD /F:C:\GAMES\SYNDICAT\CD\SYNDICAT.ISO /Q
    SHSUCDX /D:SHSU-CDH,D /Q
    call RUN.BAT
    SHSUCDX /U /Q
    SHSUCDHD /U /Q

On real DOS that is Jason Hood's SHSUCDHD (an image file as a CD-ROM
device) and SHSUCDX (his small MSCDEX replacement), built from his
sources into `cdrom/` and copied next to the launcher by `make`. They
load before the game and unload after it, so nothing stays resident.
Under DOSBox the launcher uses `IMGMOUNT D image -t iso` instead; it
knows it is in DOSBox by the Z: drive. `cdmount=` and `cdunmount=` in
the INI replace either, with `$ISO` standing for the image path.

A CD game from the server comes with its own `IMGMOUNT.BAT`, and its
start batch calls it first, so the game also runs from a plain prompt
with the WAVE86 folder on the PATH. The batch reads two things from the
environment, which the launcher sets from the INI: `WAVECDROM`, the
folder holding the discs (`cdrom_storage=`; without it a game keeps its disc in
its own `CD\` folder, with it every disc goes there, named after the
game, `SETTLR2G.ISO`), and `IMGMOUNT`, how to mount them: `SOFTWARE`
(SHSUCDHD and SHSUCDX, the default) or the name of a card that emulates
a CD-ROM drive itself - `PICOGUS`, `PICOMEM` - which DOS sees as a real
drive with its letter fixed at boot (`cdrom_letter=`), the image loaded
by the card's own command, `cdmount_<mode>=` (and `cdunmount_<mode>=`).
The image is appended to that command, as its full path or, with
`cdrom_name=1`, as its bare file name.

A card that mounts images itself gets the discs as they came. The ISO
conversion keeps only the data track, and many of these games have their
music as CD audio: Settlers II Gold carries 328 MB of it and Warcraft II
more still. So when `imgmount=` names a card, the launcher asks the server
for `?cd=raw` and receives the cue sheet and its image untouched - renamed
to 8.3 after the game folder, `SETTLR2G.CUE` and `SETTLR2G.BIN`, with the
cue's `FILE` line rewritten to match - and the card gets the `.CUE`. In
software mode it still asks for `?cd=local`, the ISO, because SHSUCDHD
reads nothing else.

A PicoGUS with CD-ROM support reads its images from a drive, so put
them there and let PGUSINIT load them:

    cdrom_storage=W:
    imgmount=PICOGUS
    cdrom_letter=D
    cdmount_picogus=C:\PICOGUS\PGUSINIT.EXE /cdloadname

`PGUSINIT.EXE /cdloadname` is what PICOGUS runs when `cdmount_picogus=`
is not given - `/cdload` takes the number of an image, `/cdloadname` its
name. The card needs a moment before a disc it has just been handed can
be read, and a game that looks at once finds the drive empty, so the
batch waits for a key after loading the image (`cdrom_pause=0` skips it).
A game with `sound=` switches the card's mode just before, and `pgusinit
/mode` reloads its firmware, so an image handed over straight after is
refused; when the load command returns an error the batch says so and
waits for a key to try again.

The launcher writes a game's `IMGMOUNT.BAT` itself from these settings,
every time it starts that game, with the image path, the command and the
letter written in as they are. The server's version reads all of that
from the environment, which asks the shell for five `SET`s it may not
have room for and cannot know what card the machine has; this one needs
only `CD`, which the game's own batch reads. It also means a change in
the INI reaches a game that is already installed. A batch that fetches
its disc over NetDrive is left alone - that one needs the server's
address at run time - and so is anything not generated in the first
place. A PicoMem 2 would be `cdrom_storage=S:\CDROM`, `imgmount=PICOMEM`,
once its firmware has a command to load an image; until then keep it on
`SOFTWARE` and the discs still collect on its SD card. Under DOSBox the
batch uses IMGMOUNT whatever the mode says; playing off the server
always mounts in software. The batch puts the letter the disc landed on into `CD`,
and the server rewrites the start batch to say `%CD%:` wherever the
eXoDOS conf said `D:`, so it does not matter if D: is taken by a real
CD-ROM or by NetDrive. When a game has `IMGMOUNT.BAT` the launcher
leaves the mounting to it and calls it with `/U` afterwards.

### CD images that stay on the server

A 300 MB disc does not have to travel to the DOS disk at all. With

    python3 tools/waveserve.py ~/Downloads/eXoDOS --port 8086 --cd --netdrive ~/wave86-cd

the server wraps each game's ISO in a FAT volume under `~/wave86-cd`
(built once, at index time) and runs Michael Brutman's mTCP NetDrive
server on UDP port 2002 to hand those volumes out. `make netdrive`
fetches his official build of that server for this machine into
`build/netdrive`. On the DOS side the game's `IMGMOUNT.BAT` attaches
the volume as a drive with NetDrive, points SHSUCDHD at the ISO on it
and lets SHSUCDX give the disc a letter. Brutman measured this stack on
a 386DX-40 at the same speed as NetDrive alone, around 370 KB/s.

What the DOS machine needs for that:

    DEVICE=C:\WAVE86\NETDRIVE.SYS -d:2   in CONFIG.SYS (LASTDRIVE=Z too)
    DRVOFF D:                            first thing in AUTOEXEC.BAT
    PACKETINT 0x60, 0x65                 in MTCP.CFG (the second interrupt
                                         is for DHCP and WAVEGET while
                                         NetDrive holds the first); MTU 1500

The two lines are about the letter. DOS hands a block device driver the
next free letter, so `NETDRIVE.SYS` lands on D:, exactly where the games
want their disc, and no driver can ask for another letter. The way out
(DivByZero's, from the NetDrive thread on VOGONS) is to let NetDrive
reserve two letters and take the first out of use again: `DRVOFF`
(`net/DRVOFF.C`, a dozen lines against DOS's drive table) marks D:
invalid, so SHSUCDX can give the disc D: while NetDrive works on E:.
The game's `IMGMOUNT.BAT` finds NetDrive's letter by itself; `netdrive=`
in the INI names it if that guess is wrong. The launcher passes the
server's address in `WAVENDSRV` (the machine of `server=`, port 2002 or
`netdrive_port=`). The network view marks such games "NET CD".

There is no CD audio either way: SHSUCDHD serves data tracks only and the
server drops the audio tracks when it makes the ISO. What the ISO route
keeps is a real CD-ROM drive as far as the game can tell.

### Playing off the server

With NetDrive set up, Enter in the network view plays a game without
copying anything (I installs it instead): the server builds a disk image of the whole game on
first request (its files, its batches, the disc under `CD\`, kept in
the same folder as the CD volumes), the DOS side attaches it as a
drive, runs the game from there and detaches it afterwards. `WAVE86
/play SYNDICAT` does the same from the prompt, given a fetched list.
The image is session scoped, NetDrive's term for a private write
journal per connection that is thrown away when the session ends: any
number of machines can play from one image, and a game can save as it
likes, but those saves are gone next time. Download the game for
keeps. Speed is NetDrive's, around 370 KB/s on a 386, so big games
load slower than from the hard disk. DOSBox's own shell has no packet driver, so
these games run under a real DOS: `make dosrun` has the driver in its
CONFIG.SYS.

The disc lands on D: because that is where eXoDOS mounts it and games
like Syndicate Plus have `D:` written into their start batch. If a real
CD-ROM already owns D: with MSCDEX loaded, SHSUCDX refuses to install
beside it: either let SHSUCDX drive the real drive too (`SHSUCDX
/D:MSCD001` in AUTOEXEC.BAT instead of MSCDEX, giving the image its
letter after that) or add `/I` through a `cdmount=` line.

### A queue, and picking up where it stopped

DOS runs one program at a time, and the launcher hands WAVEGET the whole
machine while it fetches, so nothing downloads in the background. What it
does instead is work through a list unattended: `Space` puts the game
under the cursor in the install queue (a mark in the list, the total in
the details pane), and `I` then fetches the lot, one after another, with
each transfer showing its place in the queue. Sixteen games fit.

A download that stops part way is not lost. WAVEGET leaves a note,
`WAVE86.RSM`, in the game's folder saying how many of its files are
already there and which request they came from; the next attempt asks the
server to start after them (`/pack/KEY?from=N`, and the server opens the
stream with `S <files> <bytes>` so the client knows what it skipped). The
file that was half written when the line went down is sent again from the
start. The note goes when the game is complete.

The queue survives the same way. `NETGAME.TXT` is the list of games on
their way in, and anything that did not arrive - a folder that never
appeared, or one with a resume note in it - goes straight back into the
queue when the launcher comes up again. So a queue interrupted by `Esc`,
a dropped line or the power switch is still there afterwards: press `I`
and it carries on. Inside the batch, `Esc` (exit code 2) stops the rest
of the queue, while a game the server cannot provide (3) is skipped and
the next one starts.

### Is what arrived what was sent?

TCP's 16-bit checksum is thin cover for a few hundred megabytes over a
tired ISA card, so WAVEGET asks for `?crc=1` and the server follows every
file with `C <crc32>`. The number is worked out on the DOS side as the
bytes are written - no second pass over the disk - and a file that does
not match stops the transfer there and then, naming the file. It is not
counted as done, so the resume note points at it and the next attempt
fetches it again. A run where every file matched says so: *checksums
good*. Files skipped by a resume were checked when they were written.

That is corruption, not security. The protocol is plain HTTP on your own
network with no authentication and no encryption: anything on the LAN
could answer instead of your server, and the CRC would then be the
attacker's CRC. It is meant for a machine you own on a network you
trust.

### The transfer screen

A game takes minutes to arrive on a 486, so `WAVEGET` takes the screen
while it works: the launcher's colours in a band across the top, a
progress bar in the same gradient, KB done and left, the rate, the time
remaining and the files as they land. It plays the soundtrack too - the
launcher's music engine (`src/music.c`) built again in large model and
linked into WAVEGET, reading the same `MUSIC\` folder. `M`, `+`, `-`,
`<` and `>` do what they do in the menu; `Esc` stops the download. The
music line carries the track, a live level and the volume, and when
there is nothing to hear it says why - no OPL, no tracks (with the
folder it looked in), or `netmusic=0`. An update is over in a second or
two, so that screen holds for a moment at the end rather than flashing
past.

Only the FM tracks play: `cfg_modrate` is nailed to 0 in WAVEGET, which
keeps the MOD mixer and its DMA away from the packet driver, so the cost
is a handful of OPL register writes on each timer tick. `netmusic=0` in
`WAVE86.INI` turns it off altogether, and `adlib=` is honoured there as
it is in the launcher. Set `WAVEDUMP=C:\SCREEN.BIN` and WAVEGET writes
its text screen there once a second, which is how the harness gets to
look at it.

### Updating WAVE86 from the server

`U` in the network view fetches a fresh `WAVE86.EXE`, `WAVEGET.EXE` and
`DRVOFF.EXE` from the machine that builds them and starts the new
launcher: the test loop on real hardware is `make`, then `N`, `U`.
`WAVEGET UPDATE 192.168.1.10:8086 C:\WAVE86` does the same from the
prompt. Each file is written beside its target and renamed only once it
has arrived whole, so a dropped connection cannot leave half a program
behind - including WAVEGET, which overwrites itself.

The server sends what `make` put in `build/`. `--update DIR` adds
everything in DIR, and anything there with the same name wins:

    python3 tools/waveserve.py ~/Downloads/eXoDOS --port 8086 --update ~/wave86-push

That is the way to push a `WAVE86.INI`, a driver, whatever else. Three
files are deliberately not in the default set: `WAVE.BAT`, because
COMMAND.COM is reading it line by line while the update runs, `MTCP.CFG`,
because DHCP keeps the lease in it, and `WAVE86.INI`, because it is the
machine's own settings and the launcher writes to it.

## Testing without a DOS machine

`WAVE86 /dump [DIR]` draws the menu with DIR selected (`/dump NET [DIR]`
draws the network view), then dumps the text buffer, the BIOS font and the palette; `tools/rendscr.py` turns that into a PNG. That is
how the screenshot above was made. `WAVE86 /mustest [seconds]` plays
headlessly and reports where the player got to; with a MOD it also
writes the mixer output to `MODDUMP.RAW`, which I compared against
libopenmpt to check the mixer. All of this runs under
`SDL_VIDEODRIVER=dummy dosbox-x -nogui`.

One thing to know: dosbox-x hangs after about ten `-c` commands, so
longer test sequences go into a DOS batch file and get started with a
single `call`.

### On a real DOS kernel

DOSBox's shell is not DOS: it has no device driver chain, keeps every
drive letter for itself and answers INT 21h its own way, so things like
the CD drivers can only be checked against a real kernel. `make dostest`
boots one in dosbox-x: it builds a hard-disk image with the launcher and
`GAMES\`, boots `dos/FREEDOS.IMG` (a FreeDOS 1.3 floppy stripped to the
kernel and shell, see `dos/README.md`), runs `tools/dostest.py`'s smoke
test and prints the results. The smoke test runs `WAVE86 /diag`, and for
the first game with a CD image loads SHSUCDHD and SHSUCDX, lists D: and
checks the batch the launcher generates. About half a minute.

    make dostest                                   FreeDOS
    make dostest DOS=~/Downloads/dos/Disk1.img     MS-DOS 6.22, from its setup disk 1
    make dosrun                                    the same, on screen, booting into WAVE.BAT
    python3 tools/dostest.py --game SYNDICAT --script mytest.bat

`make dosrun` is `make run` on a real kernel: the window boots DOS, loads
the packet driver, DHCP and a mouse driver, and starts the launcher with
sound and the network view working against a waveserve on this machine,
at 50,000 CPU cycles (`DOSTEST_ARGS=--cycles=N` for another speed). The disk
starts clean, with only Alley Cat from `GAMES\` (`DOSTEST_ARGS="--game
xcom --game keen4"` for others); what you download there lands inside
`build/dosrun/hd.img`, a 500 MB disk (FAT16 stops at 2047 MB;
`--size=1500` for more), not in `GAMES\`.

Any bootable 1.44 MB floppy image works as `DOS=`; the first setup disk
of MS-DOS 5 or 6 is a plain boot disk once the harness replaces its
AUTOEXEC. Keep those outside the repo. The CONFIG.SYS the harness
writes loads JEMMEX (FreeDOS's memory manager: XMS, EMS, upper memory)
with `DOS=HIGH,UMB`, NetDrive with DEVICEHIGH and the packet driver, the
mouse and the CD drivers with LH, so a booted game has its conventional
memory. Your own test batch runs on C:
with the launcher in `C:\WAVE86` and the games in `C:\GAMES`; whatever
it writes to `C:\RESULTS` is printed, and `FAIL.TXT` there fails the
run. `tools/nettest.bat` is one such batch: the packet driver, DHCP and a
list fetch from a waveserve here, the chain the 486 uses. Needs mtools
(`brew install mtools`).

## Layout

    src/wave86.c   main loop, WAVERUN.BAT, command line flags
    src/ui.c       the screen: direct video memory, CP437, VU meter
    src/vga.c      card detection, palette, screen dump
    src/scan.c     finding games and their executables
    src/ini.c      WAVE86.INI
    src/music.c    AdLib detection, IMF player, playlist, volume
    src/mod.c      Sound Blaster DMA, ProTracker loader, sequencer, mixer
    src/cpu.c      CPU and memory identification for the header line
    src/net.c      the server's game list, download bookkeeping
    net/           WAVEGET.CPP, MTCP.CFG, mTCP's library and DHCP (GPL)
    cdrom/         SHSUCDHD and SHSUCDX, the CD image drivers for real DOS
    dos/           FreeDOS boot floppy for make dostest
    tools/         composers, converters, renderers (host side)
    music/         the soundtrack
    docs/          screenshot

## Later

- The network side on the real 486 with its PicoMem: everything above
  is verified in dosbox-x and on real DOS kernels booted in it, not yet
  on the card itself.
- XM playback, if the 486 can take it.
- Joystick navigation, 50-line mode, a wider list for big collections.

## Licence

WAVE86 is free software under the GNU General Public License, version 3
or later; see `LICENSE`. It ships with other people's programs under
their own terms, mTCP, the SHSUCD suite, the Crynwr packet driver and a
few FreeDOS pieces; `THIRD-PARTY.md` lists each with its licence and
where it came from.
