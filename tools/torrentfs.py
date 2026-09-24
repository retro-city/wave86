#!/usr/bin/env python3
"""torrentfs - the files inside a torrent, read as if they were on disk.

    t = Torrent("eXoDOS.torrent")           what is in it: no network, no libtorrent
    swarm = Swarm(t, "~/wave86-cache")      the pieces: fetched when asked for, kept
    tree = Tree(t, swarm)
    with tree.open("eXo/eXoDOS/Commander Keen 4 - Secret of the Oracle (1991).zip") as f:
        zipfile.ZipFile(f).namelist()       costs the pieces the zip's directory is in

A torrent is one long run of bytes cut into pieces; a file is an offset and
a length in it. A read maps to pieces, asks the swarm for the ones that are
not here yet - the one being read first, a few ahead of it next - and
blocks until they are. Nothing is downloaded that was not asked for: every
file starts at priority 0, and what arrives goes into libtorrent's part
file rather than into 14,011 files on disk. What is held is seeded back.

Only Swarm needs libtorrent (brew install libtorrent-rasterbar). Torrent,
Tree and TorrentFile are plain Python, and anything with Swarm's have/
want/read can stand in for it - the tests use one that lives in memory.

    torrentfs.py <torrent> <cache> ls [folder]
    torrentfs.py <torrent> <cache> get <file in the torrent> <file to write>
"""
import os, io, sys, time, hashlib, threading, collections


# ---- the .torrent file ------------------------------------------------------

def bdecode(b, i=0, spans=None):
    """(value, where it ended). spans, if given, gets the byte range of each
    value in the outermost dict: the info-hash is the SHA-1 of one of them."""
    c = b[i:i + 1]
    if c == b"i":
        e = b.index(b"e", i)
        return int(b[i + 1:e]), e + 1
    if c == b"l":
        i += 1
        out = []
        while b[i:i + 1] != b"e":
            v, i = bdecode(b, i)
            out.append(v)
        return out, i + 1
    if c == b"d":
        i += 1
        out = {}
        while b[i:i + 1] != b"e":
            k, i = bdecode(b, i)
            v, e = bdecode(b, i)
            if spans is not None:
                spans[k] = (i, e)
            out[k], i = v, e
        return out, i + 1
    colon = b.index(b":", i)
    n = int(b[i:colon])
    return b[colon + 1:colon + 1 + n], colon + 1 + n


