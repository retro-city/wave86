/*
 * waveget.cpp - WAVE86's network fetcher. Built on mTCP, so GPL v3.
 *
 *   WAVEGET LIST host:port outfile
 *       GET /list from waveserve into outfile (one game per line).
 *   WAVEGET GET host:port DIR gamesdir [kb [key [cdrom]]]
 *       GET /pack/key (default: DIR) and write the files into gamesdir\DIR\;
 *       with cdrom given, CD\NAME.ISO goes to cdrom\NAME.ISO instead.
 *   WAVEGET DISK host:port key
 *       ask for the game's NetDrive disk, to play it off the server.
 *   WAVEGET UPDATE host:port [dir]
 *       GET /update: a fresh WAVE86.EXE and helpers into dir (default: here),
 *       each renamed into place only once it has arrived whole.
 *
 * GET and UPDATE take over the screen while they run - a progress bar,
 * the rate, the files as they land - and play the launcher's soundtrack
 * on the way (netmusic=0 in WAVE86.INI if you would rather they did not).
 * M, +, -, < and > work as they do in the menu.
 *
 * Needs a packet driver and MTCPCFG (run DHCP first), like every mTCP
 * program. Esc or Ctrl-Break aborts. Exit code 0 = done, 1 = network
 * or setup problem, 2 = aborted, 3 = the server said no.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <dos.h>

#include "types.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "timer.h"

#define TCP_RECV_BUFFER   (16384)
#define INBUF             (8192)
#define CONNECT_TIMEOUT   (10000ul)
#define DISK_TIMEOUT (120000)   /* a big disc takes the server a while */
#define IDLE_TIMEOUT      (30000ul)

static TcpSocket *sock;
static uint8_t inBuf[INBUF];
static char hdrBuf[2048];
static uint16_t hdrLen = 0;

volatile uint8_t CtrlBreakDetected = 0;
void __interrupt __far ctrlBreakHandler() { CtrlBreakDetected = 1; }

/*
 * The launcher's music engine (src/music.c, src/mod.c) and CPU probe,
 * built in large model and linked in here: a download takes minutes and
 * the machine is only waiting for packets, so it may as well play. These
 * four are what music.c expects of whoever links it - the launcher fills
 * them from WAVE86.INI, we choose them:
 *   cfg_modrate 0 leaves the MOD mixer and its DMA out of it, so only the
 *   IMF tracks play, a few OPL register writes per timer tick, which the
 *   packet driver does not notice;
 *   cfg_music 1 means start playing at once (netmusic=0 in the INI, read
 *   below, turns the whole thing off);
 *   cfg_adlib -1 means detect (the INI's adlib= overrides).
 * mus_init() registers mus_shutdown with atexit, so every exit path here
 * unhooks the timer and silences the OPL.
 */
extern "C" {
  char home_dir[80] = "";
  int cfg_adlib = -1;
  int cfg_music = 1;
  int cfg_modrate = 0;
  void mus_init(void);
  void mus_poll(void);
  void mus_toggle(void);
  void mus_skip(int dir);
  void mus_volume(int delta);
  int  mus_vu(void);
  extern int mus_present, mus_on, mus_ntracks, mus_vol;
  extern char mus_track[9];
  extern char cpu_desc[32];
  void cpu_identify(void);
}
static int musicWanted = 1;         /* netmusic=0 in WAVE86.INI turns it off */

static void msg(const char *m);     /* on the transfer screen, or plain */
static char serverText[32] = "";

static int userWantsOut(void) {
  if (CtrlBreakDetected) return 1;
  while (biosIsKeyReady()) {
    uint16_t k = biosKeyRead();
    uint8_t c = (uint8_t)(k & 0xFF);
    if (c == 27 || c == 3) return 1;
    switch (c) {                    /* the launcher's music keys */
      case 'm': case 'M': mus_toggle();   break;
      case '+': case '=': mus_volume(1);  break;
      case '-': case '_': mus_volume(-1); break;
      case '.': case '>': mus_skip(1);    break;
      case ',': case '<': mus_skip(-1);   break;
    }
  }
  return 0;
}

