WAVE86 server - waveserve
=========================

This is the other end of WAVE86's network view: a small web server that
lists the games of the eXoDOS collection to the DOS machine, packs a game
up when the launcher asks for it, lets it be played straight off the
server through mTCP NetDrive, and hands out new versions of the DOS
programs. It runs on a Mac, a Linux machine or a Raspberry Pi on the same
network as the DOS machine.

What is in this folder
----------------------
    run-server.sh      starts the server (macOS, Linux)
    run-server.bat     the same for Windows, installing what it needs first
    waveserve.ini      the settings, one per line, each with what it does
    eXoDOS.torrent     the eXoDOS v6.04 collection's torrent, from the eXo
                       project (www.retro-exo.com)
    tools/             the server itself, in Python (waveserve.py and the
                       files it uses)
    build/             the DOS programs the launcher's U key (WAVEGET
                       UPDATE) fetches from here: WAVE86.EXE, WAVEGET.EXE...
    netdrive/          Michael Brutman's mTCP NetDrive server, for macOS
                       (Apple silicon), Linux (x86-64, ARM) and Windows
    requirements.txt   the Python package the server needs, for pip

What it needs
-------------
Python 3.9 or newer, and libtorrent's Python bindings for the eXoDOS
torrent. Everything else is in Python's own library. The pictures for
the launcher's details pane need ffmpeg as well; without it the server
runs and sends no pictures.

macOS, with Homebrew:

    brew install libtorrent-rasterbar ffmpeg

Debian, Ubuntu, Raspberry Pi OS:

    sudo apt install python3-libtorrent ffmpeg

Anywhere else, or to keep it out of the system's Python, with pip in a
virtual environment in this folder (pip has libtorrent for Python 3.9
to 3.13; with 3.14 use the packages above instead):

    python3 -m venv .venv
    .venv/bin/pip install -r requirements.txt

On Windows, unpack the whole zip and start run-server.bat in the
unpacked folder (double-click it, or type it in a command prompt there).
It looks for a Python from 3.9 to 3.13 for x64 or x86, the Visual C++
runtime libtorrent needs, and ffmpeg, and offers to install what is
missing with winget (Python.Python.3.13, Microsoft.VCRedist.2015+.x64,
Gyan.FFmpeg; winget comes with App Installer, from the Microsoft
Store). Then it makes .venv here with that Python, installs
requirements.txt into it the first time, and starts the server. A
Python 3.14 alone will not do, nor an ARM64 one: pip has no libtorrent
for them, so on ARM64 Windows it installs the x64 Python, which Windows
11 runs as well. A "no" to FFmpeg is remembered in ffmpeg-no.txt. Behind
a proxy, set HTTPS_PROXY before the first start, for pip.
run-server.bat --help lists the server's options.

run-server.sh uses .venv when it is there, and the system's python3
otherwise.

Starting it
-----------
    ./run-server.sh          (macOS, Linux)
    run-server.bat           (Windows)

The first time, the server asks where eXoDOS is:

    a) On demand, via BitTorrent - the torrent file, eXoDOS.torrent from
       this folder unless you name another
    b) Local install - the eXoDOS (or eXoDOS Lite) folder, the one with
       eXo/eXoDOS in it

and writes the answer into waveserve.ini: torrent= or exodos=, with the
other emptied, so a local install turns the torrent off. It asks only on
a terminal; --setup asks again, and firstrun=1 in waveserve.ini does it
at the next start.

The console shows what the DOS machines are fetching and how the swarm is
doing; q stops the server. With no terminal (a service, a log file) it
logs instead. Any option on the command line overrides waveserve.ini for
that run, for example:

    ./run-server.sh --port 8090 --log serve.log

python3 tools/waveserve.py --help lists them all.

On macOS, a folder unpacked by Finder is marked as downloaded, and the
NetDrive program will not start until the mark is gone:

    xattr -dr com.apple.quarantine .

What happens
------------
The server lists every game in eXoDOS.torrent at once, but fetches
nothing until a DOS machine asks for a game. Then it downloads only that
game's pieces from the swarm into cache= (~/.wave86/torrent by default)
and sends the game on while they arrive. The whole collection is
hundreds of gigabytes; the cache holds only what has been asked for.
Pictures for the launcher's details pane are made once per game into
thumbs= (~/.wave86/thumbs), and the NetDrive volumes for playing off the
server go into netdrive= (~/.wave86/cd). A server from before 0.6 kept
them in ~/wave86-torrent, ~/wave86-thumbs and ~/wave86-cd; this one
moves them under ~/.wave86 when it starts. Stop the old server first.

An eXoDOS collection already on the disk (or an eXoDOS Lite install) can
be used instead of, or as well as, the torrent: put its folder, the one
with eXo/eXoDOS in it, in exodos= in waveserve.ini.

The ports
---------
    TCP 8086    WAVE86 on the DOS machine connects here (port=)
    UDP 2002    NetDrive, for games played off the server (netdrive_port=)
    TCP/UDP 6881  the torrent swarm (torrent_port=)

A firewall on this machine has to let the DOS machine in on the first
two.

The DOS side
------------
In C:\WAVE86\WAVE86.INI on the DOS machine, server= names this machine:

    server=192.168.1.20:8086

or press N in the launcher and type the address when it asks. N then
lists the games; Enter installs one (in the PicoMEM image's Network Mode,
Enter plays it off the server instead), Space queues it, P plays it off
the server. U in the network view fetches the programs in build/ here.

The launcher for the DOS machine is in wave86-x.x-dos.zip, and a
ready-made PicoMEM 2 disk in wave86-x.x-picomem-hdd.zip, from the same
release.

Licences
--------
WAVE86 is GPL (LICENSE.TXT in the DOS zip). NetDrive's licence is
netdrive/copying.txt. The torrent file describes the eXoDOS collection,
which is the eXo project's.
