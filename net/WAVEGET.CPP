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
#include <ctype.h>
#include <time.h>

#include "types.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "timer.h"
#include "ip.h"

#define TCP_RECV_BUFFER   (16384)
#define INBUF             (8192)
#define CONNECT_TIMEOUT   (10000ul)
#define DISK_TIMEOUT (120000)   /* a big disc takes the server a while */
/* How long the server may say nothing before we give up on it. It was 30
   seconds, which a server reading its games off a disk never noticed; one
   that fetches them from a swarm can stall in the middle of a file, where
   it has no way of telling us so. nettimeout= in the INI, in seconds.
   Since then the server says when it waits, and a connection that goes
   silent has stopped for good more often than not - on the PicoMEM the
   packets stop coming while a fresh connection works at once - so after
   netreconnect= seconds of silence the connection is dropped and the rest
   asked for on a new one, in this same run, as many times as it takes;
   only nettimeout= seconds with nothing new at all end it. */
static unsigned long idleMs = 120000ul;
#define IDLE_TIMEOUT      (idleMs)
static unsigned long reconnectMs = 15000ul;   /* netreconnect=0 turns it off */
/* how the last attempt ended, for the one that decides what to say */
static int gaveUpWaiting = 0;               /* the stream stopped for silence, not an error */
static int connectFailed = 0;               /* ... or never started: no answer to the SYN */
/* the kick: nothing comes and mTCP sends nothing on its own, but a NIC
   that keeps what it received until its next interrupt (the card's, it
   seems) is woken by any transmit - an ARP request, sixty bytes, whose
   reply also says the way in works. Counted for the log and the words. */
static unsigned kicks = 0, rescued = 0;     /* sent; followed by data within a second */
static int reconnects = 0;
static int silentArp = 0, silentRx = 0;     /* during the last silence: a reply came; any packet came */
static int silentBad = 0;                   /* ... packets came, and failed their checksums */
static int noHeaders = 0;                   /* ... the request got no reply at all */
static int mayReconnect = 0;                /* doGet's loop is around the stream: silence ends an attempt, not the run */
static int logged = 0;                      /* a WAVEGET.LOG line was written for this run */
static int sinceProgress = 0;               /* reconnects since the note last moved */
static char askText[80] = "";               /* what the stream shows while it asks: the reconnect words */
static char lastLog[120] = "";              /* the last message, for the log line at the end */
static clockTicks_t tLastW = 0;             /* when the server last said it was waiting (a W line) */

/* the silence that ends an attempt with a new one to follow: only under
   a caller that reconnects, and only when shorter than the give-up */
static unsigned long reconnectWait(void) {
  return (mayReconnect && reconnectMs && reconnectMs < idleMs) ? reconnectMs : 0;
}
static unsigned long silentSecs = 0, silentAtK = 0, slowWriteMs = 0;
static clockTicks_t tLastData = 0;         /* when bytes last came in */
static char silentName[16] = "";

static TcpSocket *sock;
static uint8_t inBuf[INBUF];
static char hdrBuf[2048];
static uint16_t hdrLen = 0;

volatile uint8_t CtrlBreakDetected = 0;
void __interrupt __far ctrlBreakHandler() { CtrlBreakDetected = 1; }

/* A critical error (a full or write-protected disk, a drive gone) fails
   the call that met it: Abort would have DOS end WAVEGET on the spot,
   with the packet driver, the timer and the music still hooked to it. */
static int __far critFail(unsigned deverr, unsigned, unsigned __far *) {
  if (deverr & 0x0800) return _HARDERR_FAIL;
  if (deverr & 0x2000) return _HARDERR_IGNORE;
  return _HARDERR_ABORT;              /* DOS allows nothing else */
}

/*
 * The launcher's music engine (src/music.c, src/mod.c) and CPU probe,
 * built in large model and linked in here: a download takes minutes and
 * the machine is only waiting for packets, so it may as well play. These
 * four are what music.c expects of whoever links it - the launcher fills
 * them from WAVE86.INI, we choose them:
 *   cfg_modrate 0 leaves the MOD mixer and its DMA out of it, so only the
 *   IMF and MIDI tracks play, a few OPL or MPU-401 writes per timer tick,
 *   which the packet driver does not notice;
 *   cfg_music 1 means start playing at once (netmusic=0 in the INI, read
 *   below, turns the whole thing off);
 *   cfg_adlib and cfg_mpu -1 mean detect, cfg_mpuport 0 BLASTER's P or
 *   330, cfg_musicstyle 0 every kind (the INI's adlib=, mpu=, mpuport=
 *   and musicstyle= override them).
 * mus_init() registers mus_shutdown with atexit, so every exit path here
 * unhooks the timer, silences the OPL and sends every sounding MIDI note
 * its Note Off.
 */