static void pump(void) {
  PACKET_PROCESS_MULT(5);
  Arp::driveArp();
  Tcp::drivePackets();
  mus_poll();                       /* IMF: only the playlist, no mixing */
}

static void netShutdown(int rc) {
  if (sock) {
    sock->closeNonblocking();
    clockTicks_t t0 = TIMER_GET_CURRENT();
    while (!sock->isClosed() && Timer_diff(t0, TIMER_GET_CURRENT()) < TIMER_MS_TO_TICKS(2000)) pump();
    TcpSocketMgr::freeSocket(sock);
    sock = NULL;
  }
  Utils::endStack();
  exit(rc);
}

static int parseHostPort(const char *s, IpAddr_t addr, uint16_t *port) {
  int a, b, c, d, p = 8086;
  if (sscanf(s, "%d.%d.%d.%d:%d", &a, &b, &c, &d, &p) < 4) return -1;
  addr[0] = a; addr[1] = b; addr[2] = c; addr[3] = d;
  *port = (uint16_t)p;
  return 0;
}

static int connectTo(IpAddr_t addr, uint16_t port) {
  sock = TcpSocketMgr::getSocket();
  if (!sock || sock->setRecvBuffer(TCP_RECV_BUFFER)) { msg("WAVEGET: no socket"); return -1; }
  if (sock->connectNonBlocking(2048 + rand(), addr, port)) { msg("WAVEGET: connect failed"); return -1; }
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    if (userWantsOut()) return -2;
    pump();
    if (sock->isConnectComplete()) return 0;
    if (sock->isClosed() || Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(CONNECT_TIMEOUT)) {
      msg("WAVEGET: no answer from the server");
      return -1;
    }
  }
}

static int sendAll(const char *s) {
  uint16_t len = strlen(s), done = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (done < len) {
    pump();
    int rc = sock->send((uint8_t *)(s + done), len - done);
    if (rc > 0) { done += rc; t0 = TIMER_GET_CURRENT(); }
    else if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) return -1;
  }
  return 0;
}

/* one receive step: >0 bytes in inBuf, 0 nothing yet, -1 remote closed, -2 abort */
static int16_t recvStep(void) {
  if (userWantsOut()) return -2;
  pump();
  int16_t rc = sock->recv(inBuf, INBUF);
  if (rc > 0) return rc;
  if (sock->isRemoteClosed()) {
    rc = sock->recv(inBuf, INBUF);   /* drain what arrived with the FIN */
    return rc > 0 ? rc : -1;
  }
  return 0;
}

/* GET the path; returns 0 with the first body bytes at inBuf[*bodyStart..n) */
static int httpGet(IpAddr_t addr, uint16_t port, const char *path, int16_t *n, uint16_t *bodyStart) {
  char req[160];
  int rc = connectTo(addr, port);
  if (rc) return rc == -2 ? 2 : 1;
  sprintf(req, "GET %s HTTP/1.0\r\nHost: %d.%d.%d.%d\r\nUser-Agent: WAVE86\r\nConnection: close\r\n\r\n",
          path, addr[0], addr[1], addr[2], addr[3]);
  if (sendAll(req)) { msg("WAVEGET: could not send the request"); return 1; }
  hdrLen = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    int16_t got = recvStep();
    if (got == -2) return 2;
    if (got == -1) { msg("WAVEGET: connection closed before the headers came"); return 1; }
    if (got == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) { msg("WAVEGET: timeout"); return 1; }
      continue;
    }
    t0 = TIMER_GET_CURRENT();
    for (int16_t i = 0; i < got; i++) {
      if (hdrLen < sizeof(hdrBuf) - 1) hdrBuf[hdrLen++] = inBuf[i];
      if (hdrLen >= 4 && memcmp(hdrBuf + hdrLen - 4, "\r\n\r\n", 4) == 0) {
        hdrBuf[hdrLen] = 0;
        if (strncmp(hdrBuf, "HTTP/1.", 7) != 0 || strncmp(hdrBuf + 9, "200", 3) != 0) {
          char said[80];
          hdrBuf[hdrLen > 40 ? 40 : hdrLen] = 0;
          sprintf(said, "WAVEGET: the server answered: %s", hdrBuf);
          msg(said);
          return 3;
        }
        *bodyStart = i + 1;
        *n = got;
        return 0;
      }
    }
  }
}