class Torrent:
    """A .torrent file: the piece size, and where each file lies in the run
    of bytes. Paths are relative to the torrent's own folder, '/' between
    the parts, so they match a download of it on disk."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            raw = f.read()
        spans = {}
        top, _ = bdecode(raw, 0, spans)
        info = top[b"info"]
        if b"pieces" not in info:
            raise ValueError(f"{path}: a v2-only torrent, which this cannot read")
        self.infohash = hashlib.sha1(raw[spans[b"info"][0]:spans[b"info"][1]]).hexdigest()
        self.name = info[b"name"].decode("utf-8", "replace")
        self.piece_length = info[b"piece length"]
        self.hashes = info[b"pieces"]
        self.num_pieces = len(self.hashes) // 20
        self.private = bool(info.get(b"private"))
        self.files = []                         # (path, offset, size)
        off = 0
        if b"files" in info:
            for f in info[b"files"]:
                if b"p" not in f.get(b"attr", b""):     # padding is not a file
                    self.files.append(("/".join(p.decode("utf-8", "replace") for p in f[b"path"]),
                                       off, f[b"length"]))
                off += f[b"length"]
        else:
            self.files.append((self.name, 0, info[b"length"]))
            off = info[b"length"]
        self.size = off

    def pieces(self, offset, length):
        """the pieces that hold these bytes"""
        if length <= 0:
            return range(0)
        return range(offset // self.piece_length, (offset + length - 1) // self.piece_length + 1)

    def piece_size(self, piece):
        return min(self.piece_length, self.size - piece * self.piece_length)


# ---- a file in it -----------------------------------------------------------

class TorrentFile(io.RawIOBase):
    """One file of the torrent: seekable, read-only, and slow the first
    time. Tree.open() wraps it in a BufferedReader, which is what makes
    read(n) return n bytes across a piece boundary; .raw gets back here."""

    def __init__(self, torrent, swarm, name, offset, size, readahead=0):
        self.t, self.swarm, self.name = torrent, swarm, name
        self.offset, self.size, self.pos = offset, size, 0
        self.readahead = readahead              # pieces to ask for beyond the one being read
        self.last = -1                          # the piece the last read was in

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, pos, whence=0):
        self.pos = max(0, pos + (0, self.pos, self.size)[whence])
        return self.pos

    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0:
            return 0
        at = self.offset + self.pos
        piece = at // self.t.piece_length
        if self.readahead and piece != self.last:
            # somebody reading straight through gets here next: have it coming
            end = (self.offset + self.size - 1) // self.t.piece_length
            ahead = [p for p in range(piece + 1, min(end, piece + self.readahead) + 1)
                     if not self.swarm.have(p)]
            if ahead:
                self.swarm.want(ahead, urgent=True)
        self.last = piece
        data = self.swarm.read(piece)
        start = at - piece * self.t.piece_length
        chunk = data[start:start + n]
        b[:len(chunk)] = chunk
        self.pos += len(chunk)
        return len(chunk)

    def missing(self, start=0, length=None):
        """how many of the pieces under these bytes are not here yet"""
        length = self.size - start if length is None else length
        return sum(1 for p in self.t.pieces(self.offset + start, length) if not self.swarm.have(p))

    def pieces(self, start=0, length=None):
        length = self.size - start if length is None else length
        return self.t.pieces(self.offset + start, length)

    def prefetch(self, start=0, length=None):
        """ask for these bytes without waiting for them"""
        want = [p for p in self.pieces(start, length) if not self.swarm.have(p)]
        if want:
            self.swarm.want(want)
        return len(want)


class Tree:
    """listdir, isfile, open: the torrent's file list as a folder tree"""

    def __init__(self, torrent, swarm):
        self.t, self.swarm = torrent, swarm
        self.by_path = {p: (off, size) for p, off, size in torrent.files}
        self.dirs = collections.defaultdict(set)
        for p in self.by_path:
            parts = p.split("/")
            for k in range(len(parts)):
                self.dirs["/".join(parts[:k])].add(parts[k])

    @staticmethod
    def norm(path):
        return "/".join(x for x in path.replace("\\", "/").split("/") if x and x != ".")

    def listdir(self, path=""):
        path = self.norm(path)
        if path not in self.dirs:
            raise FileNotFoundError(path)
        return sorted(self.dirs[path])

    def isdir(self, path):
        return self.norm(path) in self.dirs

    def isfile(self, path):
        return self.norm(path) in self.by_path

    def size(self, path):
        return self.by_path[self.norm(path)][1]

    def raw(self, path, readahead=0):
        path = self.norm(path)
        if path not in self.by_path:
            raise FileNotFoundError(path)
        off, size = self.by_path[path]
        return TorrentFile(self.t, self.swarm, path, off, size, readahead)

    def open(self, path, readahead=0):
        """readahead=0 to look into a file (a zip's directory, a cue sheet)
        and pay for nothing else; 2 or so to read it from end to end"""
        return io.BufferedReader(self.raw(path, readahead), 1 << 16)


# ---- the pieces -------------------------------------------------------------

