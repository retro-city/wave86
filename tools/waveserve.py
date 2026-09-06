#!/usr/bin/env python3
"""waveserve - serve an eXoDOS collection to WAVE86 over plain HTTP/1.0.

    waveserve.py ~/Downloads/eXoDOS --port 8086

    GET /list          one game per line: id|DIR|Title|year|genre|KB
    GET /info/<id>     a few lines about one game
    GET /pack/<id>     the game's files as a stream the 486 writes straight
                       into C:/GAMES/DIR (DOS backslashes on the wire): "F <bytes> <DOS path>\n" + data
                       for each file, then "E\n". Nothing to unzip on DOS.

Games are the "Title (Year).zip" files under eXo/eXoDOS; each holds one
8.3-named folder of plain game files. Zips still downloading (.part) and
games above --max-mb are left out of the list.
"""
import os, re, sys, zipfile, argparse, unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GAMES = []          # dicts: title, year, dir, zip, kb, files
DOSNAME = re.compile(r'^[A-Z0-9_$~!#%&\-@^\'`(){}]{1,8}(\.[A-Z0-9_$~!#%&\-@^\'`(){}]{1,3})?$')


def ascii_title(s):
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return re.sub(r'[|\r\n]', ' ', s).strip()


def dos_path(parts):
    """DIR-relative path in DOS form, or None if a component is not 8.3."""
    out = []
    for p in parts:
        p = p.upper()
        if not DOSNAME.match(p):
            return None
        out.append(p)
    return "\\".join(out)


def index(root, max_mb):
    zdir = os.path.join(root, "eXo", "eXoDOS")
    for fn in sorted(os.listdir(zdir)):
        m = re.match(r'^(.*) \((\d{4})\)\.zip$', fn)
        if not m:
            continue
        path = os.path.join(zdir, fn)
        try:
            with zipfile.ZipFile(path) as z:
                infos = [i for i in z.infolist() if not i.is_dir()]
        except zipfile.BadZipFile:
            continue
        if not infos:
            continue
        top = infos[0].filename.split("/")[0]
        files, total = [], 0
        for i in infos:
            parts = i.filename.split("/")
            if parts[0] != top or len(parts) < 2 or parts[-1].lower().endswith(".exo"):
                continue
            rel = dos_path(parts[1:])
            if rel is None:
                continue          # not representable on DOS
            files.append((rel, i.filename, i.file_size))
            total += i.file_size
        if total > max_mb * 1024 * 1024 or not files:
            continue
        GAMES.append(dict(title=ascii_title(m.group(1)), year=m.group(2),
                          dir=top.upper(), zip=path, kb=(total + 1023) // 1024,
                          files=files, genre=""))
    GAMES.sort(key=lambda g: g["title"].lower())


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def _head(self, ctype, length=None):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):
        p = self.path
        if p == "/list":
            body = "".join(f"{i}|{g['dir']}|{g['title'][:40]}|{g['year']}|{g['genre'][:12]}|{g['kb']}\r\n"
                           for i, g in enumerate(GAMES)).encode("ascii", "replace")
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        m = re.match(r'^/(info|pack)/(\d+)$', p)
        if not m or int(m.group(2)) >= len(GAMES):
            self.send_error(404)
            return
        g = GAMES[int(m.group(2))]
        if m.group(1) == "info":
            body = (f"{g['title']} ({g['year']})\r\nDIR {g['dir']}\r\n"
                    f"FILES {len(g['files'])}\r\nKB {g['kb']}\r\n").encode("ascii", "replace")
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        self._head("application/octet-stream")
        with zipfile.ZipFile(g["zip"]) as z:
            for rel, name, size in g["files"]:
                self.wfile.write(f"F {size} {rel}\n".encode())
                with z.open(name) as f:
                    while True:
                        chunk = f.read(65536)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
        self.wfile.write(b"E\n")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % args))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--port", type=int, default=8086)
    ap.add_argument("--max-mb", type=int, default=30)
    a = ap.parse_args()
    index(a.root, a.max_mb)
    print(f"waveserve: {len(GAMES)} games from {a.root}, port {a.port}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