/* ------------------------------------------------------- the screen */
/*
 * A game takes minutes to arrive on a 486, so the transfer gets a screen
 * of its own rather than one line of printf: the launcher's colours in a
 * band across the top, a progress bar, the rate, what is being written,
 * and the tune. Written straight into text memory, 80x25, colour or mono;
 * in a graphics mode (nothing puts us there) the screen stays off and the
 * old single line is what you get. Anything WAVEGET has to say goes
 * through msg(), which shows it here and says it again in plain text once
 * the screen is handed back.
 */
#define A(fg, bg) ((uint16_t)((uint16_t)(((bg) << 4) | (fg)) << 8))
#define BOX_X   3
#define BOX_Y   4
#define BOX_W   74
#define BOX_H   9
#define BAR_X   (BOX_X + 3)
#define BAR_W   (BOX_W - 12)

static uint16_t __far *vram = NULL;
static int vcols = 80;
static uint16_t curShape = 0x0607;
static int screenOn = 0;
static char lastMsg[120] = "";
static const uint8_t band[5] = { 5, 13, 12, 12, 14 };   /* eXoDOS, dark to light */

static void scrCell(int x, int y, char ch, uint16_t attr) {
  if (!screenOn || x < 0 || x >= vcols || y < 0 || y >= 25) return;
  vram[y * vcols + x] = (uint16_t)(uint8_t)ch | attr;
}

static void scrPuts(int x, int y, const char *s, uint16_t attr) {
  while (*s) scrCell(x++, y, *s++, attr);
}

static void scrFill(int x, int y, int w, char ch, uint16_t attr) {
  while (w-- > 0) scrCell(x++, y, ch, attr);
}

static void scrClear(void) {
  int y;
  for (y = 0; y < 25; y++) scrFill(0, y, vcols, ' ', A(7, 0));
}

static void scrOpen(void) {
  union REGS r;
  r.h.ah = 0x0F; int86(0x10, &r, &r);
  if (r.h.al == 7) vram = (uint16_t __far *)MK_FP(0xB000, 0);
  else if (r.h.al <= 3) vram = (uint16_t __far *)MK_FP(0xB800, 0);
  else return;                          /* a graphics mode: leave it alone */
  vcols = r.h.ah ? r.h.ah : 80;
  if (vcols > 80 || vcols < 40) vcols = 80;
  r.h.ah = 0x03; r.h.bh = 0; int86(0x10, &r, &r);
  curShape = r.x.cx;
  r.h.ah = 0x01; r.x.cx = 0x2000; int86(0x10, &r, &r);      /* cursor away */
  screenOn = 1;
  scrClear();
}

static void scrClose(void) {
  union REGS r;
  if (!screenOn) return;
  scrClear();
  screenOn = 0;
  r.h.ah = 0x01; r.x.cx = curShape; int86(0x10, &r, &r);
  r.h.ah = 0x02; r.h.bh = 0; r.x.dx = 0; int86(0x10, &r, &r);
}

/* 12345678 -> "12,345,678" */
static void fmtNum(char *dst, unsigned long v) {
  char tmp[16];
  int n = 0, i;
  if (!v) { strcpy(dst, "0"); return; }
  while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
  for (i = n - 1; i >= 0; i--) {
    *dst++ = tmp[i];
    if (i && i % 3 == 0) *dst++ = ',';
  }
  *dst = 0;
}