class Swarm:
    """The pieces of one torrent, by way of libtorrent.

    have(piece)             is it here
    want(pieces, urgent)    get these; urgent ones first and in this order
    read(piece, timeout)    its bytes, waiting for it if it has to be fetched
    status()                a dict for whoever shows what is going on
    close()                 write down what we hold, for the next start
    """

    ALERT_WHEN_AVAILABLE = 1

    def __init__(self, torrent, cache_dir, port=6881, up_kb=1024, down_kb=0, interface="", dht=True,
                 connections=200, log=None, keep=8, portmap=False):
        """up_kb: KB/s given back at most, 0 for no limit, None for no
        uploading at all; down_kb: taken at most, 0 for no limit;
        interface: an address or device name to listen and call out on,
        empty for all of them; port: where peers reach us."""
        import libtorrent as lt
        self.lt, self.t = lt, torrent
        self.log = log or (lambda text: None)
        cache_dir = os.path.abspath(os.path.expanduser(cache_dir))
        self.store = os.path.join(cache_dir, torrent.infohash)
        self.resume_path = os.path.join(cache_dir, torrent.infohash + ".resume")
        os.makedirs(self.store, exist_ok=True)
        self.cond = threading.Condition()
        self.held = collections.OrderedDict()   # piece -> bytes, the last few read
        self.keep = keep
        self.asked = set()                      # reads libtorrent owes us
        self.prio = {}                          # piece -> the priority we gave it
        self.wanted = set()                     # asked for and not here yet, for status()
        self.failed = {}                        # piece -> why a read of it failed
        self.error = None
        self.dirty = False                      # pieces came in since resume data was written
        self.saved_once = False
        self.stopping = False
        cat = lt.alert.category_t
        settings = {
            "listen_interfaces": f"{interface}:{port}" if interface else f"0.0.0.0:{port},[::]:{port}",
            "alert_mask": cat.status_notification | cat.storage_notification | cat.error_notification,
            "alert_queue_size": 10000,
            "enable_dht": bool(dht),
            "connections_limit": int(connections),
            # asking the router to open our port is for whoever runs this to
            # decide; without it we still reach every peer that can be reached
            "enable_upnp": portmap,
            "enable_natpmp": portmap,
        }
        if interface:
            settings["outgoing_interfaces"] = interface
        if up_kb is None:
            settings["unchoke_slots_limit"] = 0         # nobody is ever unchoked: no upload
        elif up_kb > 0:
            settings["upload_rate_limit"] = int(up_kb) * 1024
        if down_kb and down_kb > 0:
            settings["download_rate_limit"] = int(down_kb) * 1024
        self.ses = lt.session(settings)
        ti = lt.torrent_info(torrent.path)
        atp = None
        if os.path.isfile(self.resume_path):
            try:
                atp = lt.read_resume_data(open(self.resume_path, "rb").read())
            except Exception as e:
                self.log(f"torrent: resume data unreadable ({e}); checking the cache instead")
        if atp is None:
            atp = lt.add_torrent_params()
        atp.ti = ti
        atp.save_path = self.store
        atp.flags |= lt.torrent_flags.default_dont_download
        atp.flags &= ~lt.torrent_flags.auto_managed
        atp.flags &= ~lt.torrent_flags.paused
        self.h = self.ses.add_torrent(atp)
        self.saved = time.time()
        self.thread = threading.Thread(target=self.pump, daemon=True)
        self.thread.start()

    # libtorrent's handle calls are messages to its own thread; none of them
    # waits on ours, so they are safe to make while holding self.cond

    def have(self, piece):
        return self.h.have_piece(piece)

    def want(self, pieces, urgent=False):
        with self.cond:
            for k, p in enumerate(pieces):
                self.wanted.add(p)
                if urgent:
                    if self.prio.get(p, 0) < 7:
                        self.prio[p] = 7
                        self.h.piece_priority(p, 7)
                    self.h.set_piece_deadline(p, 1000 * (k + 1), 0)
                elif self.prio.get(p, 0) < 1:
                    self.prio[p] = 1
                    self.h.piece_priority(p, 1)

    def read(self, piece, timeout=None):
        end = None if timeout is None else time.time() + timeout
        with self.cond:
            while True:
                data = self.held.get(piece)
                if data is not None:
                    self.held.move_to_end(piece)
                    return data
                if self.error:
                    raise OSError(f"torrent: {self.error}")
                if piece in self.failed:
                    raise OSError(f"torrent: piece {piece}: {self.failed.pop(piece)}")
                if self.stopping:
                    raise OSError("torrent: shutting down")
                if piece not in self.asked:
                    self.asked.add(piece)
                    self.wanted.add(piece)
                    if self.prio.get(piece, 0) < 7:
                        self.prio[piece] = 7
                        self.h.piece_priority(piece, 7)
                    # here already or not, this ends in a read_piece_alert
                    self.h.set_piece_deadline(piece, 0, self.ALERT_WHEN_AVAILABLE)
                if end is not None and time.time() >= end:
                    raise TimeoutError(f"torrent: piece {piece} did not arrive")
                if not self.cond.wait(1.0) and piece not in self.held and self.h.have_piece(piece):
                    self.h.read_piece(piece)    # an alert went missing: ask again

    def pump(self):
        lt = self.lt
        while not self.stopping:
            self.ses.wait_for_alert(500)
            for a in self.ses.pop_alerts():
                kind = type(a).__name__
                if kind == "read_piece_alert":
                    with self.cond:
                        self.asked.discard(a.piece)
                        if a.error.value():
                            self.failed[a.piece] = a.error.message()
                        else:
                            self.held[a.piece] = bytes(a.buffer)
                            self.held.move_to_end(a.piece)
                            while len(self.held) > self.keep:
                                self.held.popitem(last=False)
                            if a.piece in self.wanted:
                                self.wanted.discard(a.piece)
                                self.dirty = True
                        self.cond.notify_all()
                elif kind == "save_resume_data_alert":
                    tmp = self.resume_path + ".new"
                    with open(tmp, "wb") as f:
                        f.write(lt.write_resume_data_buf(a.params))
                    os.replace(tmp, self.resume_path)
                    with self.cond:
                        self.saved_once = True
                        self.cond.notify_all()
                elif kind in ("torrent_error_alert", "file_error_alert"):
                    with self.cond:
                        self.error = a.message()
                        self.cond.notify_all()
                    self.log(f"torrent: {a.message()}")
                elif kind in ("hash_failed_alert", "save_resume_data_failed_alert", "fastresume_rejected_alert",
                              "listen_failed_alert"):
                    self.log(f"torrent: {a.message()}")
            if time.time() - self.saved > 30:
                self.saved = time.time()
                if self.dirty or self.wanted:
                    self.dirty = False
                    self.h.save_resume_data()

    def status(self):
        with self.cond:
            asked = bool(self.wanted)
        s = self.h.status(self.lt.status_flags_t.query_pieces) if asked else self.h.status()
        with self.cond:
            if asked:                           # what came in without anybody reading it
                here = s.pieces
                gone = {p for p in self.wanted if here[p]}
                if gone:
                    self.wanted -= gone
                    self.dirty = True
            waiting = len(self.wanted)
        return dict(peers=s.num_peers, seeds=s.num_seeds, down=s.download_payload_rate, up=s.upload_payload_rate,
                    got=s.total_payload_download, sent=s.total_payload_upload,
                    held=s.num_pieces * self.t.piece_length, waiting=waiting,
                    state=str(s.state), error=self.error)

    def close(self):
        with self.cond:
            self.saved_once = False
            self.h.save_resume_data()
            end = time.time() + 10
            while not self.saved_once and time.time() < end:
                self.cond.wait(0.5)
            self.stopping = True
            self.cond.notify_all()
        self.thread.join(2)
        self.ses.pause()


