# Network games from eXoDOS (plan)

Findings from 2026-09-06, before writing any code.

## What was checked

- mTCP (2025-01-10 release) is the TCP/IP stack for real-mode DOS: C++,
  compiled per application with Open Watcom, runs on an 8088, talks to a
  packet driver, has a DHCP client. Its HTGET source **builds with the
  toolchain in this repo** (wpp + wasm + wlink, large model, -0 code).
  wmake cannot follow the makefile's DOS-style search paths on macOS, so
  the objects are compiled with explicit commands instead.
- dosbox-x on this Mac has NE2000 emulation with the slirp backend, ships
  `NE2000.COM` on `Z:\SYSTEM`, and its slirp hands out DHCP. Test run:
  packet driver + mTCP `DHCP.EXE` + `HTGET` fetched a 2 MB file from a
  Python http.server on the Mac (`10.0.2.2`), byte for byte, in 17 s
  including boot. So the whole DOS side can be developed and tested
  here before the PicoMem is involved.
- eXoDOS v6.04 (Lite) layout: one zip per game in `eXo/eXoDOS/`, named
  `Title (Year).zip`. Inside is a single folder with an 8.3 name (for
  example `$100000P/`) holding the plain game files plus an empty
  `Title (Year).exo` marker. No dosbox.conf in the zip; the per-game
  DOSBox files and the LaunchBox XML live in `Content/!DOSmetadata.zip`,
  `Content/XODOSMetadata.zip` and `Content/LaunchBox.zip`.

## Design

Three parts, only one of which speaks TCP:

1. **waveserve.py** on the Mac or NAS, plain Python, indexes an eXoDOS
   folder and serves HTTP/1.0 on a port:
   - `GET /list` -> one line per game: `id|DIR|Title|year|genre|KB|EXE`
     (EXE from the game's eXoDOS dosbox.conf autoexec, when it names one).
     Text, under 96 characters per line, so the DOS side parses it with
     `fgets`. Games above a size limit (CD-ROM rips) are left out unless
     asked for.
   - `GET /pack/<id>` -> a stream of `F <bytes> <DOS path>` headers each
     followed by the raw file, then `E`. The client writes files straight
     to `C:\GAMES\<DIR>\`, so nothing has to be unzipped on the 486.
     Long or non-8.3 names are mangled or skipped, `.exo` markers skipped.
   - Title and year come from the zip name; genre, developer and notes
     from `xml/all/MS-DOS.xml` (matched on the `<RootFolder>` short name);
     games whose autoexec uses `imgmount` (CD images) are left out unless
     `--cd`. The index is rebuilt every `--rescan` seconds (default 300).
     The server also looks in the nested install root `<root>/eXoDOS/`
     that `Setup eXoDOS.bat` creates, and in `Content/GameData/eXoDOS/`.
2. **WAVEGET.EXE**, a separate DOS program built on mTCP (C++, large
   model, GPL like mTCP): `WAVEGET LIST` writes `NETLIST.TXT` next to
   the launcher, `WAVEGET GET <id>` streams a pack into `C:\GAMES` with
   a progress line and leaves `NETGAME.TXT` (DIR and title) for the
   launcher. The server address comes from `server=` in `WAVE86.INI`.
3. **WAVE86** gets a network view (`N`): it shows `NETLIST.TXT`, and
   Enter hands `WAVEGET GET <id>` to the batch loop exactly like a game
   launch. When the menu comes back it rescans, names the new folder from
   `NETGAME.TXT`, and the game is there. The launcher stays 8086 C with no
   network code in it; all memory is free while WAVEGET runs.

On the 486, AUTOEXEC loads the PicoMem's NE2000 packet driver
(`NE2000 0x60 <irq> <port>`), sets `MTCPCFG=C:\WAVE86\MTCP.CFG` and runs
`DHCP` once. WAVEGET and DHCP.EXE ship with the launcher.

## Expectations

- A 486 with mTCP moves several hundred KB/s on ISA; the PicoMem's WiFi
  bridge is the limit. Even at 50 KB/s a typical floppy-era game (0.3 to
  10 MB) arrives in well under a minute.
- Executable detection stays in the launcher, which already handles the
  eXoDOS folders (they are the games' own files). For the odd game that
  needs it, the per-game DOSBox autoexec in the metadata can seed `exe=`.
- CD-era games and anything needing DOSBox-only tricks (imgmount) are out
  of scope for the first version.

## Order of work

1. waveserve.py against the local eXoDOS Lite; test with curl.
2. WAVEGET (start from HTGET, replace the file writer with the pack
   parser); test in dosbox-x with slirp.
3. The `N` view in the launcher, the batch hand-off, INI naming.
4. Then the PicoMem on the real 486.