extern "C" {
  char home_dir[80] = "";
  int cfg_adlib = -1;
  int cfg_music = 1;
  int cfg_modrate = 0;
  int cfg_mpu = -1;
  unsigned cfg_mpuport = 0;
  extern int cfg_musicstyle;        /* music.c's: 0 all */
  int mus_style_parse(const char *s);
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
static int musicWanted = 0;         /* netmusic=1 in WAVE86.INI turns it on */
static int crcWanted = 1;           /* netcrc=0, or a server that sends none */
static uint16_t writeMax = 0;       /* netwrite=N: at most N bytes per write */
static char artKind[16] = "title";  /* netart=: the picture that comes with a game, or none */
static char themeName[12] = "exodos";   /* theme=: whose colours it is made for; the launcher's default look */
static int screenWanted = 1;        /* netscreen=0: the single progress line */

static void msg(const char *m);     /* on the transfer screen, or plain */
static uint16_t localPort = 4096;   /* a fresh one per run: see connectTo */
static char queueLabel[12] = "";    /* "2/7" from /q:, when a queue is running */
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

/* the connection, closed and let go of. isCloseDone() is what frees the
   socket's buffers (isClosed() never comes true when we close first):
   it waits for the other side's FIN up to TCP_CLOSE_TIMEOUT, five
   seconds in WAVEGET.CFG, then forces it. */
static void dropSocket(void) {
  if (!sock) return;
  sock->closeNonblocking();
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (!sock->isCloseDone() && Timer_diff(t0, TIMER_GET_CURRENT()) < TIMER_MS_TO_TICKS(20000)) pump();
  TcpSocketMgr::freeSocket(sock);
  sock = NULL;
}

/* whom to ARP for the kick: the server when it is on our network, else
   the gateway (the same choice the stack makes for every packet) */
static void kickTarget(IpAddr_t target) {
  int i, same = 1;
  for (i = 0; i < 4; i++)
    if ((MyIpAddr[i] & Netmask[i]) != (sock->dstHost[i] & Netmask[i])) same = 0;
  memcpy(target, same ? sock->dstHost : Gateway, sizeof(IpAddr_t));
}

/* WAVEGET.LOG next to the program: one line per transfer that ended,
   with the stack's counters, for a stall to be understood later */
static void logLine(const char *what) {
  char path[100];
  FILE *f;
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  sprintf(path, "%sWAVEGET.LOG", home_dir);
  f = fopen(path, "a");
  if (!f) return;
  fprintf(f, "%02d-%02d %02d:%02d %s | in %lu out %lu dropped %lu lowbuf %u | tcp in %lu seqerr %lu nospace %lu badsum %lu (ip %lu) | arp req %lu rep %lu | kicks %u woke %u reconnects %d | last silence: arp %d rx %d bad %d\n",
          tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, what,
          (unsigned long)Packets_received, (unsigned long)Packets_sent, (unsigned long)Packets_dropped, (unsigned)Buffer_lowFreeCount,
          (unsigned long)Tcp::Packets_Received, (unsigned long)Tcp::Packets_SeqOrAckError, (unsigned long)Tcp::Packets_DroppedNoSpace,
          (unsigned long)Tcp::ChecksumErrors, (unsigned long)Ip::badChecksum,
          (unsigned long)Arp::RequestsSent, (unsigned long)Arp::RepliesReceived, kicks, rescued, reconnects,
          silentArp, silentRx, silentBad);
  fclose(f);
  logged = 1;
}

static void netShutdown(int rc) {
  dropSocket();
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
  /* A fresh local port every time. With a fixed one, two downloads in a
     row reuse the same four-tuple while the server's side is still in
     TIME_WAIT and the SYN is quietly dropped - which is precisely what a
     queue does. Seeded from the BIOS tick in main(). */
  /* and a fresh one for every connection within a run too: a reconnect
     on the port the stalled connection still holds at the server's end
     gets no SYN-ACK, only its ten seconds of silence */
  localPort = (uint16_t)(localPort >= 65000u ? 4096u : localPort + 1u);
  connectFailed = 0;
  if (sock->connectNonBlocking(localPort, addr, port)) { msg("WAVEGET: connect failed"); connectFailed = 1; return -1; }
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    if (userWantsOut()) return -2;
    pump();
    if (sock->isConnectComplete()) return 0;
    if (sock->isClosed() || Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(CONNECT_TIMEOUT)) {
      msg("WAVEGET: no answer from the server");
      connectFailed = 1;
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
  char req[256];                    /* the path alone can pass 90 chars: ?cd=&from=&at=&crc=&wait=&art=&theme=&thumb= */
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
      unsigned long silence = reconnectWait() ? reconnectWait() : IDLE_TIMEOUT;
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(silence)) {
        /* no reply at all: silence like any other for a caller that
           reconnects to weigh, said here for the others */
        gaveUpWaiting = 1;
        noHeaders = 1;
        silentSecs = Timer_diff(t0, TIMER_GET_CURRENT()) * TIMER_TICK_LEN / 1000;
        if (!mayReconnect) msg("WAVEGET: timeout: no reply to the request");
        return 1;
      }
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
  char cut[BOX_W - 5];
  strncpy(cut, text, sizeof(cut) - 1);          /* inside the box, not over it */
  cut[sizeof(cut) - 1] = 0;
  scrFill(BOX_X + 3, BOX_Y + BOX_H - 2, BOX_W - 6, ' ', A(7, 0));
  scrPuts(BOX_X + 3, BOX_Y + BOX_H - 2, cut, A(7, 0));
}

/* what WAVEGET has to say, wherever it can be read */
static void msg(const char *m) {
  strncpy(lastLog, m, sizeof(lastLog) - 1);
  lastLog[sizeof(lastLog) - 1] = 0;
  if (screenOn) {
    strncpy(lastMsg, m, sizeof(lastMsg) - 1);
    lastMsg[sizeof(lastMsg) - 1] = 0;
    uiStatus(m);
  } else {
    puts(m);
  }
}

/* the log line for a run that ended without one: whatever was last said */
static void logLast(int rc) {
  if (logged) return;
  logLine(lastLog[0] ? (strncmp(lastLog, "WAVEGET: ", 9) == 0 ? lastLog + 9 : lastLog) : (rc ? "ended" : "ready"));
}

static void uiOpen(const char *title, const char *host) {
  int x, y;
  char line[96];
  if (!screenWanted) return;        /* netscreen=0: the old single line */
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
  if (queueLabel[0]) {              /* /q:3/7: where we are in the queue */
    sprintf(line, "%s of the queue", queueLabel);
    scrPuts(BOX_X + 5 + strlen(title), BOX_Y + 1, line, A(14, 0));
  }
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
  fflush(stdout);                   /* redirected output, should we hang */
}

/* The tune, its level and the volume, under the box. When there is no
   music it says why - which beats silence when you were expecting some:
   MUSIC\ is looked for beside WAVEGET.EXE, the same folder the launcher
   plays from. */
static void uiMusic(void) {
  int y = BOX_Y + BOX_H + 1, i, vu, playing;
  char what[64];
  if (!screenOn) return;
  playing = musicWanted && mus_present && mus_ntracks && mus_on;
  if (!musicWanted)      strcpy(what, "MUSIC OFF (netmusic=0)");
  else if (!mus_present) strcpy(what, "NO ADLIB, SOUND BLASTER OR MPU-401");
  else if (!mus_ntracks) sprintf(what, "NO TRACKS IN %sMUSIC", home_dir);
  else if (!mus_on)      strcpy(what, "MUSIC OFF (M)");
  else                   strcpy(what, mus_track);
  scrCell(BOX_X + 3, y, (char)14, A(playing ? 13 : 8, 0));
  scrFill(BOX_X + 5, y, 48, ' ', A(7, 0));
  scrPuts(BOX_X + 5, y, what, A(playing ? 7 : 8, 0));
  if (!mus_present || !mus_ntracks) return;
  vu = playing ? mus_vu() : 0;
  for (i = 0; i < 10; i++)                  /* the level, live */
    scrCell(BOX_X + 22 + i, y, (char)(i < vu ? 221 : 250),
            A(i < vu ? band[(i * 5) / 10] : 8, 0));
  scrPuts(BOX_X + 36, y, "VOL", A(8, 0));   /* and where + and - put it */
  for (i = 0; i < 10; i++)
    scrCell(BOX_X + 40 + i, y, (char)(i < mus_vol ? 219 : 176),
            A(i < mus_vol ? 11 : 8, 0));
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
  uiDump();
}

/* MUSIC\ sits next to WAVEGET.EXE; netmusic=0 in WAVE86.INI says no,
   netcrc=0 turns the checksums off, and adlib=, mpu=, mpuport= and
   musicstyle= are honoured the way the launcher honours them */
/* a word from an INI line: up to a space, a tab or a ; comment */
static void iniWord(char *dst, unsigned n, const char *s) {
  unsigned i = 0;
  while (*s == ' ' || *s == '\t') s++;
  while (*s && *s != ' ' && *s != '\t' && *s != ';' && *s != '\r' && *s != '\n' && i < n - 1) dst[i++] = *s++;
  dst[i] = 0;
}

static void loadSettings(const char *exePath) {
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
      else if (strnicmp(buf, "netcrc=", 7) == 0) crcWanted = atoi(buf + 7);
      else if (strnicmp(buf, "netwrite=", 9) == 0) writeMax = (uint16_t)atoi(buf + 9);
      else if (strnicmp(buf, "netscreen=", 10) == 0) screenWanted = atoi(buf + 10);
      else if (strnicmp(buf, "netart=", 7) == 0) iniWord(artKind, sizeof(artKind), buf + 7);
      else if (strnicmp(buf, "theme=", 6) == 0) iniWord(themeName, sizeof(themeName), buf + 6);
      else if (strnicmp(buf, "nettimeout=", 11) == 0) {
        unsigned long secs = strtoul(buf + 11, NULL, 10);
        if (secs >= 5 && secs <= 3600) idleMs = secs * 1000ul;
      }
      else if (strnicmp(buf, "netreconnect=", 13) == 0) {
        unsigned long secs = strtoul(buf + 13, NULL, 10);
        reconnectMs = secs == 0 ? 0 : (secs < 5 ? 5000ul : secs > 3600 ? 3600000ul : secs * 1000ul);
      }
      else if (strnicmp(buf, "adlib=", 6) == 0) cfg_adlib = atoi(buf + 6);
      else if (strnicmp(buf, "mpu=", 4) == 0) cfg_mpu = atoi(buf + 4);
      else if (strnicmp(buf, "mpuport=", 8) == 0) cfg_mpuport = (unsigned)strtoul(buf + 8, NULL, 16);
      else if (strnicmp(buf, "musicstyle=", 11) == 0) cfg_musicstyle = mus_style_parse(buf + 11);
    }
    fclose(f);
  }
}

