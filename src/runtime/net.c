/*
 * Multiplayer (issue #2): Theme Park's NetBIOS sessions, carried over TCP.
 *
 * The game has a complete network game for up to 4 players (docs/multiplayer.md):
 * a session between every pair of machines, and every frame each sends the
 * others its 6-byte command and its tick. Here the game's own protocol runs
 * unchanged; only the transport is replaced. One machine hosts (player 0,
 * which the game already treats as the master) and the others connect to it;
 * the host relays what players 1-3 send each other. Each pair is a byte stream
 * in order, which is all the game asks of its sessions: every receive asks
 * for exactly what the other side sent.
 *
 *   --host N         be player 0 of N, and wait for the others at start-up
 *   --join ADDR      connect to the host (ADDR or ADDR:PORT)
 *   --port P         the host's port (default 7790)
 *
 * The overrides (run_lift.py):
 *   0x4393BE  start the network   connect here, then the game's own set-up
 *   0x438A01  "is NetBIOS loaded"  yes
 *   0x439414  connect the sessions the handshake already did it: fill the tables
 *   0x438F65  send on a session   to that player
 *   0x4390DF  receive             from that player
 *   0x439B20  hang up             close
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include "compat.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "dos32.h"

#define M8(a)  (*(uint8_t*)dos32_mem(a))
#define M16(a) (*(uint16_t*)dos32_mem(a))
#define M32(a) (*(uint32_t*)dos32_mem(a))
#define ARG(n) M32(g_esp + 4 + 4 * (n))
static void ret(uint32_t v) { g_eax = v; g_esp += 4; }

#define PLAYERS   0x4C7792  /* byte: -m N */
#define NET_N     0x4A91A9  /* word: players the sessions were made for */
#define NET_ME    0x4A91A7  /* word: this machine's player number */
#define NET_DONE  0x4A91AB  /* byte: sessions made */
#define SLOT      0x4C7810  /* byte[8]: 2 this machine, 1 a connected player */
#define NCBS      0x4C7828  /* far pointer[8] (6 bytes each): each player's session */

int  net_host_players;           /* --host N */
char net_join[256];              /* --join ADDR[:PORT] */
int  net_port = 7790;            /* --port */

static int me, n;
static int netlog = -1;   /* TP_NETLOG=1: every transfer bigger than a frame's command */
extern long frame_no;     /* main.c: retrace count, 70 a second */
static void logx(const char* what, int who, uint32_t len) {
    if (netlog < 0) netlog = getenv("TP_NETLOG") != NULL;
    if (netlog && len > 6) fprintf(stderr, "[net] %.2f %s %d: %u bytes\n", frame_no / 70.0, what, who, len);
}
static SOCKET link_[4];          /* host: to player i; others: link_[0] to the host */

/* What has come in from each player and not been read yet. */
static struct { CRITICAL_SECTION lk; uint8_t* b; size_t r, w, cap; } in[4];

typedef struct { uint8_t to, from, pad[2]; uint32_t len; } hdr_t;

static void die(const char* what) {
    fprintf(stderr, "themepark: network: %s\n", what);
    exit(3);
}

static int all(SOCKET s, void* p, size_t len, int rx) {
    for (char* c = p; len; ) {
        int k = rx ? recv(s, c, (int)len, 0) : send(s, c, (int)len, 0);
        if (k <= 0) return -1;
        c += k; len -= (size_t)k;
    }
    return 0;
}

static CRITICAL_SECTION send_lk;
static void put(SOCKET s, int to, int from, const void* p, uint32_t len) {
    hdr_t h = { (uint8_t)to, (uint8_t)from, {0, 0}, len };
    EnterCriticalSection(&send_lk);   /* the host's relay threads send too */
    int bad = all(s, &h, sizeof h, 0) || all(s, (void*)p, len, 0);
    LeaveCriticalSection(&send_lk);
    if (bad) die("a player has left the game");
}

/* One per link: file what arrives, or on the host pass it on. */
static DWORD WINAPI reader(LPVOID arg) {
    SOCKET s = link_[(intptr_t)arg];
    enum { MAX = 1 << 16 };          /* the game sends at most 2048 bytes at once */
    uint8_t* buf = malloc(MAX);      /* one each: the host runs a reader per player */
    for (;;) {
        hdr_t h;
        if (all(s, &h, sizeof h, 1) || h.len > MAX || h.from >= 4 || h.to >= 4 ||
            all(s, buf, h.len, 1)) die("a player has left the game");
        if (me == 0 && h.to != 0) { put(link_[h.to], h.to, h.from, buf, h.len); continue; }
        EnterCriticalSection(&in[h.from].lk);
        if (in[h.from].w + h.len > in[h.from].cap) {
            memmove(in[h.from].b, in[h.from].b + in[h.from].r, in[h.from].w - in[h.from].r);
            in[h.from].w -= in[h.from].r; in[h.from].r = 0;
            while (in[h.from].w + h.len > in[h.from].cap) {
                in[h.from].cap = in[h.from].cap ? in[h.from].cap * 2 : 1 << 16;
                in[h.from].b = realloc(in[h.from].b, in[h.from].cap);
            }
        }
        memcpy(in[h.from].b + in[h.from].w, buf, h.len);
        in[h.from].w += h.len;
        LeaveCriticalSection(&in[h.from].lk);
    }
    return 0;
}

