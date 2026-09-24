#!/usr/bin/env python3
"""Tests for torrentfs that need neither libtorrent nor a network: a small
collection is made into a torrent in memory, and a swarm that lives in
memory hands out its pieces - late, if a test wants to see a read wait.

    python3 tools/test_torrentfs.py
"""
import os, io, sys, time, random, hashlib, zipfile, tempfile, threading, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import torrentfs

PIECE = 16384


def bencode(v):
    if isinstance(v, int):
        return b"i%de" % v
    if isinstance(v, bytes):
        return b"%d:%s" % (len(v), v)
    if isinstance(v, str):
        return bencode(v.encode())
    if isinstance(v, list):
        return b"l" + b"".join(bencode(x) for x in v) + b"e"
    return b"d" + b"".join(bencode(k) + bencode(x) for k, x in sorted(v.items())) + b"e"


def make_zip(members):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for name, data in members:
            z.writestr(name, data)
    return buf.getvalue()


def make_torrent(files, folder="eXoDOS", pad_after=None):
    """files: [(path, bytes)] -> (the .torrent's bytes, the payload)"""
    payload = b"".join(data for _, data in files)
    entries = []
    for path, data in files:
        entries.append({b"length": len(data), b"path": [p.encode() for p in path.split("/")]})
        if path == pad_after:
            pad = bytes(100)
            entries.append({b"length": len(pad), b"path": [b".pad", b"100"], b"attr": b"p"})
            payload = payload.replace(data, data + pad, 1)
    hashes = b"".join(hashlib.sha1(payload[i:i + PIECE]).digest() for i in range(0, len(payload), PIECE))
    info = {b"name": folder.encode(), b"piece length": PIECE, b"pieces": hashes, b"files": entries}
    return bencode({b"announce": b"udp://nowhere:1/announce", b"info": info}), payload, info


class FakeSwarm:
    """have/want/read over bytes in memory. A piece becomes 'here' the first
    time it is read, after `delay` seconds; every fetch is written down."""

    def __init__(self, torrent, payload, delay=0.0):
        self.t, self.payload, self.delay = torrent, payload, delay
        self.here, self.fetched, self.wants = set(), [], []
        self.lock = threading.Lock()

    def have(self, piece):
        return piece in self.here

    def want(self, pieces, urgent=False):
        self.wants.append((list(pieces), urgent))

    def read(self, piece, timeout=None):
        with self.lock:
            new = piece not in self.here
        if new:
            if timeout is not None and timeout < self.delay:
                raise TimeoutError(piece)
            time.sleep(self.delay)
            with self.lock:
                self.here.add(piece)
                self.fetched.append(piece)
        data = self.payload[piece * PIECE:(piece + 1) * PIECE]
        assert hashlib.sha1(data).digest() == self.t.hashes[piece * 20:piece * 20 + 20]
        return data


class TorrentFsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rnd = random.Random(86)
        cls.big = ("CD/GAME.BIN", rnd.randbytes(20 * PIECE + 321))
        cls.games = {
            "Alpha (1990)": make_zip([("ALPHA/ALPHA.EXE", rnd.randbytes(3000)), ("ALPHA/DATA.DAT", rnd.randbytes(700))]),
            "Beta (1991)": make_zip([("BETA/START.BAT", b"@echo off\r\nbeta\r\n"), ("BETA/BETA.EXE", rnd.randbytes(5 * PIECE)),
                                     ("BETA/" + cls.big[0], cls.big[1]), ("BETA/CD/GAME.CUE", b'FILE "GAME.BIN" BINARY\r\n')]),
            "Gamma (1992)": make_zip([("GAMMA/G.COM", rnd.randbytes(90))]),
        }
        cls.files = [("eXoDOS Manual.pdf", rnd.randbytes(PIECE + 17))]
        cls.files += [(f"eXo/eXoDOS/{name}.zip", data) for name, data in cls.games.items()]
        cls.files += [("Content/!DOSmetadata.zip", make_zip([("eXo/eXoDOS/!dos/ALPHA/dosbox.conf", b"[autoexec]\nalpha\n")]))]
        raw, cls.payload, cls.info = make_torrent(cls.files, pad_after="eXoDOS Manual.pdf")
        cls.tmp = tempfile.NamedTemporaryFile(suffix=".torrent", delete=False)
        cls.tmp.write(raw)
        cls.tmp.close()
        cls.t = torrentfs.Torrent(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.tmp.name)

    def tree(self, delay=0.0):
        swarm = FakeSwarm(self.t, self.payload, delay)
        return torrentfs.Tree(self.t, swarm), swarm

    def test_torrent(self):
        t = self.t
        self.assertEqual(t.infohash, hashlib.sha1(bencode(self.info)).hexdigest())
        self.assertEqual(t.piece_length, PIECE)
        self.assertEqual(t.size, len(self.payload))
        self.assertEqual(t.num_pieces, (len(self.payload) + PIECE - 1) // PIECE)
        self.assertEqual([p for p, _, _ in t.files], [p for p, _ in self.files])    # no padding in the list
        for path, off, size in t.files:             # and the padding is counted in what follows it
            self.assertEqual(self.payload[off:off + size], dict(self.files)[path])
        self.assertEqual(list(t.pieces(PIECE - 1, 2)), [0, 1])
        self.assertEqual(list(t.pieces(PIECE, PIECE)), [1])
        self.assertEqual(list(t.pieces(5, 0)), [])
        self.assertEqual(t.piece_size(t.num_pieces - 1), len(self.payload) - (t.num_pieces - 1) * PIECE)

    def test_tree(self):
        tree, _ = self.tree()
        self.assertEqual(tree.listdir(""), ["Content", "eXo", "eXoDOS Manual.pdf"])
        self.assertEqual(tree.listdir("eXo/eXoDOS"), [f"{n}.zip" for n in sorted(self.games)])
        self.assertEqual(tree.listdir("eXo\\eXoDOS\\"), tree.listdir("./eXo/eXoDOS"))
        self.assertTrue(tree.isdir("eXo") and not tree.isfile("eXo"))
        self.assertTrue(tree.isfile("eXo/eXoDOS/Gamma (1992).zip"))
        self.assertEqual(tree.size("eXo/eXoDOS/Gamma (1992).zip"), len(self.games["Gamma (1992)"]))
        self.assertRaises(FileNotFoundError, tree.listdir, "eXo/nothing")
        self.assertRaises(FileNotFoundError, tree.open, "eXo/eXoDOS/Delta (1993).zip")

    def test_reads_match(self):
        tree, _ = self.tree()
        rnd = random.Random(1)
        for path, data in self.files:
            with tree.open(path) as f:
                self.assertEqual(f.read(), data, path)
                for _ in range(40):
                    at, n = rnd.randrange(len(data) + 5), rnd.randrange(3 * PIECE)
                    f.seek(at)
                    self.assertEqual(f.read(n), data[at:at + n], (path, at, n))
                f.seek(-10, 2)
                self.assertEqual(f.read(), data[-10:])
                self.assertEqual(f.read(5), b"")

    def test_only_what_is_touched(self):
        """a zip's directory and one small member cost their pieces and no more"""
        tree, swarm = self.tree()
        path = "eXo/eXoDOS/Beta (1991).zip"
        with tree.open(path) as f, zipfile.ZipFile(f) as z:
            total = len(f.raw.pieces())
            self.assertGreater(total, 25)
            names = z.namelist()
            after_dir = len(swarm.fetched)
            self.assertLessEqual(after_dir, 2)          # the end of the zip, nothing else
            self.assertEqual(z.read("BETA/CD/GAME.CUE"), b'FILE "GAME.BIN" BINARY\r\n')
            self.assertEqual(z.read("BETA/START.BAT"), b"@echo off\r\nbeta\r\n")
            self.assertLessEqual(len(swarm.fetched), 3)
            self.assertEqual(swarm.wants, [])           # looking into a file asks for nothing ahead
            self.assertEqual(f.raw.missing(), total - len(swarm.fetched))
            self.assertEqual(z.read("BETA/" + self.big[0]), self.big[1])
            self.assertGreater(f.raw.missing(), 0)      # BETA.EXE: nobody has read it
            z.read("BETA/BETA.EXE")
            self.assertEqual(f.raw.missing(), 0)
        self.assertEqual(len(names), 4)

    def test_readahead_and_prefetch(self):
        tree, swarm = self.tree()
        with tree.open("eXo/eXoDOS/Beta (1991).zip", readahead=2) as f:
            first = f.raw.pieces()[0]
            self.assertEqual(f.raw.prefetch(), len(f.raw.pieces()))
            self.assertEqual(swarm.wants[-1], (list(f.raw.pieces()), False))
            f.read(10)
            self.assertEqual(swarm.wants[-1], ([first + 1, first + 2], True))
            f.read(PIECE)                               # into the next piece: one more announced
            self.assertEqual(swarm.wants[-1], ([first + 2, first + 3], True))
            f.seek(-5, 2)
            n = len(swarm.wants)
            f.read()                                    # the last piece: nothing beyond the file
            self.assertEqual(len(swarm.wants), n)

    def test_a_read_waits(self):
        tree, swarm = self.tree(delay=0.3)
        with tree.open("eXo/eXoDOS/Gamma (1992).zip") as f:
            t0 = time.time()
            data = f.read()
            self.assertGreaterEqual(time.time() - t0, 0.25)
            self.assertEqual(data, self.games["Gamma (1992)"])
            t0 = time.time()
            f.seek(0)
            f.read()
            self.assertLess(time.time() - t0, 0.2)      # here now


if __name__ == "__main__":
    unittest.main()