static void musicStart(const char *exePath) {
  loadSettings(exePath);
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

/* ---------------------------------------------------------- THUMB */
/* The game's picture on its own, into <folder>\THUMBS\<THEME>.THM, the
   moment an install starts - before its files, and without touching the
   pack, whose file count the resume note keeps. Quiet, and never fatal:
   no picture is 0 as well. */
static int doThumb(IpAddr_t addr, uint16_t port, const char *key, const char *folder) {
  char path[120], out[160], theme[12];
  unsigned char thm[1024];
  unsigned len = 0;
  unsigned i;
  if (!artKind[0] || stricmp(artKind, "0") == 0 || stricmp(artKind, "none") == 0) return 0;
  strcpy(theme, themeName[0] ? themeName : "exodos");
  sprintf(path, "/thumb/%s%cart=%s&theme=%s&thumb=36x10", key, strchr(key, '?') ? '&' : '?', artKind, theme);
  int16_t n; uint16_t start;
  if (httpGet(addr, port, path, &n, &start)) return 0;     /* none, or no server: the pack brings it later */
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    if (n > (int16_t)start) {
      unsigned k = (unsigned)(n - start);
      if (len + k > sizeof(thm)) return 0;                 /* not a picture of ours */
      memcpy(thm + len, inBuf + start, k); len += k;
    }
    start = 0;
    n = recvStep();
    if (n == -2) return 2;
    if (n == -1) break;
    if (n == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(10000ul)) return 0;
      continue;
    }
    t0 = TIMER_GET_CURRENT();
  }
  if (len < 8 || memcmp(thm, "W86T", 4) != 0) return 0;
  for (i = 0; theme[i]; i++) theme[i] = (char)toupper((unsigned char)theme[i]);
  mkdir(folder);
  sprintf(out, "%s\\THUMBS", folder);
  mkdir(out);
  sprintf(out, "%s\\THUMBS\\%s.THM", folder, theme);
  FILE *f = fopen(out, "wb");
  if (!f) return 0;
  fwrite(thm, 1, len, f);
  fclose(f);
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

static void destPath(char *full, const char *rel) {
  if (cdromDir && strnicmp(rel, "CD\\", 3) == 0) {   /* the disc goes to the CD folder */
    mkdir(cdromDir);
    sprintf(full, "%s\\%s", cdromDir, rel + 3);
  } else {
    ensureDirs(rel);
    sprintf(full, "%s\\%s", gameRoot, rel);
  }
}

/* ------------------------------------------------------------- CRC32 */
/*
 * The stream is only as trustworthy as TCP's 16-bit checksum, which is
 * not much over a few hundred megabytes on tired ISA hardware. Ask for
 * ?crc=1 and the server follows each file with "C <crc32>"; the same
 * number is worked out here as the bytes are written, so a file that
 * arrived damaged is caught there and then rather than when the game
 * fails to start. Same polynomial as zlib.
 */
static uint16_t crcLo[256], crcHi[256];   /* the table, in 16-bit halves */

static void crcInit(void) {
  unsigned long c;
  int i, k;
  for (i = 0; i < 256; i++) {
    c = (unsigned long)i;
    for (k = 0; k < 8; k++)
      c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : (c >> 1);
    crcLo[i] = (uint16_t)c;
    crcHi[i] = (uint16_t)(c >> 16);
  }
}

/*
 * One byte at a time, by hand. An 8086 has no 32-bit register, so the
 * compiled "crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)" juggles two words
 * for every operation; with the CRC held in DX:BX the shift by eight is
 * three byte moves. About 13 instructions a byte against 24, which on a
 * 486 fetching at 160 KB/s is the difference between the checksum being
 * noticed and not. 8086 opcodes only: shifts by one, LODSB, LOOP.
 */
static unsigned long crcAdd(unsigned long crc, const uint8_t *p, uint16_t n) {
  uint16_t lo = (uint16_t)crc, hi = (uint16_t)(crc >> 16);
  if (!n) return crc;
  _asm {
    push ds
    push si
    push di
    push es
    mov  bx, lo
    mov  dx, hi
    mov  cx, n
    mov  ax, seg crcLo
    mov  es, ax
    lds  si, p
  next:
    lodsb
    xor  al, bl
    xor  ah, ah
    shl  ax, 1
    mov  di, ax
    mov  bl, bh
    mov  bh, dl
    mov  dl, dh
    xor  dh, dh
    xor  bx, es:crcLo[di]
    xor  dx, es:crcHi[di]
    loop next
    pop  es
    pop  di
    pop  si
    pop  ds
    mov  lo, bx
    mov  hi, dx
  }
  return ((unsigned long)hi << 16) | lo;
}

/* ------------------------------------------------ resuming a download */
/*
 * A game can be 300 MB and a 486 is not quick, so a download that stops
 * half way - Esc, a dropped line, someone tripping over the cable - must
 * not have to start again. WAVE86.RSM in the game's folder says how many
 * of its files are already there and which request they belong to; the
 * next attempt asks the server to start after them (?from=N) and the
 * server says what it left out. The note is written whenever a transfer
 * ends early and removed when one finishes.
 */
static char rsmPath[140] = "";
static const char *rsmKey = NULL;       /* NULL: not that kind of transfer */
static int doneFiles = 0;               /* whole files on the disk, this game */
static unsigned long partBytes = 0;     /* ... and this much of the next one */
static unsigned long partCrc = 0xFFFFFFFFUL;    /* its CRC so far, unfinished */
static int partCrcKnown = 1;            /* 0 when netcrc=0 kept no CRC */
static unsigned long resumeAt = 0;      /* read back from the note */
static unsigned long resumeCrc = 0xFFFFFFFFUL;
static int resumeCrcKnown = 0;

