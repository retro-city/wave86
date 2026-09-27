# waveserve on top of the torrent: a plan

Goal: `waveserve` lists the whole eXoDOS collection and serves any game from
it while holding only what has been asked for. The torrent is the disk; the
pieces a request needs are fetched when it needs them.

## Where it stands (2026-09-19)

Phases 0 to 3 are built and tested; 4 is half done, 5 is not started.

- **0, the spike**: done. One piece out of the real swarm in 11 to 14 s from
  a cold start (most of it finding peers; a running server has them
  already), nothing else downloaded, everything in libtorrent's part file,
  a restart knows what it holds in 0.1 s. The engine is libtorrent 2.1.1
  from Homebrew.
- **1, `tools/torrentfs.py`** and `tools/test_torrentfs.py`: done. A 290 MB
  zip read through it from the swarm at 6.5 MB/s tests clean.
- **2, the seam and the two-stage index**: done, with `index()` split into
  `game_from_zip()` and the disk-only index byte-identical before and
  after. 7,606 games listed in three seconds; a game that is only in the
  torrent is opened in 3 to 18 s. What the opened zips held is kept in
  `<cache>/<infohash>.index`; when waveserve.py itself has changed since,
  the zips are opened again in the background, which costs the disk a
  little and the swarm nothing.
- **3, `W`/`T`/`X` lines**: done on both sides, and run with the real
  WAVEGET under FreeDOS in the emulator: it waited 11 s showing "the
  server is fetching it: 1 pieces to go, 95 peers, 716 KB/s", then took
  the game with good checksums, the wait kept out of its KB/s.
- **4**: NetDrive images of torrent games are made on first request
  (done, not yet run against a real disc); running with no eXoDOS folder
  at all works, from the metadata zip in the torrent: the confs (16 MB),
  the XML (two pieces) and, since 2026-09-23, the artwork for the pictures
  that come with a game (one piece per picture).
- **5, a cache budget**: not started.

Still to look at: a `make` target for the Python tests; the launcher's
answer to an `exe=` that does not exist (the 8 games with no program in
their conf); whether the Lite torrent is worth adding as a second source.

## What was measured (2026-09-19)

**eXoDOS.torrent** (the full one): 14,011 files, 650 GB, 83,164 pieces of
8 MB, public, tracker opentrackr. It is byte-identical to the torrent eXoDOS
Lite ships in `eXo/util/aria/`, and Lite's own `install.bat` fetches one game
at a time with `aria2c --select-file=N`. We are doing what the collection
already does, at piece grain instead of file grain.

- 7,666 game zips in `eXo/eXoDOS/` (median 0.36 MB; 6,167 are smaller than
  one piece), 6,334 extras zips in `Content/GameData/eXoDOS/` (manuals and
  the like, which waveserve does not use), the metadata zips in `Content/`.
- Fetching one game costs the pieces its zip overlaps: median **one piece,
  8 MB** (Keen 4: a 0.6 MB zip, 8 MB fetched, and its neighbours in the
  alphabet come along free). Settlers II: 444 MB zip, 56 pieces.
- Reading every zip's directory (its last 64 KB) to index the way waveserve
  does today would touch 2,062 pieces = **16 GB**. Too much for a listing,
  so the index has to come from metadata, and a zip is opened only when
  somebody wants that game.

**eXoDOS_Lite.torrent**: 11 files, 6.1 GB, 1 MB pieces, private (tracker
only, no DHT). It holds the metadata - which is already unpacked in
`~/Downloads/eXoDOS`: `xml/all/MS-DOS.xml`, 7,666 `!dos/<DIR>/dosbox.conf`,
and `eXo/Update/index.txt` (`<DIR>:<Title (Year)>`). So the Lite torrent is
only needed by somebody who has no Lite install on disk; that is the last
phase, not the first.

**What the metadata can and cannot say without the zip** (checked against
the 28 zips on disk and all 7,666 confs):

