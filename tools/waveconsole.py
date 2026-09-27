"""waveserve's console: what the server is doing, on the terminal it was
started from.

    transfers under way, with a bar, the rate and the file being sent
    the log, or the game list (g), scrolling with the arrow keys
    q quits the server, r re-indexes the collection now

The layout is render(): a pure function from the server's state to rows of
(text, style), so it can be looked at and tested without a terminal. run()
paints those rows with curses, four times a second, and turns keys into
actions. Nothing else here knows about curses, and waveserve only imports
this module when it has a terminal to draw on.
"""
import collections, time

STYLES = ("title", "head", "dim", "ok", "bad", "warn", "plain")

RATE_WINDOW = 3.0       # seconds a KB/s figure is averaged over


def fmt_size(b):
    """bytes as MB or KB, the way the DOS side says it"""
    if b >= 100 * 1048576:
        return f"{b / 1048576:.0f} MB"
    if b >= 1048576:
        return f"{b / 1048576:.1f} MB"
    return f"{b / 1024:.0f} KB"


def fmt_time(s):
    s = int(s)
    if s >= 3600:
        return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"
    return f"{s // 60}:{s % 60:02d}"


def bar(frac, width, ascii_only):
    n = max(0, min(width, int(round(frac * width))))
    if ascii_only:
        return "#" * n + "." * (width - n)
    return "█" * n + "░" * (width - n)


class Rates:
    """KB/s for each transfer, from samples taken as the console ticks:
    the difference over the last few seconds, not since the start, so a
    stall shows as one."""

    def __init__(self):
        self.hist = collections.defaultdict(lambda: collections.deque(maxlen=40))

    def sample(self, transfers, now):
        for t in transfers:
            if not t["ended"]:
                self.hist[t["id"]].append((now, t["bytes"]))
        for tid in [k for k in self.hist if k not in {t["id"] for t in transfers}]:
            del self.hist[tid]

    def rate(self, t, now):
        h = self.hist.get(t["id"])
        if not h or len(h) < 2:
            return None
        t0, b0 = h[0]
        for ts, bs in h:                    # the oldest sample inside the window
            if now - ts <= RATE_WINDOW:
                break
            t0, b0 = ts, bs
        tn, bn = h[-1]
        return (bn - b0) / (tn - t0) if tn > t0 else None


def transfer_row(t, now, rate, width, ascii_only):
    """one transfer on one line, cut to width"""
    who = f"{t['client']:<15.15}"
    what = f"{t['what']:<18.18}"
    if t["ended"]:
        took = fmt_time(t["ended"] - t["started"])
        if t["status"] == "done":
            tail = f"done  {fmt_size(t['bytes'])} in {took}"
            style = "ok"
        elif t["status"] == "dropped":
            tail = f"the client went away at {fmt_size(t['bytes'])}, after {took}"
            style = "warn"
        else:
            tail = f"{t['status']}"
            style = "bad"
        return (f" {who} {what} {tail}"[:width], style)
    total = t["total"]
    if total:
        frac = min(1.0, t["bytes"] / total)
        pct = f"{frac * 100:3.0f}%"
        sizes = f"{fmt_size(t['bytes'])}/{fmt_size(total)}"
        prog = bar(frac, 10, ascii_only)
    else:
        pct, sizes, prog = "", fmt_size(t["bytes"]), ""
    if rate is not None and rate > 0:
        speed = f"{rate / 1024:.0f} KB/s"
        left = f"{fmt_time((total - t['bytes']) / rate)} left" if total and t["bytes"] < total else ""
    else:
        speed, left = "-- KB/s", ""
    # what must be there, then what fits: the bar, the time left, the file
    src = t.get("src", "")              # ALL HERE, or WAITING FOR TORRENT: n pieces
    core = " ".join(x for x in (f" {who}", what, f"[{src}]" if src else "", pct, sizes, speed) if x)
    line = core
    if prog and len(core) + 11 <= width:
        line = core.replace(f" {who} {what} ", f" {who} {what} {prog} ", 1)
    if left and len(line) + 2 + len(left) <= width:
        line += "  " + left
    note = t.get("note", "")
    if note and len(line) + 4 + len(note) <= width:
        line += f"  [{note}]"
    f = t.get("file", "")
    room = width - len(line) - 2
    if f and room >= 8:                 # a few letters of a tail say nothing
        line += "  " + (f if len(f) <= room else "..." + f[-(room - 3):])
    return (line[:width], "plain")