static void nodelay(SOCKET s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one); }

/* Make the links: the host waits for every player, then tells each its number. */
static void connect_all(void) {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    InitializeCriticalSection(&send_lk);
    for (int i = 0; i < 4; i++) InitializeCriticalSection(&in[i].lk);
    if (net_host_players) {
        n = net_host_players; me = 0;
        SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof one);
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)net_port); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(l, (struct sockaddr*)&a, sizeof a) || listen(l, 4)) die("cannot listen on that port");
        for (int i = 1; i < n; i++) {
            fprintf(stderr, "themepark: network: waiting for %d more player%s on port %d\n", n - i, n - i > 1 ? "s" : "", net_port);
            if ((link_[i] = accept(l, NULL, NULL)) == INVALID_SOCKET) die("accept failed");
            nodelay(link_[i]);
        }
        closesocket(l);
        for (int i = 1; i < n; i++) { uint8_t hello[2] = { (uint8_t)i, (uint8_t)n }; all(link_[i], hello, 2, 0); }
        for (int i = 1; i < n; i++) CloseHandle(CreateThread(NULL, 0, reader, (LPVOID)(intptr_t)i, 0, NULL));
    } else {
        char host[256]; const char* port = "7790"; char pbuf[16];
        snprintf(host, sizeof host, "%s", net_join);
        char* c = strrchr(host, ':');
        if (c) { *c = 0; port = c + 1; } else { snprintf(pbuf, sizeof pbuf, "%d", net_port); port = pbuf; }
        struct addrinfo hint = { 0 }, *r;
        hint.ai_family = AF_INET; hint.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, port, &hint, &r)) die("cannot find the host");
        fprintf(stderr, "themepark: network: joining %s:%s\n", host, port);
        for (int tries = 0;; tries++) {            /* the host may still be starting */
            link_[0] = socket(AF_INET, SOCK_STREAM, 0);
            if (!connect(link_[0], r->ai_addr, (int)r->ai_addrlen)) break;
            closesocket(link_[0]);
            if (tries == 600) die("the host did not answer");
            Sleep(100);
        }
        freeaddrinfo(r);
        nodelay(link_[0]);
        uint8_t hello[2];
        if (all(link_[0], hello, 2, 1)) die("the host closed the connection");
        me = hello[0]; n = hello[1];
        CloseHandle(CreateThread(NULL, 0, reader, (LPVOID)(intptr_t)0, 0, NULL));
    }
    fprintf(stderr, "themepark: network: player %d of %d\n", me + 1, n);
}

int net_enabled(void) { return net_host_players || net_join[0]; }

/* 0x4393BE: called by the command line's -m. */
void net_start(void (*lifted)(void)) {
    if (!net_enabled()) { fprintf(stderr, "themepark: -m needs --host N or --join ADDR\n"); ret(0xFFFFFFFFu); return; }
    connect_all();
    M8(PLAYERS) = (uint8_t)n;     /* a joining machine learns the count from the host */
    lifted();                     /* the game's set-up: its session blocks and buffers */
}

void net_present(void (*lifted)(void)) { (void)lifted; ret(0); }

/* 0x439414(players): the sessions are the links already made. */
void net_connect(void (*lifted)(void)) {
    (void)lifted;
    M16(NET_N) = (uint16_t)n;
    M16(NET_ME) = (uint16_t)me;
    M8(NET_DONE) = 1;
    for (int i = 0; i < 8; i++) M8(SLOT + i) = i == me ? 2 : i < n ? 1 : 0;
    ret((uint32_t)me);
}

/* Which player a session block belongs to. */
static int player_of(uint32_t off, uint32_t sel) {
    for (int i = 0; i < n; i++)
        if (M32(NCBS + 6 * i) == off && M16(NCBS + 6 * i + 4) == (sel & 0xFFFF)) return i;
    die("a send or receive on a session that is not a player's");
    return -1;
}

/* 0x438F65(ncb, buf, len): 0 when sent. */
void net_send(void (*lifted)(void)) {
    (void)lifted;
    int to = player_of(ARG(0), ARG(1));
    uint32_t buf = ARG(2), len = ARG(3);
    logx("send to", to, len);
    put(me == 0 ? link_[to] : link_[0], to, me, dos32_mem(buf), len);
    ret(0);
}

/* 0x4390DF(ncb, buf, len): the length received. Waits for all of it. */
void net_receive(void (*lifted)(void)) {
    (void)lifted;
    int from = player_of(ARG(0), ARG(1));
    uint32_t buf = ARG(2), len = ARG(3);
    for (;;) {
        EnterCriticalSection(&in[from].lk);
        int ok = in[from].w - in[from].r >= len;
        if (ok) { memcpy(dos32_mem(buf), in[from].b + in[from].r, len); in[from].r += len; }
        LeaveCriticalSection(&in[from].lk);
        if (ok) break;
        if (dos32_virtual_clock) { if (dos32_quit) exit(0); Sleep(1); }
        else dos32_idle(0.0005);   /* timer, keyboard and mouse still run, as in the game's own wait */
    }
    logx("receive from", from, len);
    ret(len);
}

/* 0x439B20: at exit. */
void net_hangup(void (*lifted)(void)) {
    (void)lifted;
    for (int i = 0; i < 4; i++) if (link_[i]) closesocket(link_[i]);
    ret(0);
}
