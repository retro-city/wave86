"""FAT16 volumes written directly, the way a DOS of the era expects them:
plain 8.3 entries, no long names, two FATs, 512 root entries.

Two users: tools/dostest.py fills the hard-disk image it boots (a few
thousand files in under a second, where mtools takes 0.4 s per file), and
tools/waveserve.py wraps CD images in volumes for mTCP NetDrive, which
serves a raw FAT volume as a DOS drive letter.

    format_volume(path, size_mb)            an empty NetDrive-style volume
    with Volume(path) as v: v.add_file(name, size, chunks)   one file in the root
    fill_tree(path, offset, directory)      a whole tree, subdirectories too
"""
import os, re, struct, sys, time

OK_CHARS = "A-Z0-9_\\-!#$%&@^`{}~'"


def format_volume(path, size_mb, nroot=512):
    """An empty FAT16B volume with no partition table, like mTCP NetDrive's
    own `create hd`: reserved sector 1, two FATs of 256 sectors, 512 root
    entries (more for a disc with a crowded root; whole sectors of them),
    media F8, cluster size doubled until the count fits."""
    if not 3 <= size_mb <= 2047:
        raise ValueError("NetDrive volumes are 3 to 2047 MB")
    sectors = size_mb * 1024 * 1024 // 512
    spf, nroot = 256, (nroot + 15) // 16 * 16
    data = sectors - 1 - nroot * 32 // 512 - 2 * spf
    spc = 1
    while data // spc > 65524:
        spc *= 2
    if data // spc < 4085:
        raise ValueError("too small for FAT16")
    bs = bytearray(512)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"NETDRIVE"
    struct.pack_into("<HBHBHHBHHHII", bs, 11, 512, spc, 1, 2, nroot,
                     sectors if sectors < 65536 else 0, 0xF8, spf, 0, 0, 0,
                     sectors if sectors >= 65536 else 0)
    with open(path, "wb") as f:
        f.write(bs)
        for i in range(2):
            f.seek(512 + i * spf * 512)
            f.write(b"\xF8\xFF\xFF\xFF")
        f.truncate(sectors * 512)


def cluster_size(size_mb, nroot=512):
    """The cluster size format_volume would pick for a volume of size_mb."""
    sectors = size_mb * 1024 * 1024 // 512
    data = sectors - 1 - nroot * 32 // 512 - 2 * 256
    spc = 1
    while data // spc > 65524:
        spc *= 2
    return spc * 512