static void fmtTime(char *dst, unsigned long secs) {
  if (secs > 5999ul) { strcpy(dst, "99:59"); return; }
  sprintf(dst, "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

static void uiStatus(const char *text) {
  scrFill(BOX_X + 3, BOX_Y + BOX_H - 2, BOX_W - 6, ' ', A(7, 0));
  scrPuts(BOX_X + 3, BOX_Y + BOX_H - 2, text, A(7, 0));
}

/* what WAVEGET has to say, wherever it can be read */
static void msg(const char *m) {
  if (screenOn) {
    strncpy(lastMsg, m, sizeof(lastMsg) - 1);
    lastMsg[sizeof(lastMsg) - 1] = 0;
    uiStatus(m);
  } else {
    puts(m);
  }
}

static void uiOpen(const char *title, const char *host) {
  int x, y;
  char line[96];
  scrOpen();
  if (!screenOn) return;
  for (x = 0; x < vcols; x++)                       /* the colour band */
    scrCell(x, 0, (char)223, A(band[(x * 5) / vcols], 0));
  scrPuts(2, 1, "WAVE86", A(15, 0));
  scrPuts(9, 1, "NETWORK TRANSFER", A(8, 0));
  if (cpu_desc[0]) {
    int w = strlen(cpu_desc);
    scrPuts(vcols - 2 - w, 1, cpu_desc, A(8, 0));
  }
  scrCell(BOX_X, BOX_Y, (char)218, A(5, 0));
  scrCell(BOX_X + BOX_W - 1, BOX_Y, (char)191, A(5, 0));
  scrCell(BOX_X, BOX_Y + BOX_H - 1, (char)192, A(5, 0));
  scrCell(BOX_X + BOX_W - 1, BOX_Y + BOX_H - 1, (char)217, A(5, 0));
  scrFill(BOX_X + 1, BOX_Y, BOX_W - 2, (char)196, A(5, 0));
  scrFill(BOX_X + 1, BOX_Y + BOX_H - 1, BOX_W - 2, (char)196, A(5, 0));
  for (y = BOX_Y + 1; y < BOX_Y + BOX_H - 1; y++) {
    scrCell(BOX_X, y, (char)179, A(5, 0));
    scrCell(BOX_X + BOX_W - 1, y, (char)179, A(5, 0));
  }
  scrPuts(BOX_X + 3, BOX_Y + 1, title, A(15, 0));
  sprintf(line, "from %s", host);
  scrPuts(BOX_X + BOX_W - 3 - strlen(line), BOX_Y + 1, line, A(8, 0));
  scrPuts(2, 24, "ESC", A(0, 3));
  scrPuts(6, 24, "ABORT", A(7, 0));
  if (mus_present && mus_ntracks) {
    scrPuts(15, 24, "M", A(0, 3));
    scrPuts(17, 24, "MUSIC", A(7, 0));
    scrPuts(25, 24, "+-", A(0, 3));
    scrPuts(28, 24, "VOL", A(7, 0));
    scrPuts(34, 24, "<>", A(0, 3));
    scrPuts(37, 24, "TRACK", A(7, 0));
  }
}

static void uiClose(void) {
  scrClose();
  if (lastMsg[0]) {                 /* said on the screen, said again here */
    puts(lastMsg);
    lastMsg[0] = 0;
  }
}

/* the tune and its level, at the bottom of the box */
static void uiMusic(void) {
  int y = BOX_Y + BOX_H + 1, i, vu;
  if (!screenOn || !mus_present || !mus_ntracks) return;
  scrCell(BOX_X + 3, y, (char)14, A(mus_on ? 13 : 8, 0));
  scrFill(BOX_X + 5, y, 10, ' ', A(7, 0));
  scrPuts(BOX_X + 5, y, mus_on ? mus_track : "OFF", A(mus_on ? 7 : 8, 0));
  vu = mus_on ? mus_vu() : 0;
  for (i = 0; i < 10; i++)
    scrCell(BOX_X + 16 + i, y, (char)(i < vu ? 221 : 250),
            A(i < vu ? band[(i * 5) / 10] : 8, 0));
}

static unsigned barPhase = 0;

/* the last few files that landed, under the box: a transfer that takes
   minutes should show that something is happening */
#define LOGN 8
static char fileLog[LOGN][40];
static int logHead = 0;

static void uiLog(const char *name) {
  if (!screenOn) return;
  strncpy(fileLog[logHead], name, sizeof(fileLog[0]) - 1);
  fileLog[logHead][sizeof(fileLog[0]) - 1] = 0;
  logHead = (logHead + 1) % LOGN;
}

static void uiLogDraw(void) {
  int i, y = BOX_Y + BOX_H + 3;
  for (i = 0; i < LOGN; i++) {
    const char *name = fileLog[(logHead + i) % LOGN];
    scrFill(BOX_X + 5, y + i, 40, ' ', A(7, 0));
    scrPuts(BOX_X + 5, y + i, name, A(i == LOGN - 1 ? 7 : 8, 0));
  }
}

/* WAVEDUMP=C:\SCREEN.BIN in the environment writes the text screen there
   once a second, which is how the test harness gets to look at it - the
   same idea as the launcher's /dump. */
static void uiDump(void) {
  const char *to = getenv("WAVEDUMP");
  FILE *f;
  int y, x;
  if (!screenOn || vcols != 80 || !to || !*to) return;
  f = fopen(to, "wb");
  if (!f) return;
  for (y = 0; y < 25; y++)
    for (x = 0; x < 80; x++) {
      uint16_t cell = vram[y * 80 + x];
      fwrite(&cell, 2, 1, f);
    }
  fclose(f);
}

static void uiProgress(const char *file, unsigned long done, unsigned long totalKb,
                       unsigned long kbs, unsigned long secs) {
  char a[20], b[20], t[8], line[96];
  unsigned long doneKb = done / 1024;
  int i, filled, pct;
  if (!screenOn) return;
  barPhase++;
  if (totalKb) {
    filled = (int)((unsigned long)BAR_W * doneKb / totalKb);
    if (filled > BAR_W) filled = BAR_W;
    pct = (int)(doneKb * 100 / totalKb);
    for (i = 0; i < BAR_W; i++)
      scrCell(BAR_X + i, BOX_Y + 3, (char)(i < filled ? 219 : 176),
              A(i < filled ? band[(i * 5) / BAR_W] : 8, 0));
    sprintf(line, "%3d%%", pct);
    scrPuts(BAR_X + BAR_W + 2, BOX_Y + 3, line, A(15, 0));
  } else {                          /* no total: a light running along */
    for (i = 0; i < BAR_W; i++) {
      int lit = ((i + (int)barPhase) % 12) < 4;
      scrCell(BAR_X + i, BOX_Y + 3, (char)(lit ? 219 : 176),
              A(lit ? band[(i * 5) / BAR_W] : 8, 0));
    }
  }
  fmtNum(a, doneKb);
  scrFill(BAR_X, BOX_Y + 5, BOX_W - 6, ' ', A(7, 0));
  if (totalKb) {
    fmtNum(b, totalKb);
    sprintf(line, "%sK of %sK", a, b);
  } else {
    sprintf(line, "%sK", a);
  }
  scrPuts(BAR_X, BOX_Y + 5, line, A(7, 0));
  sprintf(line, "%lu KB/s", kbs);
  scrPuts(BAR_X + 26, BOX_Y + 5, line, A(14, 0));
  fmtTime(t, secs);
  sprintf(line, "%s elapsed", t);
  scrPuts(BAR_X + 40, BOX_Y + 5, line, A(8, 0));
  if (totalKb && kbs && doneKb < totalKb) {
    fmtTime(t, (totalKb - doneKb) / kbs);
    sprintf(line, "%s left", t);
    scrPuts(BAR_X + 55, BOX_Y + 5, line, A(8, 0));
  }
  scrFill(BAR_X, BOX_Y + 6, BOX_W - 6, ' ', A(7, 0));
  sprintf(line, "writing  %s", file);
  line[BOX_W - 7] = 0;
  scrPuts(BAR_X, BOX_Y + 6, line, A(7, 0));
  uiMusic();
  uiLogDraw();
  if (barPhase % 5 == 0) uiDump();
}

/* MUSIC\ sits next to WAVEGET.EXE; netmusic=0 in WAVE86.INI says no, and
   adlib= is honoured the way the launcher honours it */
static void musicStart(const char *exePath) {
  char path[120];
  FILE *f;
  int n;
  strncpy(home_dir, exePath, sizeof(home_dir) - 1);
  home_dir[sizeof(home_dir) - 1] = 0;
  n = strlen(home_dir);
  while (n && home_dir[n - 1] != '\\' && home_dir[n - 1] != ':') n--;
  home_dir[n] = 0;
  if (!n) strcpy(home_dir, ".\\");
  sprintf(path, "%sWAVE86.INI", home_dir);
  f = fopen(path, "r");
  if (f) {
    char buf[100];
    while (fgets(buf, sizeof(buf), f)) {
      if (buf[0] == '[') break;                 /* per-game sections */
      if (strnicmp(buf, "netmusic=", 9) == 0) musicWanted = atoi(buf + 9);
      else if (strnicmp(buf, "adlib=", 6) == 0) cfg_adlib = atoi(buf + 6);
    }
    fclose(f);
  }
  if (!musicWanted) return;
  cpu_identify();
  mus_init();
  if (mus_ntracks > 1)              /* not the same tune every download */
    mus_skip((int)(TIMER_GET_CURRENT() % mus_ntracks));
}

/* ---------------------------------------------------------- LIST */
static int doList(IpAddr_t addr, uint16_t port, const char *outfile) {
  int16_t n; uint16_t start;
  int rc = httpGet(addr, port, "/list", &n, &start);
  if (rc) return rc;
  FILE *f = fopen(outfile, "wb");
  if (!f) { printf("WAVEGET: cannot write %s\n", outfile); return 1; }
  unsigned long total = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    if (n > (int16_t)start) { fwrite(inBuf + start, 1, n - start, f); total += n - start; }
    start = 0;
    n = recvStep();
    if (n == -2) { fclose(f); remove(outfile); return 2; }
    if (n == -1) break;
    if (n == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) { puts("WAVEGET: timeout"); fclose(f); return 1; }
      continue;
    }
    t0 = TIMER_GET_CURRENT();
  }
  fclose(f);
  printf("WAVEGET: game list saved, %lu bytes\n", total);
  return 0;
}