| field in `/list` | from metadata alone |
|---|---|
| title, year, genre, notes | yes (zip name, XML) |
| DIR, the 8.3 folder | yes - `index.txt` predicted 28 of 28 |
| needs a CD | yes - 1,438 confs `imgmount` |
| size in KB | no - only the zip's size; an estimate until fetched |
| start program | the name yes, the extension no: 7,171 confs say `doom`, only 487 say `doom.exe` |
| file list, cue sheets, disc sizes | no |

**The engine.** `libtorrent` does everything needed (piece priorities,
deadlines, a sparse store, resume data, DHT, uTP, seeding). This Mac runs
Python 3.14: PyPI's wheels stop at 3.13, but Homebrew's
`libtorrent-rasterbar` 2.1.1 is bottled against `python@3.14` with the
bindings. `aria2c` and `transmission` only select whole files, and cannot
stream a 444 MB zip in reading order. A pure-Python client would keep
waveserve dependency-free, but it is a BitTorrent client to write and keep
working against real peers; it stays the fallback.

## Design

### 1. `tools/torrentfs.py` - the only module that knows libtorrent

- `Swarm(torrent, cache_dir)`: one session, the torrent added with every
  file at priority 0 so nothing downloads unasked; store in
  `cache_dir/<infohash>/`; resume data saved on a timer and on quit; one
  thread pumping alerts. `want(pieces, urgent)`, `have(piece)`,
  `read(piece)` (through `read_piece`, so libtorrent's part file is its
  business, with a small LRU: 8 pieces = 64 MB), `select(file)` to take a
  whole file, `status()` for the console.
- `TorrentFile(io.RawIOBase)`: a file in the torrent as a seekable,
  read-only file object. A read maps to pieces, asks for them with deadlines
  in reading order plus a read-ahead window, and blocks until they are
  there. `missing(offset, length)` says how much a read would wait for.
- `Tree`: `listdir`, `isfile`, `size`, `open` over the torrent's file list.
- libtorrent is imported only when `--torrent` is given; without it
  waveserve stays stdlib-only, as now.

`zipfile.ZipFile` takes a file object, so everything that reads a zip today
(the directory, a member, a cue sheet, the start of a BIN) works unchanged
over a `TorrentFile`, and fetches exactly the pieces it touches.

### 2. A seam in waveserve: where files come from

`index()` and the handlers use `os.listdir`, `os.path.isfile`, `open` and
`zipfile.ZipFile(path)` directly. They move behind a small `Source`
(`listdir`, `isfile`, `size`, `open`): `DiskSource` is today's behaviour,
`TorrentSource` is the tree above, and `Overlay(disk, torrent)` prefers the
disk - the metadata and the zips already downloaded - and falls through to
the torrent for the rest. `g["zip"]` becomes a source path.

### 3. The index in two stages

- **List** (no swarm traffic): every `eXo/eXoDOS/*.zip` in the torrent's
  file list becomes a game from metadata alone - title and year from the
  name, DIR from `index.txt`, CD flag and program name from the conf, size
  estimated from the zip's length. Marked unresolved.
- **Resolve** (first `/info` or `/pack` for a game): what `index()` does to
  a zip today - the file list, exact sizes, the real start program, cue
  specs, the raw-disc names, the wrappers. `index()` is split so that this
  part is one function of an open `ZipFile`, shared by disk and torrent.
  The result goes into `<cache>/<infohash>.index`, keyed by zip name and length,
  so a restart resolves nothing twice and `/list` gets exact numbers for
  every game ever touched.