def volume_size_mb(tree, margin_mb=2):
    """(size in MB, root entries) the tree needs: each file and directory
    rounded up to the cluster size the volume will actually have, a root
    directory big enough for its entries, plus a margin."""
    files, dirs = [], []
    root_entries = 0
    for r, ds, fs in os.walk(tree):
        if r == tree:
            root_entries = len(ds) + len(fs)
        else:
            dirs.append(2 + len(ds) + len(fs))
        files += [os.path.getsize(os.path.join(r, f)) for f in fs]
    nroot = max(512, (root_entries + 16 + 15) // 16 * 16)
    mb = 3
    while True:
        cs = cluster_size(mb, nroot)
        need = sum(-(-f // cs) * cs for f in files) + sum(max(1, -(-d * 32 // cs)) * cs for d in dirs)
        need += 512 + 2 * 256 * 512 + nroot * 32
        if need + margin_mb * 1048576 <= mb * 1048576:
            return mb, nroot
        mb = max(mb + 1, int(mb * 1.25))
        if mb > 2047:
            raise ValueError("too big for a FAT16 volume")


class Volume:
    """A FAT16 volume inside a file at a byte offset (0 for a bare volume,
    the partition start for a disk image)."""

    def __init__(self, path, offset=0):
        self.f = open(path, "r+b")
        self.off = offset
        self.f.seek(offset)
        bs = self.f.read(512)
        bps, spc, rsv, nfat, nroot, tot16, _, spf = struct.unpack("<HBHBHHBH", bs[11:24])
        total = tot16 or struct.unpack("<I", bs[32:36])[0]
        self.bps, self.spc, self.nfat, self.nroot, self.spf = bps, spc, nfat, nroot, spf
        self.csize = spc * bps
        self.fat_off = offset + rsv * bps
        self.root_off = self.fat_off + nfat * spf * bps
        self.data_off = self.root_off + nroot * 32
        self.nclus = (total - rsv - nfat * spf - nroot * 32 // bps) // spc
        self.f.seek(self.fat_off)
        self.fat = bytearray(self.f.read(spf * bps))
        self.next = 2
        self.f.seek(self.root_off)
        root = self.f.read(nroot * 32)
        self.root_used = sum(1 for i in range(0, len(root), 32) if root[i] not in (0, 0xE5))
        self.root_end = next((i for i in range(0, len(root), 32) if root[i] == 0), len(root))
        self.names = set()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def alloc(self, n):
        chain, c = [], self.next
        while len(chain) < n:
            if c >= self.nclus + 2:
                raise IOError("volume full")
            if struct.unpack_from("<H", self.fat, c * 2)[0] == 0:
                chain.append(c)
            c += 1
        self.next = c
        for i, c in enumerate(chain):
            struct.pack_into("<H", self.fat, c * 2, chain[i + 1] if i + 1 < len(chain) else 0xFFFF)
        return chain

    def write_chain(self, chain, data):
        for i, c in enumerate(chain):
            self.f.seek(self.data_off + (c - 2) * self.csize)
            self.f.write(data[i * self.csize:(i + 1) * self.csize])

    @staticmethod
    def entry(name, ext, attr, first, size, mtime):
        t = time.localtime(mtime)
        d = (max(t.tm_year, 1980) - 1980) << 9 | t.tm_mon << 5 | t.tm_mday
        tm = t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2
        return (name.ljust(8) + ext.ljust(3)).encode("ascii") + bytes([attr]) + bytes(10) + \
            struct.pack("<HHHI", tm, d, first, size)

    def dos_name(self, fn, used):
        base, _, ext = fn.rpartition(".") if "." in fn else (fn, "", "")
        base = re.sub(f"[^{OK_CHARS}]", "_", base.upper())[:8] or "_"
        ext = re.sub(f"[^{OK_CHARS}]", "", ext.upper())[:3]
        cand, n = (base, ext), 1
        while cand in used:
            n += 1
            cand = (base[:8 - len(f"~{n}")] + f"~{n}", ext)
        used.add(cand)
        return cand

    def add_file(self, fn, size, chunks, mtime=None):
        """One file in the root directory, its content streamed from chunks;
        the clusters are contiguous so the data is written straight through."""
        name, ext = self.dos_name(fn, self.names)
        chain = self.alloc(-(-size // self.csize)) if size else []
        if chain:
            self.f.seek(self.data_off + (chain[0] - 2) * self.csize)
            written = 0
            for chunk in chunks:
                self.f.write(chunk)
                written += len(chunk)
            if written != size:
                raise IOError(f"{fn}: wrote {written} of {size} bytes")
        if self.root_end + 32 > self.nroot * 32:
            raise IOError("root directory full")
        self.f.seek(self.root_off + self.root_end)
        self.f.write(self.entry(name, ext, 0x20, chain[0] if chain else 0, size, mtime or time.time()))
        self.root_end += 32
        return f"{name}.{ext}" if ext else name

    def close(self):
        for i in range(self.nfat):
            self.f.seek(self.fat_off + i * self.spf * self.bps)
            self.f.write(self.fat)
        self.f.close()


def fill_tree(img, off, src):
    """Write the tree under src into the (empty) FAT16 volume at byte offset
    off of img, subdirectories included."""
    v = Volume(img, off)
    csize = v.csize

    def put_dir(path, first, parent_first, fixed=None):
        names = sorted(n for n in os.listdir(path) if not n.startswith("."))
        # subdirectories start with . and ..; the root has neither
        entries = v.entry(".", "", 0x10, first, 0, os.path.getmtime(path)) + \
            v.entry("..", "", 0x10, parent_first, 0, os.path.getmtime(path)) if fixed else b""
        used = set()
        for n in names:
            p = os.path.join(path, n)
            name, ext = v.dos_name(n, used)
            st = os.stat(p)
            if os.path.isdir(p):
                count = 2 + sum(1 for x in os.listdir(p) if not x.startswith("."))
                chain = v.alloc(max(1, -(-count * 32 // csize)))
                entries += v.entry(name, ext, 0x10, chain[0], 0, st.st_mtime)
                put_dir(p, chain[0], first, chain)
            else:
                data = open(p, "rb").read()
                chain = v.alloc(-(-len(data) // csize)) if data else []
                if chain:
                    v.write_chain(chain, data)
                entries += v.entry(name, ext, 0x20, chain[0] if chain else 0, len(data), st.st_mtime)
        if fixed is None:                       # the root directory area
            if len(entries) > v.nroot * 32:
                raise IOError("too many entries in the root directory")
            v.f.seek(v.root_off)
            v.f.write(entries.ljust(v.nroot * 32, b"\0"))
        else:
            v.write_chain(fixed, entries.ljust(len(fixed) * csize, b"\0"))

    put_dir(src, 0, 0)
    v.close()