def games_rows(games, scroll, count, width, held_fn=None):
    head = f" {'DIR':<8} {'TITLE':<34.34} {'YEAR':<4} {'SIZE':>8}  {'HERE':<7} DISC"
    rows = [(head[:width], "head")]
    for g in games[scroll:scroll + count]:
        disc = ""
        if g.get("cd"):
            disc = fmt_size(g.get("cd_kb", 0) * 1024) + " ISO"
            if g.get("raw_kb"):
                disc += f", {fmt_size(g['raw_kb'] * 1024)} raw"
        here = held_fn(g) if held_fn else ""     # all, none, 3/12 pieces, or local
        rows.append((f" {g['dir']:<8} {g['title']:<34.34} {str(g.get('year') or ''):<4} "
                     f"{fmt_size(g['kb'] * 1024):>8}  {here:<7} {disc}"[:width], "plain"))
    return rows


def render(state, width, height, ascii_only=False):
    """The screen: rows of (text, style), exactly height of them.

    state: title (str), swarm (a line about the torrent, or None),
    transfers (list of dicts from Monitor.snapshot),
    lines (the log), games (list or None for the log view), scroll (rows
    from the top of the list, or None to follow the log's tail), now,
    rates (a Rates)."""
    now = state["now"]
    rows = [(f" {state['title']}"[:width].ljust(width), "title")]
    if state.get("swarm"):
        rows.append((f" {state['swarm']}"[:width], "dim"))
    transfers = state["transfers"]
    rows.append((" TRANSFERS" if transfers else " TRANSFERS  (none yet)", "head"))
    shown = transfers[-max(3, height // 3):]
    for t in shown:
        rows.append(transfer_row(t, now, state["rates"].rate(t, now), width, ascii_only))
    rows.append(("", "plain"))
    body_rows = height - len(rows) - 2          # a header line and the key line
    if body_rows < 1:
        body_rows = 1
    if state["games"] is not None:
        games = state["games"]
        scroll = max(0, min(state["scroll"] or 0, max(0, len(games) - (body_rows - 1))))
        rows.append((f" GAMES  {len(games)}   ({scroll + 1}-{min(len(games), scroll + body_rows - 1)})   g: back to the log",
                     "head"))
        rows += games_rows(games, scroll, body_rows - 1, width, state.get("held"))
    else:
        lines = state["lines"]
        follow = state["scroll"] is None
        top = max(0, len(lines) - body_rows) if follow else max(0, min(state["scroll"], max(0, len(lines) - body_rows)))
        tag = "" if follow else f"   ({top + 1}-{min(len(lines), top + body_rows)} of {len(lines)}, End follows)"
        rows.append((f" LOG{tag}", "head"))
        for line in lines[top:top + body_rows]:
            style = "bad" if " failed" in line or "went away" in line or "error" in line.lower() else "plain"
            rows.append((f" {line}"[:width], style))
    while len(rows) < height - 1:
        rows.append(("", "plain"))
    rows = rows[:height - 1]
    rows.append((" q quit   r rescan   g games/log   arrows, PgUp/PgDn, End"[:width], "dim"))
    return rows


def run(mon, title_fn, games_fn, actions, swarm_fn=None, held_fn=None):
    """Paint render() with curses until q. mon is waveserve's Monitor,
    title_fn() the title line, games_fn() the game list, actions a dict of
    callables for the keys ('rescan'), swarm_fn() a line about the torrent
    when there is one."""
    import curses, sys
    ascii_only = (sys.stdout.encoding or "").lower().replace("-", "") != "utf8"

    def loop(scr):
        curses.curs_set(0)
        scr.timeout(250)
        attrs = {s: curses.A_NORMAL for s in STYLES}
        attrs["title"] = curses.A_REVERSE | curses.A_BOLD
        attrs["head"] = curses.A_BOLD
        attrs["dim"] = curses.A_DIM
        if curses.has_colors():
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_CYAN, -1)
            curses.init_pair(2, curses.COLOR_GREEN, -1)
            curses.init_pair(3, curses.COLOR_RED, -1)
            curses.init_pair(4, curses.COLOR_YELLOW, -1)
            attrs["head"] = curses.color_pair(1) | curses.A_BOLD
            attrs["ok"] = curses.color_pair(2)
            attrs["bad"] = curses.color_pair(3)
            attrs["warn"] = curses.color_pair(4)
        rates = Rates()
        games = None            # None: the log view
        scroll = None           # None: follow the tail
        last_sample = 0
        while True:
            k = scr.getch()
            if k in (ord("q"), ord("Q")):
                return
            if k in (ord("r"), ord("R")):
                actions["rescan"]()
            elif k in (ord("g"), ord("G")):
                games = None if games is not None else list(games_fn())
                scroll = 0 if games is not None else None
            elif k == curses.KEY_UP:
                scroll = max(0, (scroll if scroll is not None else 10 ** 6) - 1)
            elif k == curses.KEY_DOWN:
                scroll = (scroll if scroll is not None else 10 ** 6) + 1
            elif k == curses.KEY_PPAGE:
                scroll = max(0, (scroll if scroll is not None else 10 ** 6) - 10)
            elif k == curses.KEY_NPAGE:
                scroll = (scroll if scroll is not None else 10 ** 6) + 10
            elif k == curses.KEY_END:
                scroll = None if games is None else 10 ** 6
            elif k == curses.KEY_HOME:
                scroll = 0
            now = time.time()
            transfers, lines = mon.snapshot()
            if now - last_sample >= 0.5:
                rates.sample(transfers, now)
                last_sample = now
            if games is not None:
                games = list(games_fn())    # a rescan may have changed it
            h, w = scr.getmaxyx()
            state = dict(title=title_fn(), swarm=swarm_fn() if swarm_fn else None, transfers=transfers,
                         lines=lines, games=games, scroll=scroll, now=now, rates=rates, held=held_fn)
            scr.erase()
            for y, (text, style) in enumerate(render(state, w, h, ascii_only)):
                try:
                    scr.addnstr(y, 0, text, w - 1, attrs.get(style, curses.A_NORMAL))
                except curses.error:
                    pass                    # the last cell of the last row
            scr.refresh()

    curses.wrapper(loop)


# ---- the first run -------------------------------------------------------
# Where eXoDOS is: on demand through the torrent, or a local install. The
# answers come back as ("torrent", path) or ("local", path), or None when
# the user leaves; the checks are waveserve's (a message, or None when the
# path will do).

SETUP_TITLE = "WAVE86 server - first run"


def setup_dialog(defaults, check_torrent, check_local, torrent_note=None):
    """The first-run questions in curses. defaults: {"torrent": path,
    "local": path}; torrent_note: a warning to show under a) (no
    libtorrent)."""
    import curses

    def loop(scr):
        curses.curs_set(0)
        warn = curses.A_BOLD
        if curses.has_colors():
            curses.use_default_colors()
            curses.init_pair(4, curses.COLOR_YELLOW, -1)
            warn = curses.color_pair(4) | curses.A_BOLD

        def put(y, x, text, attr=curses.A_NORMAL):
            h, w = scr.getmaxyx()
            if 0 <= y < h and x < w - 1:
                try:
                    scr.addnstr(y, x, text, w - 1 - x, attr)
                except curses.error:
                    pass

        def frame():
            scr.erase()
            put(0, 0, " " + SETUP_TITLE + " " * 200, curses.A_REVERSE | curses.A_BOLD)

        while True:
            frame()
            put(2, 2, "Where can I find eXoDOS?", curses.A_BOLD)
            put(4, 4, "a) On demand, via BitTorrent")
            put(5, 7, "Games are fetched from the swarm when a DOS machine asks for them.", curses.A_DIM)
            if torrent_note:
                put(6, 7, torrent_note, warn)
            put(8, 4, "b) Local install")
            put(9, 7, "An eXoDOS (or eXoDOS Lite) folder on this machine.", curses.A_DIM)
            put(11, 2, "Press a or b; Esc quits without changing anything.", curses.A_DIM)
            scr.refresh()
            k = scr.getch()
            if k == 27:
                return None
            if k in (ord("a"), ord("A")):
                kind, label, check = "torrent", "The eXoDOS torrent file:", check_torrent
            elif k in (ord("b"), ord("B")):
                kind, label, check = "local", "The eXoDOS folder (the one with eXo/eXoDOS in it):", check_local
            else:
                continue
            text, msg = defaults.get(kind) or "", None
            while True:
                text = edit_line(scr, put, frame, label, text, msg, warn)
                if text is None:
                    break                       # Esc: back to the question
                msg = check(text)
                if msg is None:
                    return kind, text

    return curses.wrapper(loop)


def edit_line(scr, put, frame, label, text, msg, warn):
    """One line of text, filled in with text: Enter gives it back, Esc
    gives None."""
    import curses
    pos = len(text)
    curses.curs_set(1)
    try:
        while True:
            frame()
            h, w = scr.getmaxyx()
            put(2, 2, label, curses.A_BOLD)
            width = max(10, w - 6)
            start = max(0, pos - width + 1)
            put(4, 2, "[" + " " * width + "]")
            put(4, 3, text[start:start + width])
            if msg:
                put(6, 2, msg, warn)
            put(8, 2, "Enter takes it; Esc goes back.", curses.A_DIM)
            try:
                scr.move(4, 3 + pos - start)
            except curses.error:
                pass
            scr.refresh()
            try:
                k = scr.get_wch()
            except curses.error:
                continue
            if k in ("\n", "\r", curses.KEY_ENTER):
                return text.strip()
            if k == "\x1b":
                return None
            if k in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                if pos:
                    text, pos = text[:pos - 1] + text[pos:], pos - 1
            elif k == curses.KEY_DC:
                text = text[:pos] + text[pos + 1:]
            elif k == curses.KEY_LEFT:
                pos = max(0, pos - 1)
            elif k == curses.KEY_RIGHT:
                pos = min(len(text), pos + 1)
            elif k in (curses.KEY_HOME, "\x01"):
                pos = 0
            elif k in (curses.KEY_END, "\x05"):
                pos = len(text)
            elif k == "\x15":                   # Ctrl-U: empty it
                text, pos = "", 0
            elif isinstance(k, str) and k.isprintable():
                text, pos = text[:pos] + k + text[pos:], pos + 1
            msg = None
    finally:
        curses.curs_set(0)


def setup_plain(defaults, check_torrent, check_local, torrent_note=None, ask=input):
    """The same questions without curses, one line at a time."""
    print(SETUP_TITLE)
    print()
    print("Where can I find eXoDOS?")
    print("  a) On demand, via BitTorrent" + (f"  ({torrent_note})" if torrent_note else ""))
    print("  b) Local install")
    while True:
        k = ask("a or b (empty to quit): ").strip().lower()
        if not k:
            return None
        if k in ("a", "b"):
            break
    kind, label, check = (("torrent", "The eXoDOS torrent file", check_torrent) if k == "a"
                          else ("local", "The eXoDOS folder (the one with eXo/eXoDOS in it)", check_local))
    while True:
        d = defaults.get(kind) or ""
        text = ask(f"{label}{f' [{d}]' if d else ''}: ").strip() or d
        msg = check(text)
        if msg is None:
            return kind, text
        print(msg)