/* ---------------------------------------------------------- DISK */
/* ask the server for the game's NetDrive disk (built on first request);
   prints its answer, "OK NAME.DSK", and returns 0 when it has one */
static int doDisk(IpAddr_t addr, uint16_t port, const char *key) {
  char path[64];
  sprintf(path, "/disk/%s", key);
  int16_t n; uint16_t start;
  int rc = httpGet(addr, port, path, &n, &start);
  if (rc) return rc;
  char reply[80]; int len = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    if (n > (int16_t)start) {
      int take = n - start;
      if (len + take > (int)sizeof(reply) - 1) take = sizeof(reply) - 1 - len;
      memcpy(reply + len, inBuf + start, take); len += take;
    }
    start = 0;
    n = recvStep();
    if (n == -2) return 2;
    if (n == -1) break;
    if (n == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(DISK_TIMEOUT)) { puts("WAVEGET: timeout waiting for the disk"); return 1; }
      continue;
    }
    t0 = TIMER_GET_CURRENT();
  }
  reply[len] = 0;
  printf("WAVEGET: %s", reply);
  return strncmp(reply, "OK ", 3) == 0 ? 0 : 1;
}

/* ---------------------------------------------------------- GET */
static char gameRoot[80];          /* gamesdir\DIR */
static char lastDir[128] = "";