#define CHECKPOINT (4UL * 1024 * 1024)  /* commit and note every 4 MB of a file */

/* The note: files|bytes|crc|request. A note from before bytes were kept
   is files|request, and means the same with none of the next file. */
static int resumeRead(void) {
  FILE *f;
  char line[128], *field[4], *e;
  int n = 0, nf = 0;
  resumeAt = 0;
  resumeCrcKnown = 0;
  if (!rsmKey) return 0;
  f = fopen(rsmPath, "r");
  if (!f) return 0;
  if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
  fclose(f);
  e = line + strlen(line);
  while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
  for (field[nf++] = line, e = line; *e && nf < 4; e++)
    if (*e == '|') { *e = 0; field[nf++] = e + 1; }
  if (nf == 4) {
    if (stricmp(field[3], rsmKey) != 0) return 0;  /* another request: start over */
    n = atoi(field[0]);
    resumeAt = strtoul(field[1], NULL, 10);
    if (field[2][0] != '-') {
      resumeCrc = strtoul(field[2], NULL, 16);
      resumeCrcKnown = 1;
    }
  } else if (nf == 2) {
    if (stricmp(field[1], rsmKey) != 0) return 0;
    n = atoi(field[0]);
  }
  return n < 0 ? 0 : n;
}

static void resumeWrite(int files, unsigned long bytes, unsigned long crc, int crcKnown) {
  FILE *f;
  if (!rsmKey || (files <= 0 && bytes == 0)) return;
  f = fopen(rsmPath, "w");
  if (!f) return;
  if (crcKnown)
    fprintf(f, "%d|%lu|%lX|%s\n", files, bytes, crc, rsmKey);
  else
    fprintf(f, "%d|%lu|-|%s\n", files, bytes, rsmKey);
  fclose(f);
}

/* what has been written so far really on the disk, FAT and all, so a note
   written now still holds after the power goes: DOS 3.3's commit */
static void commitFile(FILE *fp) {
  union REGS r;
  fflush(fp);
  r.h.ah = 0x68;
  r.x.bx = (unsigned)fileno(fp);
  intdos(&r, &r);
}

/*
 * DOS took fewer bytes than it was given. That is a full disk often enough
 * for "is it full?" to be the usual guess, but it is also a drive that went
 * away, a redirector with a file size limit, or a cluster count the driver
 * got wrong - so say which file, how far into it, what DOS called the error
 * and how much room that drive says it has.
 */
