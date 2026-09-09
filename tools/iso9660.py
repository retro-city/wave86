"""Read the files out of an ISO 9660 image (level 1, the way DOS-era CDs
were made): the primary volume descriptor, then the directory tree by
its records. No Joliet, no Rock Ridge, no multi-extent files; a game
disc has none of those.

    for path, size, read in walk(iso): ...      path like "S2\\DATA\\X.DAT"
    extract(iso, directory)                     the whole tree on disk
"""
import os, struct


def _records(f, extent, size):
    """The directory records of one directory: (name, extent, size, is_dir)."""
    f.seek(extent * 2048)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        n = data[pos]
        if n == 0:                          # padding to the sector end
            pos = (pos // 2048 + 1) * 2048
            continue
        ext = struct.unpack_from("<I", data, pos + 2)[0]
        sz = struct.unpack_from("<I", data, pos + 10)[0]
        flags = data[pos + 25]
        nl = data[pos + 32]
        name = data[pos + 33:pos + 33 + nl]
        pos += n
        if name in (b"\x00", b"\x01"):      # . and ..
            continue
        name = name.decode("ascii", "replace").split(";")[0].rstrip(".")
        yield name, ext, sz, bool(flags & 2)


def walk(iso, prefix=""):
    """(DOS path, size, extent) for every file, directories first-hand."""
    with open(iso, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            raise ValueError("not an ISO 9660 image")
        root_ext = struct.unpack_from("<I", pvd, 156 + 2)[0]
        root_size = struct.unpack_from("<I", pvd, 156 + 10)[0]
        stack = [("", root_ext, root_size)]
        while stack:
            path, ext, size = stack.pop()
            for name, e, sz, is_dir in _records(f, ext, size):
                full = f"{path}\\{name}" if path else name
                if is_dir:
                    stack.append((full, e, sz))
                else:
                    yield full, sz, e


def extract(iso, directory):
    """The disc's tree under directory; returns (files, bytes)."""
    n = total = 0
    with open(iso, "rb") as f:
        for path, size, extent in walk(iso):
            out = os.path.join(directory, *path.split("\\"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            f.seek(extent * 2048)
            with open(out, "wb") as o:
                left = size
                while left > 0:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    o.write(chunk)
                    left -= len(chunk)
            n += 1
            total += size
    return n, total