def main():
    if len(sys.argv) < 4 or sys.argv[3] not in ("ls", "get"):
        sys.exit(__doc__.strip().split("\n\n")[-1])
    t = Torrent(sys.argv[1])
    if sys.argv[3] == "ls":
        tree = Tree(t, None)
        folder = sys.argv[4] if len(sys.argv) > 4 else ""
        for name in tree.listdir(folder):
            p = Tree.norm(folder + "/" + name)
            print(f"{tree.size(p):>14,}  {name}" if tree.isfile(p) else f"{'<DIR>':>14}  {name}")
        return
    swarm = Swarm(t, sys.argv[2], log=print)
    try:
        t0 = time.time()
        with Tree(t, swarm).open(sys.argv[4], readahead=2) as f, open(sys.argv[5], "wb") as out:
            n = f.raw.prefetch()
            print(f"{f.raw.size:,} bytes in {len(f.raw.pieces())} pieces, {n} of them to fetch")
            while True:
                buf = f.read(1 << 20)
                if not buf:
                    break
                out.write(buf)
                s = swarm.status()
                print(f"\r{out.tell() * 100 // max(1, f.raw.size):3d}%  {s['peers']} peers  "
                      f"{s['down'] / 1024:.0f} KB/s   ", end="", flush=True)
        print(f"\ndone in {time.time() - t0:.1f} s")
    finally:
        swarm.close()


if __name__ == "__main__":
    main()