- **The start program of an unresolved game**: the list says `WAVE86.BAT`
  (the name the server's wrapper already falls back to), and in torrent
  mode every pack carries that wrapper, written at pack time when the zip
  is open and the real program is known. No change on the DOS side.

### 4. Serving while the data is still arriving

- A `/pack` selects the whole zip at once, so the swarm runs ahead of the
  486 (megabytes a second against 160 KB/s), and then streams as now; reads
  block when they catch up with the download.
- Before each `F` header, if the member's data is not here yet, the server
  waits - and, for a client that asked with `wait=1`, sends
  `W <percent> <text>` every few seconds (the same opt-in as `crc=1`, since
  an old WAVEGET calls an unknown line a bad stream). WAVEGET 0.5 resets its
  120 s idle timer (`nettimeout=`) on a `W` and shows it on the transfer
  screen.
- A stall inside a file longer than the client's timeout ends as a dropped
  transfer; the resume that exists (`from=`, `at=`) picks it up, and the
  server keeps fetching after the drop, so the retry finds the data local.
- `--netdrive` images and `game_disk` are built at index time today; in
  torrent mode they are built on the first request that needs them, behind
  the same `W` lines.
- What we hold is seeded back (libtorrent does this by itself);
  `torrent_upload=` in waveserve.ini caps it (KB/s; 0 no limit, off for none), `torrent_download=` the other way.
- Console: a SWARM line (peers, down and up, cache size, what is being
  fetched and how many pieces are left), and "waiting for the swarm 42%" as
  a transfer's note.

### 5. Command line

    waveserve.py ~/Downloads/eXoDOS --torrent eXoDOS.torrent --cache ~/.wave86/torrent
    make waveserve TORRENT=eXoDOS.torrent CACHE=~/.wave86/torrent

The cache only grows in the first version; its size is on the console and
it can be deleted with the server stopped.

## Phases

0. **Spike** - `brew install libtorrent-rasterbar`; a script that adds the
   torrent with everything at priority 0, fetches the one piece holding
   Keen 4, reads it back, opens the zip. Proves: nothing else downloads,
   neighbours land in the part file rather than as 14,011 empty files,
   resume data works, and how long the first piece takes on the real swarm.
   Decides the engine (fallbacks: a Python 3.13 venv with the PyPI wheel,
   then pure Python).
1. **`torrentfs.py`**, with tests against a fake in-memory swarm (piece
   arithmetic across file boundaries, blocking reads, a zip round trip). No
   libtorrent needed to run them, so CI stays as it is.
2. **Source seam, two-stage index, `<infohash>.index`, `--torrent/--cache`,
   `make waveserve TORRENT=`**. Disk-only behaviour must come out
   byte-identical: compare `/list` and a `/pack` before and after.
3. **`W` lines, WAVEGET 0.5, the console's swarm line.** End-to-end test
   without the internet: a generated three-game collection made into a
   torrent with 16 KB pieces, seeded by a second session on localhost,
   downloaded by the DOS harness.
4. **Lazy NetDrive images and game disks; bootstrap with no Lite install**
   (metadata out of `Content/!DOSmetadata.zip` and the XML out of the 5 GB
   `XODOSMetadata.zip`, from the Lite torrent where its 1 MB pieces make
   that cheap).
5. **Cache budget**: evict least-recently-served games, or the opposite -
   copy finished zips into the Lite install's `eXo/eXoDOS/`, where eXoDOS's
   own launcher counts them as downloaded too.

## Risks and open points

- libtorrent's behaviour with piece priorities inside priority-0 files
  (part file, moving data out when a file is later selected) is from its
  documentation, not yet seen here: that is what phase 0 is for.
- A rare piece can take minutes; `W` lines make that visible rather than
  fast.
- 8 MB is the smallest purchase: a 60 KB game costs a piece (and brings
  its neighbours).
- Sizes in the list are estimates until a game is resolved; the launcher's
  free-space check should lean on the high side (zip x 2, CD games x 1.1).
- The 8 games whose conf names no program get no wrapper; check what the
  launcher does with an `exe=` that is not there.
- Launcher: a "not on the server yet" mark in the network view would need a
  12th list field and a look at the data segment first (see the DGROUP
  limit); not needed for any of the above.