static void writeFailed(const char *path, unsigned long at) {
  char m[140];
  struct diskfree_t df;
  unsigned drive = (path[1] == ':') ? (unsigned)(toupper((unsigned char)path[0]) - 'A' + 1) : 0;
  unsigned long freeKb = 0;
  int known = _dos_getdiskfree(drive, &df) == 0;
  if (known) {
    unsigned long cluster = (unsigned long)df.sectors_per_cluster * df.bytes_per_sector;
    freeKb = (unsigned long)df.avail_clusters * (cluster / 512) / 2;
  }
  char why[24];
  if (_doserrno)                    /* a driver that said what went wrong */
    sprintf(why, "DOS error %d", _doserrno);
  else                              /* fewer bytes taken, nothing said */
    strcpy(why, "short write");
  if (known)
    sprintf(m, "WAVEGET: write failed %luK into %.40s (%s, %luK free there)",
            at / 1024, path, why, freeKb);
  else
    sprintf(m, "WAVEGET: write failed %luK into %.40s (%s)", at / 1024, path, why);
  msg(m);
}

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
static int streamBody(IpAddr_t addr, uint16_t port, const char *path, const char *what,
                      unsigned long kbTotal, int safe) {
  int16_t n; uint16_t pos;
  int rc;
  uiStatus(askText[0] ? askText : "asking the server ...");   /* a reconnect says why */
  askText[0] = 0;
  gaveUpWaiting = 0;
  rc = httpGet(addr, port, path, &n, &pos);
  if (rc) return rc;
  noHeaders = 0;
  uiStatus("");

  char line[200]; uint16_t lineLen = 0;
  FILE *cur = NULL; unsigned long remain = 0, done = 0, got = 0; int files = 0;
  /* done counts what is on the disk, resumed bytes included, for the
     progress; got only what this connection brought, for the rate */
  char curName[80] = "", full[160], tmp[160];
  unsigned long curSize = 0;
  int renaming = 0, checked = 0, unverified = 0, skipped = 0, waited = 0;
  int pendingRename = 0;            /* a whole file in its .TMP name, waiting for its C line */
  unsigned long crc = 0xFFFFFFFFUL, lastCrc = 0, lastCheckpoint = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT(), tStart = t0, tShow = t0, tWait = t0, tIdle = t0;
  /* silence ends the attempt after netreconnect= seconds when there is
     a next attempt, else after nettimeout= */
  unsigned long silence = reconnectWait() ? reconnectWait() : IDLE_TIMEOUT;
  int idleShown = 0;                /* the "nothing from the server" line is up */
  int kickJustNow = 0;              /* bytes came within a second of a kick: what were they? */
  int finished = 0;
  clockTicks_t tKick = t0;          /* the last kick, or the last data */
  int kickPending = 0, kickedThisSilence = 0;
  uint32_t arpAt = 0, rxAt = 0, badAt = 0;   /* the counters when the silence began */
  tLastData = t0;

  while (!finished) {
    while (pos < (uint16_t)n) {
      if (cur) {                                   /* file data */
        uint16_t chunk = n - pos;
        if (kickJustNow) { rescued++; kickJustNow = 0; }   /* it was data: the kick woke the way in */
        if ((unsigned long)chunk > remain) chunk = (uint16_t)remain;
        if (writeMax && chunk > writeMax) chunk = writeMax;   /* netwrite= */
        _doserrno = 0;
        {
          /* A write is timed: a drive that stalls (the card's SD through
             PMDFS has) is not the server going quiet, and the wait is
             taken out of the timeout and the rate - and said. */
          clockTicks_t tw = TIMER_GET_CURRENT();
          size_t wrote = fwrite(inBuf + pos, 1, chunk, cur);
          unsigned long ms = Timer_diff(tw, TIMER_GET_CURRENT()) * TIMER_TICK_LEN;
          if (ms > slowWriteMs) slowWriteMs = ms;
          if (ms >= 2000) {
            char m[80];
            sprintf(m, "the disk took %lu s to write %u bytes of %.24s", ms / 1000, chunk, curName);
            if (screenOn) uiStatus(m); else printf("\r%-70.70s", m);
            t0 = TIMER_GET_CURRENT();
            tStart += Timer_diff(tw, t0);
          }
          if (wrote != chunk) {
            writeFailed(renaming ? tmp : full, curSize - remain);
            fclose(cur);
            return 1;
          }
        }
        if (crcWanted) crc = crcAdd(crc, inBuf + pos, chunk);
        pos += chunk; remain -= chunk; done += chunk; got += chunk;
        partBytes = curSize - remain;    /* safely written: resume from here */
        partCrc = crc;
        partCrcKnown = crcWanted && !unverified;
        if (!safe && remain && partBytes - lastCheckpoint >= CHECKPOINT) {
          commitFile(cur);               /* so the note survives a power cut */
          resumeWrite(doneFiles, partBytes, partCrc, partCrcKnown);
          lastCheckpoint = partBytes;
        }
        if (remain == 0) {
          fclose(cur); cur = NULL; files++; doneFiles = files;
          partBytes = 0;
          lastCrc = crc ^ 0xFFFFFFFFUL;
          if (renaming) pendingRename = 1;   /* into place once its checksum has been seen */
          uiLog(curName);
        }
      } else {                                     /* a header line */
        char c = inBuf[pos++];
        if (c != '\n') { if (lineLen < sizeof(line) - 1) line[lineLen++] = c; continue; }
        line[lineLen] = 0; lineLen = 0;
        if (kickJustNow) { if (line[0] != 'W') rescued++; kickJustNow = 0; }   /* a W line was coming anyway */
        if (pendingRename && line[0] != 'C') {   /* no checksum for it: into place as it came */
          remove(full); rename(tmp, full); pendingRename = 0;
        }
        if (waited && (line[0] == 'F' || line[0] == 'P')) {   /* the wait is not the network's fault: keep it out of the rate */
          waited = 0;
          tStart += Timer_diff(tWait, TIMER_GET_CURRENT());
          if (screenOn) uiStatus("");
        }
        if (line[0] == 'E') { finished = 1; break; }
        if (line[0] == 'C') {           /* the file's checksum, from the server */
          unsigned long want = strtoul(line + 2, NULL, 10);
          if (!crcWanted || unverified) { unverified = 0; }
          else if (want != lastCrc) {
            char m[120];
            if (pendingRename) { remove(tmp); pendingRename = 0; }   /* the old file stays */
            sprintf(m, "WAVEGET: %.40s came through damaged - fetch it again", curName);
            files--; doneFiles = files;   /* so the next run starts at it */
            partBytes = 0;                /* ... from its first byte */
            if (doneFiles <= 0 && rsmKey) remove(rsmPath);   /* nothing before it: no note, a clean request */
            msg(m);
            return 1;
          } else
            checked++;
          if (pendingRename) {          /* it is what the server sent: into place */
            remove(full); rename(tmp, full); pendingRename = 0;
          }
          continue;
        }
        if (line[0] == 'W') {           /* the server is waiting for its own source */
          const char *sp = strchr(line + 2, ' ');
          if (screenOn) uiStatus(sp ? sp + 1 : "the server is fetching it");
          else printf("\r%-70.70s", sp ? sp + 1 : "the server is fetching it");
          if (!waited) tWait = TIMER_GET_CURRENT();
          waited = 1;
          tLastW = TIMER_GET_CURRENT();
          continue;
        }
        if (line[0] == 'T') {           /* what the pack really holds, in bytes */
          const char *sp = strchr(line + 2, ' ');
          if (sp) kbTotal = (strtoul(sp + 1, NULL, 10) + 1023) / 1024;
          continue;
        }
        if (line[0] == 'X') {           /* it gave up, and says why */
          char m[100];
          sprintf(m, "WAVEGET: the server: %.70s", line + 2);
          msg(m);
          if (cur) fclose(cur);
          return 1;
        }
        if (line[0] == 'S') {           /* resumed: what the server left out */
          char m[60];
          const char *sp = strchr(line + 2, ' ');
          files = atoi(line + 2);
          skipped = files;              /* checked in an earlier session */
          done = sp ? strtoul(sp + 1, NULL, 10) : 0;
          doneFiles = files;
          sprintf(m, "carrying on after %d files already here", files);
          uiStatus(m);
          continue;
        }
        if (line[0] == 'P') {           /* a file we have the start of */
          char *after, m[100];
          unsigned long size = strtoul(line + 2, &after, 10);
          unsigned long at = strtoul(after, &after, 10);
          const char *rel = after;
          long have = -1;
          while (*rel == ' ') rel++;
          destPath(full, rel);
          strncpy(curName, rel, sizeof(curName) - 1);
          cur = fopen(full, "r+b");
          if (cur) { fseek(cur, 0L, SEEK_END); have = ftell(cur); }
          if (!cur || have < (long)at) {
            /* not what the note said: that file from scratch, next time */
            if (cur) fclose(cur);
            cur = NULL;
            partBytes = 0;
            sprintf(m, "WAVEGET: %.40s is shorter than the note says; run it again", rel);
            msg(m);
            return 1;
          }
          fseek(cur, (long)at, SEEK_SET);
          renaming = 0;
          curSize = size; remain = size - at; done += at;
          crc = resumeCrcKnown ? resumeCrc : 0xFFFFFFFFUL;
          unverified = !resumeCrcKnown;  /* the start was never checksummed */
          partBytes = at; partCrc = crc; lastCheckpoint = at;
          partCrcKnown = crcWanted && resumeCrcKnown;   /* a note written before the first byte says the truth */
          sprintf(m, "carrying on %luK into %.40s", at / 1024, rel);
          uiStatus(m);
          if (remain == 0) {
            fclose(cur); cur = NULL; files++; doneFiles = files;
            partBytes = 0;
            lastCrc = crc ^ 0xFFFFFFFFUL;
            uiLog(curName);
          }
          continue;
        }
        if (line[0] != 'F') { char m[100]; sprintf(m, "WAVEGET: bad stream: %.60s", line); msg(m); return 1; }
        if (files && !checked)          /* no C line after the first file */
          crcWanted = 0;
        unsigned long size = strtoul(line + 2, NULL, 10);
        curSize = size;
        const char *rel = strchr(line + 2, ' ');
        if (!rel) return 1;
        rel++;
        destPath(full, rel);
        renaming = 0;
        if (safe) { tempName(tmp, full); renaming = strcmp(tmp, full) != 0; }
        strncpy(curName, rel, sizeof(curName) - 1);
        cur = fopen(renaming ? tmp : full, "wb");
        if (!cur) { char m[120]; sprintf(m, "WAVEGET: cannot create %.90s", renaming ? tmp : full); msg(m); return 1; }
        remain = size;
        crc = 0xFFFFFFFFUL;
        unverified = 0;
        partBytes = 0; lastCheckpoint = 0;
        if (size == 0) {
          fclose(cur); cur = NULL; files++; doneFiles = files;
          lastCrc = crc ^ 0xFFFFFFFFUL;     /* nothing in it: CRC 0 */
          if (renaming) pendingRename = 1;   /* into place once its checksum has been seen */
          uiLog(curName);
        }
      }
    }
    if (finished) break;
    if (Timer_diff(tShow, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(screenOn ? 200 : 500)) {
      tShow = TIMER_GET_CURRENT();
      unsigned long secs = Timer_diff(tStart, tShow) * TIMER_TICK_LEN / 1000;
      unsigned long kbs = secs ? (got / 1024) / secs : 0;
      if (screenOn) uiProgress(curName, done, kbTotal, kbs, secs);
      else if (kbTotal) printf("\r%-24s %6luK of %luK  %3lu%%  %4lu KB/s   ", curName, done / 1024, kbTotal, (done / 1024) * 100 / kbTotal, kbs);
      else         printf("\r%-24s %6luK  %4lu KB/s   ", curName, done / 1024, kbs);
    }
    n = recvStep(); pos = 0;
    if (n == -2) { if (cur) fclose(cur); msg("WAVEGET: aborted"); return 2; }
    if (n == -1) { msg("WAVEGET: the connection dropped mid-way"); if (cur) fclose(cur); return 1; }
    if (n == 0) {
      clockTicks_t now = TIMER_GET_CURRENT();
      unsigned long idle = Timer_diff(t0, now) * TIMER_TICK_LEN / 1000;
      /* a wait of any length is shown as it goes, so a screen that does
         not move is known to be waiting and not gone */
      if (idle >= 3 && Timer_diff(tIdle, now) > TIMER_MS_TO_TICKS(1000)) {
        char m[80];
        tIdle = now;
        sprintf(m, "nothing from the server for %lu s (%s at %lu)", idle,
                reconnectWait() ? "connects again" : "gives up", silence / 1000);
        if (screenOn) uiStatus(m); else printf("\r%-70.70s", m);
        idleShown = 1;
      }
      /* the kick: at three seconds of silence (seven while the server was
         sending its wait lines, which come every three), then every six,
         so data that follows one within a second is unlikely to be chance */
      if (idle >= (waited ? 7 : 3) && Timer_diff(tKick, now) >= TIMER_MS_TO_TICKS(kickedThisSilence ? 6000 : 3000)) {
        IpAddr_t target;
        if (!kickedThisSilence) {
          arpAt = Arp::RepliesReceived; rxAt = Packets_received; badAt = Tcp::ChecksumErrors + Ip::badChecksum;
          silentArp = silentRx = silentBad = 0;
        }
        kickTarget(target);
        Arp::sendArpRequestPacket(target);
        kicks++; tKick = now; kickPending = 1; kickedThisSilence = 1;
      }
      if (kickedThisSilence) {
        if (Arp::RepliesReceived != arpAt) silentArp = 1;
        if (Packets_received != rxAt) silentRx = 1;
        if (Tcp::ChecksumErrors + Ip::badChecksum != badAt) silentBad = 1;
      }
      if (Timer_diff(t0, now) > TIMER_MS_TO_TICKS(silence)) {
        /* the caller decides whether that is the end or a new connection,
           and says so: what it needs is kept here */
        gaveUpWaiting = 1;
        noHeaders = 0;
        silentSecs = idle;
        if (curName[0]) {               /* else the last known place stands */
          silentAtK = (curSize - remain) / 1024;
          strncpy(silentName, curName, sizeof(silentName) - 1);
          silentName[sizeof(silentName) - 1] = 0;
        }
        if (cur) fclose(cur);
        return 1;
      }
      n = 0; continue;
    }
    if (idleShown) { idleShown = 0; if (screenOn) uiStatus(""); else printf("\r%-70s\r", ""); }
    t0 = tLastData = TIMER_GET_CURRENT();
    if (kickPending) {                /* bytes right after a kick: counted once seen for what they are */
      kickPending = 0;
      if (Timer_diff(tKick, t0) < TIMER_MS_TO_TICKS(1000)) kickJustNow = 1;
    }
    kickedThisSilence = 0;
    tKick = t0;
  }
  unsigned long secs = Timer_diff(tStart, TIMER_GET_CURRENT()) * TIMER_TICK_LEN / 1000;
  char done_line[120];
  sprintf(done_line, "WAVEGET: %s ready: %d files, %luK in %lu s%s", what, files, done / 1024, secs,
          checked && checked >= files - skipped ? ", checksums good" : "");
  if (reconnects || rescued) {
    char extra[60];
    sprintf(extra, "%s%s", reconnects ? " after reconnecting" : "", rescued ? " (stalls kicked awake)" : "");
    if (strlen(done_line) + strlen(extra) < sizeof(done_line)) strcat(done_line, extra);
  }
  if (screenOn) { uiProgress(curName, done, kbTotal, secs ? (got / 1024) / secs : 0, secs); msg(done_line); }
  else printf("\r%-70s\n", done_line);
  logLine(done_line + 9);
  return 0;
}

/* the stream, with the resume note kept up to date around it */
static int doStream(IpAddr_t addr, uint16_t port, const char *path, const char *what,
                    unsigned long kbTotal, int safe) {
  int rc;
  doneFiles = 0;
  partBytes = 0;
  rc = streamBody(addr, port, path, what, kbTotal, safe);
  if (rc == 0) {
    if (rsmKey) remove(rsmPath);        /* it is all here now */
  } else {
    resumeWrite(doneFiles, partBytes, partCrc, partCrcKnown);   /* from here next time */
  }
  return rc;
}

/* the request for a game: the whole of it, or the rest after what the
   note says is here (from= files and at= bytes of the next) */
static void packPath(char *path, const char *k, int from, unsigned long at) {
  sprintf(path, "/pack/%s", k);         /* the key may carry ?cd=... already */
  if (from || at)
    sprintf(path + strlen(path), "%cfrom=%d", strchr(path, '?') ? '&' : '?', from);
  if (at)
    sprintf(path + strlen(path), "&at=%lu", at);
  if (crcWanted)
    sprintf(path + strlen(path), "%ccrc=1", strchr(path, '?') ? '&' : '?');
  /* wait=1: we know W, T and X lines - a server that fetches the game from
     somewhere itself may take its time and say so, instead of going quiet */
  sprintf(path + strlen(path), "%cwait=1", strchr(path, '?') ? '&' : '?');
  /* and a picture of it for the details pane, unless netart=0 */
  if (artKind[0] && stricmp(artKind, "0") != 0 && stricmp(artKind, "none") != 0)
    sprintf(path + strlen(path), "&art=%s&theme=%s&thumb=36x10", artKind, themeName[0] ? themeName : "exodos");
}

static int doGet(IpAddr_t addr, uint16_t port, const char *dir, const char *gamesdir, unsigned long kbTotal, const char *key) {
  char path[160];
  const char *k = key ? key : dir;
  int from, rc, tries = 0;
  clockTicks_t tProgress;
  sprintf(gameRoot, "%s\\%s", gamesdir, dir);
  mkdir(gamesdir);
  mkdir(gameRoot);
  sprintf(rsmPath, "%s\\WAVE86.RSM", gameRoot);
  rsmKey = k;
  from = resumeRead();
  packPath(path, k, from, resumeAt);
  uiOpen(dir, serverText);
  /* The stream, again while silence ends it and the connections bring
     something: each new one asks for the rest, from the note the last
     left behind. Connections that bring nothing new for nettimeout=
     seconds in all end it, with a message that says what stood still. */
  slowWriteMs = 0;
  kicks = rescued = 0; reconnects = sinceProgress = 0;
  logged = 0; lastLog[0] = 0; tLastW = 0; silentName[0] = 0; silentAtK = 0;
  mayReconnect = 1;
  tProgress = TIMER_GET_CURRENT();
  for (;;) {
    int fromBefore = from;
    unsigned long atBefore = resumeAt, quiet;
    clockTicks_t now;
    rc = doStream(addr, port, path, dir, kbTotal, 0);
    if (rc != 1) break;                       /* done, or aborted */
    /* silence ends an attempt; so does a reconnect that got no answer
       (the server may be a moment away): anything else was an error
       already said, and is the end */
    if (!gaveUpWaiting && !(tries && connectFailed)) break;
    quiet = Timer_diff(tLastData, TIMER_GET_CURRENT()) * TIMER_TICK_LEN / 1000;   /* before the close wait */
    dropSocket();                             /* the old one, before its successor */
    {
      unsigned long crcBefore = resumeCrc;
      int crcKnownBefore = resumeCrcKnown;
      from = resumeRead();                    /* what the attempt left behind */
      /* silence between the S line and the P line leaves a note without
         the bytes of the file that was on its way: the last note knew */
      if (from == fromBefore && resumeAt == 0 && atBefore) {
        resumeAt = atBefore; resumeCrc = crcBefore; resumeCrcKnown = crcKnownBefore;
        resumeWrite(from, resumeAt, resumeCrc, resumeCrcKnown);
      }
    }
    now = TIMER_GET_CURRENT();
    if (from != fromBefore || resumeAt != atBefore) { tProgress = now; sinceProgress = 0; }
    /* the server saying it was waiting (W lines) is not nothing: a wait
       of its own, of any length, keeps the run alive as it always has */
    if (tLastW && Timer_diff(tProgress, tLastW) < 0x80000000ul) tProgress = tLastW;
    if (!reconnectWait() || Timer_diff(tProgress, now) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) {
      char m[120], where[40] = "";
      const char *saw;
      unsigned long secs = tries ? Timer_diff(tProgress, now) * TIMER_TICK_LEN / 1000 : quiet;
      if (silentName[0]) sprintf(where, ", %luK into %.12s", silentAtK, silentName);
      /* what the silence looked like from here decides the words: the
         ARP reply that came back means the way in works and the server
         sent nothing; none, and no packet at all, means nothing reaches
         the card; frames failing their sums were read wrong off it -
         and the log has the counters either way */
      if (noHeaders && !tries)      saw = "no reply to the request in";
      else if (kicks && silentBad)  saw = "packets came in damaged for";
      else if (kicks && !silentArp && !silentRx) saw = "nothing reaches the card for";
      else if (kicks && silentArp)  saw = "the server sent nothing for";
      else                          saw = "nothing from the server for";
      if (slowWriteMs >= 2000)
        sprintf(m, "WAVEGET: no data for %lu s%s; disk stalls: %lu s", secs, where, slowWriteMs / 1000);
      else if (tries)
        sprintf(m, "WAVEGET: %s %lu s over %d connections%s", saw, secs, sinceProgress + 1, where);
      else
        sprintf(m, "WAVEGET: %s %lu s%s", saw, secs, where);
      msg(m);
      logLine(m + 9);
      break;
    }
    tries++; sinceProgress++;
    reconnects = tries;
    /* the words the next attempt shows while it asks: why there is one */
    if (connectFailed)
      sprintf(askText, "connecting again (%d): no answer last time", tries);
    else if (noHeaders)
      sprintf(askText, "connecting again (%d): no reply for %lu s", tries, silentSecs);
    else if (silentName[0])
      sprintf(askText, "connecting again (%d): silent %lu s at %luK of %.12s", tries, silentSecs, silentAtK, silentName);
    else
      sprintf(askText, "connecting again (%d): silent %lu s", tries, silentSecs);
    if (!screenOn) printf("\r%-70.70s\n", askText);
    packPath(path, k, from, resumeAt);
  }
  mayReconnect = 0;
  logLast(rc);
  uiClose();
  return rc;
}

/* ------------------------------------------------------- UPDATE */
/* WAVE86.EXE and its helpers, straight from the machine that builds them,
   into dir (the launcher's folder). Run from the batch loop, so the
   launcher itself is not in memory when its EXE is replaced. */
/* ------------------------------------- the INI, after an update */
/* WAVE86.DEF comes down with an update: the shipped WAVE86.INI, every
   setting with its comment above it. The machine's INI keeps every value
   it has; a setting it lacks - one newer than the file - is added at the
   end of its global part, comment and all, at its default, so the file
   goes on documenting itself. Returns how many were added, their names
   in `names`. */
#define INI_KEY_MAX 96
static char iniKeys[INI_KEY_MAX][20];
static int iniKeyN = 0;

static void iniKeyOf(const char *line, char *key) {
  int i = 0;
  while (*line == ' ' || *line == '\t') line++;
  while (*line && *line != '=' && *line != ' ' && *line != '\t' && *line != '\r' && *line != '\n' && i < 19)
    key[i++] = (char)tolower((unsigned char)*line++);
  key[i] = 0;
}

static int iniHas(const char *key) {
  int i;
  for (i = 0; i < iniKeyN; i++) if (stricmp(iniKeys[i], key) == 0) return 1;
  return 0;
}

static int mergeIni(const char *dir, char *names, unsigned namesLen) {
  char ini[160], def[160], tmp[160], line[200], block[1200], key[20], sect[200];
  FILE *fi, *fd, *ft;
  int added = 0, hadSection = 0;
  unsigned blockLen = 0;
  sprintf(ini, "%s\\WAVE86.INI", dir);
  sprintf(def, "%s\\WAVE86.DEF", dir);
  sprintf(tmp, "%s\\WAVE86.TMP", dir);
  names[0] = 0;
  fd = fopen(def, "r");
  if (!fd) return 0;
  fi = fopen(ini, "r");
  if (!fi) { fclose(fd); return 0; }
  iniKeyN = 0;                      /* the settings the INI has, in its global part */
  while (fgets(line, sizeof(line), fi)) {
    const char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '[') break;
    if (*s == ';' || *s == '#' || *s == '\r' || *s == '\n' || !*s || !strchr(s, '=')) continue;
    if (iniKeyN < INI_KEY_MAX) { iniKeyOf(s, iniKeys[iniKeyN]); if (iniKeys[iniKeyN][0]) iniKeyN++; }
  }
  rewind(fi);
  ft = fopen(tmp, "w");
  if (!ft) { fclose(fi); fclose(fd); return 0; }
  while (fgets(line, sizeof(line), fi)) {       /* the INI's global part, as it is */
    const char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '[') { strcpy(sect, line); hadSection = 1; break; }
    fputs(line, ft);
  }
  while (fgets(line, sizeof(line), fd)) {       /* the blocks the INI lacks */
    const char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '[' || (s[0] == ';' && s[1] == '[')) break;   /* the games: not settings */
    if (*s == '\r' || *s == '\n' || !*s) { blockLen = 0; block[0] = 0; continue; }
    if (*s == ';') {
      unsigned n = strlen(line);
      if (blockLen + n < sizeof(block)) { strcpy(block + blockLen, line); blockLen += n; }
      continue;
    }
    if (!strchr(s, '=')) continue;
    iniKeyOf(s, key);
    /* gamedir= is left alone: without the line the launcher looks in the
       GAMES folder beside it, and the shipped C:\GAMES would move a
       setup that relied on that */
    if (key[0] && !iniHas(key) && stricmp(key, "gamedir") != 0) {
      if (!added) fputs("\n; ---- Added by WAVEGET UPDATE: settings this file did not have yet, at their defaults.\n", ft);
      fputs(block, ft);                 /* the stream is text mode: \n comes out as CR LF */
      fputs(line, ft);
      if (strlen(names) + strlen(key) + 2 < namesLen) { if (names[0]) strcat(names, ", "); strcat(names, key); }
      added++;
      blockLen = 0; block[0] = 0;       /* the comment went with it; a group's next key stands alone */
    }
  }
  if (added && hadSection) fputs("\n", ft);
  if (hadSection) {                             /* the games, as they were */
    fputs(sect, ft);
    while (fgets(line, sizeof(line), fi)) fputs(line, ft);
  }
  {
    int bad = ferror(ft);               /* a full disk: the INI stays as it was */
    if (fclose(ft) != 0) bad = 1;
    fclose(fi); fclose(fd);
    if (!added || bad) { remove(tmp); return 0; }
  }
  sprintf(def, "%s\\WAVE86.BAK", dir);       /* the old file is a step away until the new one is in place */
  remove(def);
  if (rename(ini, def) != 0) { remove(tmp); return 0; }
  if (rename(tmp, ini) != 0) { rename(def, ini); remove(tmp); return 0; }
  remove(def);
  return added;
}