static void ensureDirs(const char *rel) {
  /* create every folder on the way to rel (backslash separated) */
  char full[160];
  strcpy(full, gameRoot);
  const char *p = rel;
  while (1) {
    const char *bs = strchr(p, '\\');
    if (!bs) break;
    strcat(full, "\\");
    strncat(full, p, bs - p);
    if (strcmp(full, lastDir) != 0) { mkdir(full); strcpy(lastDir, full); }
    p = bs + 1;
  }
}

static const char *cdromDir = NULL;      /* where the discs go, if not the game folder */

/* an update writes each file beside its target first: NAME.W86 */
static void tempName(char *dst, const char *full) {
  char *dot, *bs;
  strcpy(dst, full);
  dot = strrchr(dst, '.');
  bs = strrchr(dst, '\\');
  if (dot && (!bs || dot > bs)) *dot = 0;
  strcat(dst, ".W86");
}

/* The file stream both GET and UPDATE speak: "F <size> <relpath>\n" then the
   bytes, over and over, and "E\n" at the end. Everything lands under
   gameRoot. With safe set (UPDATE) a file is written under a temporary name
   and renamed once its last byte is in, so a connection that drops halfway
   leaves the program already on the disk intact - including WAVEGET.EXE,
   which is busy overwriting itself. */
