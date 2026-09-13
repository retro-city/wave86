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

static int userWantsOut(void) {
  if (CtrlBreakDetected) return 1;
  if (biosIsKeyReady()) {
    uint16_t k = biosKeyRead();
    if ((k & 0xFF) == 27 || (k & 0xFF) == 3) return 1;
  }
  return 0;
}

static void pump(void) {
  PACKET_PROCESS_MULT(5);
  Arp::driveArp();
  Tcp::drivePackets();
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
  if (!sock || sock->setRecvBuffer(TCP_RECV_BUFFER)) { puts("WAVEGET: no socket"); return -1; }
  if (sock->connectNonBlocking(2048 + rand(), addr, port)) { puts("WAVEGET: connect failed"); return -1; }
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    if (userWantsOut()) return -2;
    pump();
    if (sock->isConnectComplete()) return 0;
    if (sock->isClosed() || Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(CONNECT_TIMEOUT)) {
      puts("WAVEGET: no answer from the server");
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
  if (sendAll(req)) { puts("WAVEGET: could not send the request"); return 1; }
  hdrLen = 0;
  clockTicks_t t0 = TIMER_GET_CURRENT();
  while (1) {
    int16_t got = recvStep();
    if (got == -2) return 2;
    if (got == -1) { puts("WAVEGET: connection closed before the headers came"); return 1; }
    if (got == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) { puts("WAVEGET: timeout"); return 1; }
      continue;
    }
    t0 = TIMER_GET_CURRENT();
    for (int16_t i = 0; i < got; i++) {
      if (hdrLen < sizeof(hdrBuf) - 1) hdrBuf[hdrLen++] = inBuf[i];
      if (hdrLen >= 4 && memcmp(hdrBuf + hdrLen - 4, "\r\n\r\n", 4) == 0) {
        hdrBuf[hdrLen] = 0;
        if (strncmp(hdrBuf, "HTTP/1.", 7) != 0 || strncmp(hdrBuf + 9, "200", 3) != 0) {
          hdrBuf[hdrLen > 40 ? 40 : hdrLen] = 0;
          printf("WAVEGET: server answered: %s\n", hdrBuf);
          return 3;
        }
        *bodyStart = i + 1;
        *n = got;
        return 0;
      }
    }
  }
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
  int rc = httpGet(addr, port, path, &n, &pos);
  if (rc) return rc;

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
        if (fwrite(inBuf + pos, 1, chunk, cur) != chunk) { puts("\nWAVEGET: disk write failed (full?)"); fclose(cur); return 1; }
        pos += chunk; remain -= chunk; done += chunk;
        if (remain == 0) {
          fclose(cur); cur = NULL; files++;
          if (renaming) { remove(full); rename(tmp, full); }
        }
      } else {                                     /* a header line */
        char c = inBuf[pos++];
        if (c != '\n') { if (lineLen < sizeof(line) - 1) line[lineLen++] = c; continue; }
        line[lineLen] = 0; lineLen = 0;
        if (line[0] == 'E') { finished = 1; break; }
        if (line[0] != 'F') { printf("\nWAVEGET: bad stream: %s\n", line); return 1; }
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
        if (!cur) { printf("\nWAVEGET: cannot create %s\n", renaming ? tmp : full); return 1; }
        remain = size;
        if (size == 0) {
          fclose(cur); cur = NULL; files++;
          if (renaming) { remove(full); rename(tmp, full); }
        }
      }
    }
    if (finished) break;
    if (Timer_diff(tShow, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(500)) {
      tShow = TIMER_GET_CURRENT();
      unsigned long secs = Timer_diff(tStart, tShow) * TIMER_TICK_LEN / 1000;
      unsigned long kbs = secs ? (done / 1024) / secs : 0;
      if (kbTotal) printf("\r%-24s %6luK of %luK  %3lu%%  %4lu KB/s   ", curName, done / 1024, kbTotal, (done / 1024) * 100 / kbTotal, kbs);
      else         printf("\r%-24s %6luK  %4lu KB/s   ", curName, done / 1024, kbs);
    }
    n = recvStep(); pos = 0;
    if (n == -2) { if (cur) fclose(cur); puts("\nWAVEGET: aborted"); return 2; }
    if (n == -1) { puts("\nWAVEGET: the connection dropped mid-way"); if (cur) fclose(cur); return 1; }
    if (n == 0) {
      if (Timer_diff(t0, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(IDLE_TIMEOUT)) { puts("\nWAVEGET: timeout"); if (cur) fclose(cur); return 1; }
      n = 0; continue;
    }
    t0 = TIMER_GET_CURRENT();
  }
  unsigned long secs = Timer_diff(tStart, TIMER_GET_CURRENT()) * TIMER_TICK_LEN / 1000;
  printf("\rWAVEGET: %s ready: %d files, %luK in %lu s                    \n", what, files, done / 1024, secs);
  return 0;
}

static int doGet(IpAddr_t addr, uint16_t port, const char *dir, const char *gamesdir, unsigned long kbTotal, const char *key) {
  char path[64];
  sprintf(path, "/pack/%s", key ? key : dir);
  sprintf(gameRoot, "%s\\%s", gamesdir, dir);
  mkdir(gamesdir);
  mkdir(gameRoot);
  return doStream(addr, port, path, dir, kbTotal, 0);
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
  printf("WAVEGET: updating WAVE86 in %s\n", gameRoot);
  return doStream(addr, port, "/update", "update", 0, 1);
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

  int rc = 1;
  if (stricmp(argv[1], "LIST") == 0) {
    rc = doList(addr, port, argv[3]);
  } else if (stricmp(argv[1], "DISK") == 0 && argc >= 4) {
    rc = doDisk(addr, port, argv[3]);
  } else if (stricmp(argv[1], "UPDATE") == 0) {
    rc = doUpdate(addr, port, argc > 3 ? argv[3] : NULL);
  } else if (stricmp(argv[1], "GET") == 0 && argc >= 5) {
    if (argc > 7) cdromDir = argv[7];
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