static int doUpdate(IpAddr_t addr, uint16_t port, const char *dir) {
  strncpy(gameRoot, dir && dir[0] ? dir : ".", sizeof(gameRoot) - 1);
  gameRoot[sizeof(gameRoot) - 1] = 0;
  int len = strlen(gameRoot);       /* C:\WAVE86\ -> C:\WAVE86 */
  if (len > 1 && gameRoot[len - 1] == '\\' && gameRoot[len - 2] != ':') gameRoot[len - 1] = 0;
  rsmKey = NULL;
  uiOpen("WAVE86 ITSELF", serverText);
  if (!screenOn) printf("WAVEGET: updating WAVE86 in %s\n", gameRoot);
  logged = 0; lastLog[0] = 0;
  int rc = doStream(addr, port, crcWanted ? "/update?crc=1" : "/update", "update", 0, 1);
  if (rc == 3) {                    /* a server too old to know the query */
    crcWanted = 0;
    dropSocket();                   /* the one socket there is, before the next connect */
    rc = doStream(addr, port, "/update", "update", 0, 1);
  }
  if (rc == 1 && gaveUpWaiting && !noHeaders) {   /* the stream leaves the words to its caller */
    char m[100];
    sprintf(m, "WAVEGET: nothing from the server for %lu s", silentSecs);
    msg(m);
  }
  logLast(rc);
  if (rc == 0 && screenOn) {        /* it takes a second: hold it long
                                       enough to be read (a key skips) */
    clockTicks_t t0 = TIMER_GET_CURRENT(), tv = t0;
    while (!biosIsKeyReady() && Timer_diff(t0, TIMER_GET_CURRENT()) < TIMER_MS_TO_TICKS(2500)) {
      mus_poll();
      if (TIMER_GET_CURRENT() != tv) {    /* the VU wants the tick rate */
        tv = TIMER_GET_CURRENT();
        uiMusic();
      }
    }
    if (biosIsKeyReady()) biosKeyRead();
  }
  uiClose();
  if (rc == 0) {
    char names[120];
    int n = mergeIni(gameRoot, names, sizeof(names));
    if (n) printf("WAVEGET: WAVE86.INI: %d new setting%s added at %s default%s (%s), comments and all\n",
                  n, n == 1 ? "" : "s", n == 1 ? "its" : "their", n == 1 ? "" : "s", names);
  }
  return rc;
}

