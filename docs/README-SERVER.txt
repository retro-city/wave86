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
    run-server.bat     the same for Windows (untested)
    waveserve.ini      the settings, one per line, each with what it does
    eXoDOS.torrent     the eXoDOS v6.04 collection's torrent, from the eXo
                       project (www.retro-exo.com)
    tools/             the server itself, in Python (waveserve.py and the
                       files it uses)
    build/             the DOS programs the launcher's U key (WAVEGET
                       UPDATE) fetches from here: WAVE86.EXE, WAVEGET.EXE...
    netdrive/          Michael Brutman's mTCP NetDrive server, for macOS
                       (Apple silicon), Linux (x86-64, ARM) and Windows

What it needs
-------------
    Python 3.10 or newer.
    libtorrent's Python bindings, for the torrent:
        macOS           brew install libtorrent-rasterbar
        Debian, Ubuntu  sudo apt install python3-libtorrent
        elsewhere       pip install libtorrent
    ffmpeg, for the games' pictures in the launcher (optional):
        macOS           brew install ffmpeg
        Debian, Ubuntu  sudo apt install ffmpeg

Starting it
-----------
    ./run-server.sh

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