static int doStream(IpAddr_t addr, uint16_t port, const char *path, const char *what,
                    unsigned long kbTotal, int safe) {
  int16_t n; uint16_t pos;
  int rc;
  uiStatus("asking the server ...");
  rc = httpGet(addr, port, path, &n, &pos);
  if (rc) return rc;
  uiStatus("");

  char line[200]; uint16_t lineLen = 0;
  FILE *cur = NULL; unsigned long remain = 0, done = 0; int files = 0;
  char curName[80] = "", full[160], tmp[160];
  int renaming = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT(), tStart = t0, tShow = t0;
  int finished = 0;

  while (!finished) {
    while (pos < (uint16_t)n) {
      if (cur) {                                   /* file data */
        uint16_t chunk = n - pos;
        if ((unsigned long)chunk > remain) chunk = (uint16_t)remain;
        if (fwrite(inBuf + pos, 1, chunk, cur) != chunk) { msg("WAVEGET: disk write failed (is it full?)"); fclose(cur); return 1; }
        pos += chunk; remain -= chunk; done += chunk;
        if (remain == 0) {
          fclose(cur); cur = NULL; files++;
          if (renaming) { remove(full); rename(tmp, full); }
          uiLog(curName);
        }
      } else {                                     /* a header line */
        char c = inBuf[pos++];
        if (c != '\n') { if (lineLen < sizeof(line) - 1) line[lineLen++] = c; continue; }
        line[lineLen] = 0; lineLen = 0;
        if (line[0] == 'E') { finished = 1; break; }
        if (line[0] != 'F') { char m[100]; sprintf(m, "WAVEGET: bad stream: %.60s", line); msg(m); return 1; }
        unsigned long size = strtoul(line + 2, NULL, 10);
        const char *rel = strchr(line + 2, ' ');
        if (!rel) return 1;
        rel++;
        if (cdromDir && strnicmp(rel, "CD\\", 3) == 0) {   /* the disc goes to the CD folder */
          mkdir(cdromDir);
          sprintf(full, "%s\\%s", cdromDir, rel + 3);
        } else {
          ensureDirs(rel);
          sprintf(full, "%s\\%s", gameRoot, rel);
        }
        renaming = 0;
        if (safe) { tempName(tmp, full); renaming = strcmp(tmp, full) != 0; }
        strncpy(curName, rel, sizeof(curName) - 1);
        cur = fopen(renaming ? tmp : full, "wb");
        if (!cur) { char m[120]; sprintf(m, "WAVEGET: cannot create %.90s", renaming ? tmp : full); msg(m); return 1; }
        remain = size;
        if (size == 0) {
          fclose(cur); cur = NULL; files++;
          if (renaming) { remove(full); rename(tmp, full); }
          uiLog(curName);
        }
      }
    }
    if (finished) break;
    if (Timer_diff(tShow, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(screenOn ? 200 : 500)) {
      tShow = TIMER_GET_CURRENT();
      unsigned long secs = Timer_diff(tStart, tShow) * TIMER_TICK_LEN / 1000;
      unsigned long kbs = secs ? (done / 1024) / secs : 0;
      if (screenOn) uiProgress(curName, done, kbTotal, kbs, secs);
      else if (kbTotal) printf("\r%-24s %6luK of %luK  %3lu%%  %4lu KB/s   ", curName, done / 1024, kbTotal, (done / 1024) * 100 / kbTotal, kbs);
      else         printf("\r%-24s %6luK  %4lu KB/s   ", curName, done / 1024, kbs);
    }
    n = recvStep(); pos = 0;
    if (n == -2) { if (cur) fclose(cur); msg("WAVEGET: aborted"); return 2; }
    if (n == -1) { msg("WAVEGET: the connection dropped mid-way"); if (cur) fclose(cur); return 1; }
    if (n == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) { msg("WAVEGET: timeout"); if (cur) fclose(cur); return 1; }
      n = 0; continue;
    }
    t0 = TIMER_GET_CURRENT();
  }
  unsigned long secs = Timer_diff(tStart, TIMER_GET_CURRENT()) * TIMER_TICK_LEN / 1000;
  char done_line[120];
  sprintf(done_line, "WAVEGET: %s ready: %d files, %luK in %lu s", what, files, done / 1024, secs);
  if (screenOn) { uiProgress(curName, done, kbTotal, secs ? (done / 1024) / secs : 0, secs); msg(done_line); }
  else printf("\r%-70s\n", done_line);
  return 0;
}