/* ---------------------------------------------------------- main */
static void usage(void) {
  puts("WAVEGET LIST host:port outfile\nWAVEGET GET host:port DIR gamesdir [kb [key [cdrom]]]\n"
       "WAVEGET THUMB host:port key gamefolder\n"
       "WAVEGET DISK host:port key\nWAVEGET UPDATE host:port [dir]");
  exit(1);
}

int main(int argc, char **argv) {
  IpAddr_t addr; uint16_t port;
  _harderr(critFail);
  /* /q:K/N says which of a queue this is. An argument and not an
     environment variable: a DOS environment that is full cannot take
     another SET, and that is exactly the machine this runs on. */
  {
    int i, j;
    for (i = 1; i < argc; i++)
      if (strnicmp(argv[i], "/q:", 3) == 0) {
        strncpy(queueLabel, argv[i] + 3, sizeof(queueLabel) - 1);
        queueLabel[sizeof(queueLabel) - 1] = 0;
        for (j = i; j + 1 < argc; j++) argv[j] = argv[j + 1];
        argc--;
        i--;
      }
  }
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
  {   /* mTCP's own tick counter starts at 0 in every run, so seed from the
         BIOS one: the same local port twice running lands on the server's
         TIME_WAIT socket and the SYN goes nowhere. */
    unsigned long __far *biosTicks = (unsigned long __far *)MK_FP(0x40, 0x6C);
    srand((unsigned)*biosTicks);
  }
  localPort = (uint16_t)(2048 + (rand() % 16384));
  crcInit();

  strncpy(serverText, argv[2], sizeof(serverText) - 1);
  serverText[sizeof(serverText) - 1] = 0;

  int rc = 1;
  if (stricmp(argv[1], "LIST") == 0) {
    rc = doList(addr, port, argv[3]);
  } else if (stricmp(argv[1], "DISK") == 0 && argc >= 4) {
    rc = doDisk(addr, port, argv[3]);
  } else if (stricmp(argv[1], "THUMB") == 0 && argc >= 5) {
    loadSettings(argv[0]);
    rc = doThumb(addr, port, argv[3], argv[4]);
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
    if (queueLabel[0]) {            /* a queue is running: say so and move on */
      clockTicks_t t0 = TIMER_GET_CURRENT();
      printf("That one failed. The queue carries on in 5 s (or press a key).\n");
      while (!biosIsKeyReady() && Timer_diff(t0, TIMER_GET_CURRENT()) < TIMER_MS_TO_TICKS(5000)) { }
    } else {
      puts("Press a key.");
      while (!biosIsKeyReady()) { }
    }
    if (biosIsKeyReady()) biosKeyRead();
  }
  netShutdown(rc);
  return rc;
}