static int doGet(IpAddr_t addr, uint16_t port, const char *dir, const char *gamesdir, unsigned long kbTotal, const char *key) {
  char path[64];
  sprintf(path, "/pack/%s", key ? key : dir);
  sprintf(gameRoot, "%s\\%s", gamesdir, dir);
  mkdir(gamesdir);
  mkdir(gameRoot);
  uiOpen(dir, serverText);
  int rc = doStream(addr, port, path, dir, kbTotal, 0);
  uiClose();
  return rc;
}

/* ------------------------------------------------------- UPDATE */
/* WAVE86.EXE and its helpers, straight from the machine that builds them,
   into dir (the launcher's folder). Run from the batch loop, so the
   launcher itself is not in memory when its EXE is replaced. */
static int doUpdate(IpAddr_t addr, uint16_t port, const char *dir) {
  strncpy(gameRoot, dir && dir[0] ? dir : ".", sizeof(gameRoot) - 1);
  gameRoot[sizeof(gameRoot) - 1] = 0;
  int len = strlen(gameRoot);       /* C:\WAVE86\ -> C:\WAVE86 */
  if (len > 1 && gameRoot[len - 1] == '\\' && gameRoot[len - 2] != ':') gameRoot[len - 1] = 0;
  uiOpen("WAVE86 ITSELF", serverText);
  if (!screenOn) printf("WAVEGET: updating WAVE86 in %s\n", gameRoot);
  int rc = doStream(addr, port, "/update", "update", 0, 1);
  uiClose();
  return rc;
}

/* ---------------------------------------------------------- main */
static void usage(void) {
  puts("WAVEGET LIST host:port outfile\nWAVEGET GET host:port DIR gamesdir [kb [key [cdrom]]]\n"
       "WAVEGET DISK host:port key\nWAVEGET UPDATE host:port [dir]");
  exit(1);
}

int main(int argc, char **argv) {
  IpAddr_t addr; uint16_t port;
  if (argc < 3 || (argc < 4 && stricmp(argv[1], "UPDATE") != 0)) usage();
  if (parseHostPort(argv[2], addr, &port)) { puts("WAVEGET: server must be a.b.c.d:port (set server= in WAVE86.INI)"); return 1; }

  if (Utils::parseEnv() != 0) {
    puts("WAVEGET: MTCPCFG is not set or the file is bad. Load the packet driver,");
    puts("         SET MTCPCFG=C:\\WAVE86\\MTCP.CFG and run DHCP once.");
    return 1;
  }
  if (Utils::initStack(1, TCP_SOCKET_RING_SIZE, ctrlBreakHandler, ctrlBreakHandler)) {
    puts("WAVEGET: TCP/IP would not start (packet driver loaded? DHCP done?)");
    return 1;
  }

  strncpy(serverText, argv[2], sizeof(serverText) - 1);
  serverText[sizeof(serverText) - 1] = 0;

  int rc = 1;
  if (stricmp(argv[1], "LIST") == 0) {
    rc = doList(addr, port, argv[3]);
  } else if (stricmp(argv[1], "DISK") == 0 && argc >= 4) {
    rc = doDisk(addr, port, argv[3]);
  } else if (stricmp(argv[1], "UPDATE") == 0) {
    musicStart(argv[0]);
    rc = doUpdate(addr, port, argc > 3 ? argv[3] : NULL);
  } else if (stricmp(argv[1], "GET") == 0 && argc >= 5) {
    if (argc > 7) cdromDir = argv[7];
    musicStart(argv[0]);
    rc = doGet(addr, port, argv[3], argv[4], argc > 5 ? strtoul(argv[5], NULL, 10) : 0, argc > 6 ? argv[6] : NULL);
  } else {
    Utils::endStack();
    usage();
  }
  if (rc == 1 || rc == 3) {
    puts("Press a key.");
    while (!biosIsKeyReady()) { }
    biosKeyRead();
  }
  netShutdown(rc);
  return rc;
}
