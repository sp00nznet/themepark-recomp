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
 *   --host N         be player 1 of N, and wait for the others at start-up
 *   --host N --resume   the host's own game, after the host crashed
 *   --join ADDR      connect to the host (ADDR or ADDR:PORT); also to rejoin
 *   --port P         the host's port (default 7790)
 *
 * Dropping out and back in. Every CHECKPOINT_EVERY frames, at the main park
 * loop's exchange, every machine saves a save state named for that frame and
 * tells the host. When a link breaks, the others wait (the title bar says for
 * whom) instead of closing. A player who crashed starts again with --join and
 * gets their old place; when everyone is back, the host picks the newest
 * checkpoint all of them have, and every machine goes back to it: its network
 * traffic is blank until it reaches the main park loop and loads its own
 * checkpoint there. A rejoining player clicks into a park (any), and their
 * own park comes back at its first frame. Each rollback starts a new epoch,
 * and traffic from an older one is dropped.
 *
 * The overrides (run_lift.py):
 *   0x4393BE  start the network   connect here, then the game's own set-up
 *   0x438A01  "is NetBIOS loaded"  yes
 *   0x439414  connect the sessions the handshake already did it: fill the tables
 *   0x438F65  send on a session   to that player
 *   0x4390DF  receive             from that player
 *   0x41CCF8  the command dispatcher  who called it; checkpoints and rollbacks
 *   0x4399F2  the frame's exchange  house rules, the count of frames in step
 *   0x439B20  hang up             close
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#define getpid _getpid
#define MSG_NOSIGNAL 0           /* Windows has no SIGPIPE */
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
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "dos32.h"
#include "net.h"
#include "rules.h"

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
#define EXCHANGE  0x004399F2u
#define FRAME_TICK 0x0040226Cu  /* the main loop's frame, in every state: it calls the dispatcher */

int  net_host_players;           /* --host N */
int  net_resume;                 /* --resume */
char net_join[256];              /* --join ADDR[:PORT] */
int  net_port = 7790;            /* --port */
int  net_wait_on_drop = 1;       /* 0: a lost player ends the game (headless runs, unless TP_NET_REJOIN) */

int net_me, net_n;               /* this machine's player number; players */
static uint32_t game_id, epoch;
static int checkpoint_every = 750;   /* frames: 30 s at 25 fps; TP_NET_CP */

/* ---- the links -------------------------------------------------------------- */

enum { DATA, SAVED, ROLLBACK, LIST, DOWN, UP, ACTION };
typedef struct { uint8_t to, from, type, pad; uint32_t epoch, len; } hdr_t;
typedef struct { uint32_t magic; uint8_t want, pad[3]; uint32_t game; } hello_t;      /* joiner -> host */
typedef struct { uint8_t id, n, rejoin, pad; uint32_t game, epoch, rules; } welcome_t;   /* host -> joiner */
#define MAGIC 0x324E5054u   /* "TPN2": the welcome carries the host's modern rules */

static CRITICAL_SECTION lk;      /* links, queues, the saved lists, the rollback */
static SOCKET link_[4];          /* host: to player i; others: link_[0], to the host */
static int up[4];                /* host: player i is connected; others: up[0], the host */
static int down_seen[4];         /* who the others are waiting for, for the title bar */

/* What has come in from each player and not been read yet. */
static struct { uint8_t* b; size_t r, w, cap; } in[4];

static int resync;                       /* traffic is blank until the rollback's load */
static uint32_t rollback_to = 0xFFFFFFFFu;
static uint32_t frame;                   /* exchanges in step (the checkpoint clock) */
static int from_frame_tick;              /* the dispatcher was called by the main loop's frame */

/* Each player's checkpoints, newest last (the host's view; each machine its own too). */
#define KEEP 6
static uint32_t saved[4][KEEP];
static int nsaved[4], listed[4];

static void rollback_local(uint32_t f);
static int netlog = -1;   /* TP_NETLOG=1: every transfer bigger than a frame's command */
extern long frame_no;     /* main.c */
static void logx(const char* fmt, ...) {
    if (netlog < 0) netlog = getenv("TP_NETLOG") != NULL;
    if (!netlog) return;
    va_list a; va_start(a, fmt);
    fprintf(stderr, "[net] %.2f ", frame_no / 70.0); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a);
}

extern char state_note[128];             /* game.c: the title bar */
extern volatile DWORD state_note_until;
void sound_reset(void);
void state_path(int slot, char* out, size_t n);

static void note(const char* fmt, int a) {
    snprintf(state_note, sizeof state_note, fmt, a);
    state_note_until = GetTickCount() + 1500;
}

static void die(const char* what) {
    fprintf(stderr, "themepark: network: %s\n", what);
    exit(3);
}

static int all(SOCKET s, void* p, size_t len, int rx) {
    for (char* c = p; len; ) {
        int k = rx ? recv(s, c, (int)len, 0) : send(s, c, (int)len, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;   /* Linux: dos32's timer signal, not a lost player */
        if (k <= 0) return -1;
        c += k; len -= (size_t)k;
    }
    return 0;
}

static void link_down(int i);

/* Send on link i (the caller holds lk). A broken link is marked down. */
static void put(int i, int to, int from, int type, const void* p, uint32_t len) {
    if (!up[i]) return;
    hdr_t h = { (uint8_t)to, (uint8_t)from, (uint8_t)type, 0, epoch, len };
    if (all(link_[i], &h, sizeof h, 0) || all(link_[i], (void*)p, len, 0)) link_down(i);
}

/* To a player: directly from the host, through it from anyone else. */
static void to_player(int to, int type, const void* p, uint32_t len) {
    put(net_me == 0 ? to : 0, to, net_me, type, p, len);
}

static void broadcast(int type, const void* p, uint32_t len) {   /* host: to every player */
    for (int i = 1; i < net_n; i++) put(i, i, 0, type, p, len);
}

/* ---- checkpoints ---------------------------------------------------------- */

static void cp_path(int player, uint32_t f, char* out, size_t n) {
    char dir[MAX_PATH];
    state_path(0, dir, sizeof dir);              /* states/slot0.tps: the folder */
    char* s = strrchr(dir, '/'); if (s) s[1] = 0;
    if (f == 0xFFFFFFFFu) snprintf(out, n, "%snet-%08x-p%d.txt", dir, game_id, player + 1);
    else snprintf(out, n, "%snet-%08x-p%d-%u.tps", dir, game_id, player + 1, f);
}

static void remember(int player, uint32_t f) {   /* lk held */
    if (nsaved[player] == KEEP) memmove(saved[player], saved[player] + 1, (KEEP - 1) * 4), nsaved[player]--;
    saved[player][nsaved[player]++] = f;
}

/* This machine's list, kept beside the checkpoints so a restarted machine knows. */
static void write_list(void) {
    char p[MAX_PATH];
    cp_path(net_me, 0xFFFFFFFFu, p, sizeof p);
    FILE* f = fopen(p, "w");
    if (!f) return;
    for (int k = 0; k < nsaved[net_me]; k++) fprintf(f, "%u\n", saved[net_me][k]);
    fclose(f);
}

static void read_list(void) {
    char p[MAX_PATH];
    unsigned v;
    cp_path(net_me, 0xFFFFFFFFu, p, sizeof p);
    FILE* f = fopen(p, "r");
    nsaved[net_me] = 0;
    if (f) { while (fscanf(f, "%u", &v) == 1) remember(net_me, v); fclose(f); }
}

/* Who played what last time, for --resume and for rejoining after a crash. */
static void last_path(char* out, size_t n) {
    state_path(0, out, n);
    char* s = strrchr(out, '/');   /* the host's and a joiner's apart: both may run from one folder */
    if (s) snprintf(s + 1, n - (size_t)(s + 1 - out), net_host_players ? "net-last-host.txt" : "net-last-join.txt");
}

static void save_last(void) {
    char p[MAX_PATH];
    last_path(p, sizeof p);
    FILE* f = fopen(p, "w");
    if (f) { fprintf(f, "%08x %d %d\n", game_id, net_me, net_n); fclose(f); }
}

static int load_last(uint32_t* game, int* me, int* n) {
    char p[MAX_PATH];
    last_path(p, sizeof p);
    FILE* f = fopen(p, "r");
    int ok = f && fscanf(f, "%x %d %d", game, me, n) == 3;
    if (f) fclose(f);
    return ok;
}

/* The host, once everyone is back: the newest checkpoint all of them have. */
static void maybe_rollback(void) {      /* lk held */
    for (int i = 0; i < net_n; i++) if (!up[i] || !listed[i]) return;
    uint32_t best = 0xFFFFFFFFu;
    for (int k = nsaved[0] - 1; k >= 0 && best == 0xFFFFFFFFu; k--) {
        int everyone = 1;
        for (int i = 1; i < net_n; i++) {
            int has = 0;
            for (int j = 0; j < nsaved[i]; j++) has |= saved[i][j] == saved[0][k];
            everyone &= has;
        }
        if (everyone) best = saved[0][k];
    }
    for (int i = 0; i < net_n; i++) listed[i] = 1;
    if (best == 0xFFFFFFFFu) {
        broadcast(DOWN, "\xFF", 1);
        die("no checkpoint that every player has: the game cannot carry on");
    }
    epoch++;
    broadcast(ROLLBACK, &best, 4);
    logx("rollback to frame %u, epoch %u", best, epoch);
    rollback_local(best);
}

static void rollback_local(uint32_t f) {   /* lk held: from now, blank until the load */
    for (int i = 0; i < 4; i++) in[i].r = in[i].w = 0;
    for (int i = 0; i < 4; i++) {           /* checkpoints past it never happened */
        while (nsaved[i] && saved[i][nsaved[i] - 1] > f) nsaved[i]--;
        down_seen[i] = 0;
    }
    rollback_to = f;
    resync = 1;
}

/* ---- reading the links ---------------------------------------------------- */

static void file_in(int from, const uint8_t* p, uint32_t len) {   /* lk held */
    if (in[from].w + len > in[from].cap) {
        memmove(in[from].b, in[from].b + in[from].r, in[from].w - in[from].r);
        in[from].w -= in[from].r; in[from].r = 0;
        while (in[from].w + len > in[from].cap) {
            in[from].cap = in[from].cap ? in[from].cap * 2 : 1 << 16;
            in[from].b = realloc(in[from].b, in[from].cap);
        }
    }
    memcpy(in[from].b + in[from].w, p, len);
    in[from].w += len;
}

static DWORD WINAPI reconnect(LPVOID arg);
static DWORD WINAPI reader(LPVOID arg);

static void link_down(int i) {          /* lk held */
    if (!up[i]) return;
    if (!net_wait_on_drop) die("a player has left the game");
    up[i] = 0;
    closesocket(link_[i]);
    if (net_me == 0) {
        listed[i] = 0;
        down_seen[i] = 1;
        uint8_t who = (uint8_t)i;
        broadcast(DOWN, &who, 1);
        fprintf(stderr, "themepark: network: player %d has left; waiting for them to rejoin (--join)\n", i + 1);
    } else {
        down_seen[0] = 1;
        fprintf(stderr, "themepark: network: lost the host; trying to reconnect\n");
        CloseHandle(CreateThread(NULL, 0, reconnect, NULL, 0, NULL));
    }
}

static DWORD WINAPI reader(LPVOID arg) {
    int i = (int)(intptr_t)arg;
    SOCKET s = link_[i];
    enum { MAX = 1 << 16 };          /* the game sends at most 2048 bytes at once */
    uint8_t* buf = malloc(MAX);      /* one each: the host runs a reader per player */
    for (;;) {
        hdr_t h;
        if (all(s, &h, sizeof h, 1) || h.len > MAX || h.from >= 4 || h.to >= 4 || all(s, buf, h.len, 1)) {
            EnterCriticalSection(&lk);
            if (link_[i] == s) link_down(i);
            LeaveCriticalSection(&lk);
            free(buf);
            return 0;
        }
        EnterCriticalSection(&lk);
        switch (h.type) {
        case DATA:
            if (h.epoch != epoch) break;                       /* from before a rollback */
            if (net_me == 0 && h.to != 0) put(h.to, h.to, h.from, DATA, buf, h.len);
            else file_in(h.from, buf, h.len);
            break;
        case ACTION:
            if (net_me == 0 && h.to != 0) put(h.to, h.to, h.from, ACTION, buf, h.len);
            else net_action_received(h.from, buf, h.len);
            break;
        case SAVED: remember(h.from, *(uint32_t*)buf); break;  /* host */
        case LIST:                                             /* host: a player is back */
            nsaved[h.from] = 0;
            for (uint32_t k = 0; k + 4 <= h.len; k += 4) remember(h.from, *(uint32_t*)(buf + k));
            listed[h.from] = 1;
            maybe_rollback();
            break;
        case ROLLBACK: epoch = h.epoch; rollback_local(*(uint32_t*)buf); break;
        case DOWN:
            if (buf[0] == 0xFF) { LeaveCriticalSection(&lk); die("the host could not find a checkpoint every player has"); }
            down_seen[buf[0]] = 1;
            break;
        case UP: down_seen[buf[0]] = 0; break;
        }
        LeaveCriticalSection(&lk);
    }
}

static void nodelay(SOCKET s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one); }

/* A joiner says hello; returns 0 and fills *h, or -1. */
static int read_hello(SOCKET s, hello_t* h) { return all(s, h, sizeof *h, 1) || h->magic != MAGIC ? -1 : 0; }

/* Host: a player (re)joining a game under way. */
static void admit(SOCKET s) {
    hello_t h;
    if (read_hello(s, &h)) { closesocket(s); return; }
    EnterCriticalSection(&lk);
    int id = -1;
    if (h.want > 0 && h.want < net_n && !up[h.want] && (h.game == game_id || h.game == 0)) id = h.want;
    for (int i = 1; i < net_n && id < 0; i++) if (!up[i]) id = i;
    if (id < 0) { LeaveCriticalSection(&lk); closesocket(s); return; }   /* game full */
    welcome_t w = { (uint8_t)id, (uint8_t)net_n, 1, 0, game_id, epoch, tp_rules };
    if (all(s, &w, sizeof w, 0)) { LeaveCriticalSection(&lk); closesocket(s); return; }
    nodelay(s);
    link_[id] = s; up[id] = 1; listed[id] = 0;
    uint8_t who = (uint8_t)id;
    broadcast(UP, &who, 1);
    down_seen[id] = 0;
    fprintf(stderr, "themepark: network: player %d is back\n", id + 1);
    CloseHandle(CreateThread(NULL, 0, reader, (LPVOID)(intptr_t)id, 0, NULL));
    LeaveCriticalSection(&lk);
}

static SOCKET listener = INVALID_SOCKET;
static DWORD WINAPI acceptor(LPVOID arg) {
    (void)arg;
    for (;;) {
        SOCKET s = accept(listener, NULL, NULL);
        if (s != INVALID_SOCKET) admit(s);
    }
    return 0;
}

static SOCKET dial(void) {
    char host[256], pbuf[16];
    const char* port;
    snprintf(host, sizeof host, "%s", net_join);
    char* c = strrchr(host, ':');
    if (c) { *c = 0; port = c + 1; } else { snprintf(pbuf, sizeof pbuf, "%d", net_port); port = pbuf; }
    struct addrinfo hint = { 0 }, *r;
    hint.ai_family = AF_INET; hint.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hint, &r)) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, r->ai_addr, (int)r->ai_addrlen)) { closesocket(s); s = INVALID_SOCKET; }
    freeaddrinfo(r);
    return s;
}

/* Joiner: hello, welcome. Fills net_me/net_n/game_id/epoch. 0 or -1. */
static int greet(SOCKET s, int want, uint32_t game, int* rejoin) {
    hello_t h = { MAGIC, (uint8_t)want, {0, 0, 0}, game };
    welcome_t w;
    if (all(s, &h, sizeof h, 0) || all(s, &w, sizeof w, 1)) return -1;
    net_me = w.id; net_n = w.n; game_id = w.game; epoch = w.epoch;
    tp_rules = w.rules;             /* every machine plays by the host's */
    *rejoin = w.rejoin;
    return 0;
}

/* Tell the host which checkpoints this machine has (after a rejoin). */
static void send_list(void) {           /* lk held */
    put(0, 0, net_me, LIST, saved[net_me], (uint32_t)nsaved[net_me] * 4);
}

static DWORD WINAPI reconnect(LPVOID arg) {    /* others: the host went away */
    (void)arg;
    for (;;) {
        Sleep(1000);
        SOCKET s = dial();
        if (s == INVALID_SOCKET) continue;
        int rejoin, me = net_me;
        uint32_t game = game_id;
        if (greet(s, me, game, &rejoin) || net_me != me || game_id != game) {
            closesocket(s);
            if (game_id != game) die("the host started a different game");
            continue;
        }
        nodelay(s);
        EnterCriticalSection(&lk);
        link_[0] = s; up[0] = 1; down_seen[0] = 0;
        send_list();
        CloseHandle(CreateThread(NULL, 0, reader, (LPVOID)(intptr_t)0, 0, NULL));
        LeaveCriticalSection(&lk);
        fprintf(stderr, "themepark: network: back with the host\n");
        return 0;
    }
}

/* Make the links. The host waits for every player, then tells each its number. */
static void connect_all(void) {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    InitializeCriticalSection(&lk);
    { const char* e = getenv("TP_NET_CP"); if (e && atoi(e) > 0) checkpoint_every = atoi(e); }
    if (net_host_players) {
        net_n = net_host_players; net_me = 0;
        int lme, ln;
        if (net_resume) {
            if (!load_last(&game_id, &lme, &ln) || lme != 0 || ln != net_n) die("--resume: this machine did not host a game of that size");
            resync = 1;                           /* back to a checkpoint, once everyone is here */
            read_list();
        } else game_id = (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)getpid();
        listener = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof one);
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)net_port); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(listener, (struct sockaddr*)&a, sizeof a) || listen(listener, 4)) die("cannot listen on that port");
        up[0] = 1; listed[0] = 1;
        for (int got = 1; got < net_n; ) {
            fprintf(stderr, "themepark: network: waiting for %d more player%s on port %d\n",
                    net_n - got, net_n - got > 1 ? "s" : "", net_port);
            SOCKET s = accept(listener, NULL, NULL);
            hello_t h;
            if (s == INVALID_SOCKET || read_hello(s, &h)) { if (s != INVALID_SOCKET) closesocket(s); continue; }
            int id = -1;
            if (net_resume && h.game == game_id && h.want > 0 && h.want < net_n && !up[h.want]) id = h.want;
            for (int i = 1; i < net_n && id < 0; i++) if (!up[i]) id = i;
            welcome_t wl = { (uint8_t)id, (uint8_t)net_n, (uint8_t)net_resume, 0, game_id, epoch, tp_rules };
            if (all(s, &wl, sizeof wl, 0)) { closesocket(s); continue; }
            nodelay(s);
            link_[id] = s; up[id] = 1; got++;
        }
        /* A new game: the host hears of every checkpoint as it is made. Resuming:
         * each player sends its list first. */
        for (int i = 1; i < net_n; i++) listed[i] = !net_resume;
        for (int i = 1; i < net_n; i++) CloseHandle(CreateThread(NULL, 0, reader, (LPVOID)(intptr_t)i, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, acceptor, NULL, 0, NULL));
    } else {
        uint32_t lgame = 0; int lme = 0xFF, ln;
        if (!load_last(&lgame, &lme, &ln)) lgame = 0, lme = 0xFF;
        fprintf(stderr, "themepark: network: joining %s\n", net_join);
        SOCKET s;
        for (int tries = 0; (s = dial()) == INVALID_SOCKET; tries++) {   /* the host may still be starting */
            if (tries == 600) die("the host did not answer");
            Sleep(100);
        }
        int rejoin;
        if (greet(s, lme, lgame, &rejoin)) die("the host closed the connection (is the game full?)");
        nodelay(s);
        link_[0] = s; up[0] = 1;
        if (rejoin) { resync = 1; read_list(); EnterCriticalSection(&lk); send_list(); LeaveCriticalSection(&lk); }
        CloseHandle(CreateThread(NULL, 0, reader, (LPVOID)(intptr_t)0, 0, NULL));
    }
    save_last();
    fprintf(stderr, "themepark: network: player %d of %d%s\n", net_me + 1, net_n, resync ? ", rejoining" : "");
}

/* ---- the overrides ---------------------------------------------------------- */

int net_enabled(void) { return net_host_players || net_join[0]; }

/* 0x4393BE: called by the command line's -m. */
void net_start(void (*lifted)(void)) {
    if (!net_enabled()) { fprintf(stderr, "themepark: -m needs --host N or --join ADDR\n"); ret(0xFFFFFFFFu); return; }
    connect_all();
    M8(PLAYERS) = (uint8_t)net_n;   /* a joining machine learns the count from the host */
    lifted();                       /* the game's set-up: its session blocks and buffers */
}

void net_present(void (*lifted)(void)) { (void)lifted; ret(0); }

/* 0x439414(players): the sessions are the links already made. */
void net_connect(void (*lifted)(void)) {
    (void)lifted;
    M16(NET_N) = (uint16_t)net_n;
    M16(NET_ME) = (uint16_t)net_me;
    M8(NET_DONE) = 1;
    for (int i = 0; i < 8; i++) M8(SLOT + i) = i == net_me ? 2 : i < net_n ? 1 : 0;
    ret((uint32_t)net_me);
}

/* Which player a session block belongs to. */
static int player_of(uint32_t off, uint32_t sel) {
    for (int i = 0; i < net_n; i++)
        if (M32(NCBS + 6 * i) == off && M16(NCBS + 6 * i + 4) == (sel & 0xFFFF)) return i;
    die("a send or receive on a session that is not a player's");
    return -1;
}

/* 0x438F65(ncb, buf, len): 0 when sent. */
void net_send(void (*lifted)(void)) {
    (void)lifted;
    int to = player_of(ARG(0), ARG(1));
    uint32_t buf = ARG(2), len = ARG(3);
    EnterCriticalSection(&lk);
    if (!resync) {
        if (len > 6) logx("send to %d: %u bytes", to, len);
        to_player(to, DATA, dos32_mem(buf), len);
    }
    LeaveCriticalSection(&lk);
    ret(0);
}

/* Someone is missing: say who, in the title bar. */
static void waiting_note(void) {
    if (net_me != 0 && down_seen[0]) { note("Lost the host: waiting for it to come back", 0); return; }
    for (int i = 0; i < net_n; i++)
        if (i != net_me && down_seen[i]) { note("Player %d has left: waiting for them to rejoin", i + 1); return; }
}

/* 0x4390DF(ncb, buf, len): the length received. Waits for all of it. */
void net_receive(void (*lifted)(void)) {
    (void)lifted;
    int from = player_of(ARG(0), ARG(1));
    uint32_t buf = ARG(2), len = ARG(3);
    for (int spins = 0;; spins++) {
        EnterCriticalSection(&lk);
        int ok = resync || in[from].w - in[from].r >= len;
        if (resync) memset(dos32_mem(buf), 0, len);           /* blank until the rollback's load */
        else if (ok) { memcpy(dos32_mem(buf), in[from].b + in[from].r, len); in[from].r += len; }
        if (!ok && spins % 64 == 63) waiting_note();
        LeaveCriticalSection(&lk);
        if (ok) break;
        if (dos32_virtual_clock) { if (dos32_quit) exit(0); Sleep(1); }
        else dos32_idle(0.0005);   /* timer, keyboard and mouse still run, as in the game's own wait */
    }
    if (len > 6 && !resync) logx("receive from %d: %u bytes", from, len);
    ret(len);
}

/* Checkpoints and rollbacks: at the start of the dispatcher when the main
 * loop's frame calls it, just before the frame's exchange. That is the one
 * place every machine passes at the same point of the game's traffic, and a
 * restarted one can always get back to (the title's next screen runs it).
 *
 * A state loads only where the host's own stack is as deep as when it was
 * saved. So this is one call, made unconditionally at the top of
 * net_dispatch, and the save or the load is one indirect call inside it: with
 * two call sites gcc put them 16 bytes apart, and nothing would load. */
#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
static NOINLINE void checkpoint(int main_loop) {
    if (!net_n) return;                /* a one-player game: the dispatcher runs every frame */
    int live = M8(NET_DONE) != 0;      /* in step with the others: the sessions are made */
    char p[MAX_PATH], tmp[MAX_PATH];
    EnterCriticalSection(&lk);
    int back = resync && rollback_to != 0xFFFFFFFFu && main_loop;   /* even on the screen after the title */
    uint32_t to = rollback_to;
    LeaveCriticalSection(&lk);
    int save = !back && !resync && live && main_loop && frame && frame % (uint32_t)checkpoint_every == 0;
    if (!back && !save) return;
    cp_path(net_me, back ? to : frame, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    int (*volatile io)(const char*, uint32_t) = back ? dos32_state_load : dos32_state_save;
    int r = io(back ? p : tmp, EXCHANGE);
    if (back) {
        if (r == 0) {
            sound_reset();
            EnterCriticalSection(&lk);
            if (rollback_to == to) { resync = 0; rollback_to = 0xFFFFFFFFu; frame = to; }
            LeaveCriticalSection(&lk);
            note("Back in step from the checkpoint at frame %d", (int)to);
            fprintf(stderr, "themepark: network: back in step from frame %u\n", to);
        } else if (r == -1) die("this machine's checkpoint is missing: the game cannot carry on");
        else { static int k; if (k++ < 3) logx("checkpoint %u does not load here yet (esp %08x)", to, g_esp); }
    }
    else if (save) {
        if (r == 0) {
            remove(p);
            rename(tmp, p);
            EnterCriticalSection(&lk);
            if (nsaved[net_me] == KEEP) { char old[MAX_PATH]; cp_path(net_me, saved[net_me][0], old, sizeof old); remove(old); }
            remember(net_me, frame);
            write_list();
            if (net_me != 0) put(0, 0, net_me, SAVED, &frame, 4);
            LeaveCriticalSection(&lk);
            logx("checkpoint at frame %u", frame);
        }
    }
}

/* 0x4399F2: the frame's exchange. House rules are done here, between steps
 * of the park, and the frames in step are counted. */
void net_exchange(void (*lifted)(void)) {
    int live = M8(NET_DONE) != 0;
    net_actions_apply(live && from_frame_tick && !resync);
    lifted();
    if (!resync && live) frame++;
}

/* 0x41CCF8, the command dispatcher (it calls the exchange first): who called
 * it, and the checkpoints. */
void net_dispatch(void (*lifted)(void)) {
    int was = from_frame_tick;
    from_frame_tick = g_cur_func == FRAME_TICK;   /* an override starts with its caller's */
    checkpoint(from_frame_tick);
    lifted();
    from_frame_tick = was;
}

/* 0x439B20: at exit. */
void net_hangup(void (*lifted)(void)) {
    (void)lifted;
    for (int i = 0; i < 4; i++) if (up[i]) closesocket(link_[i]);
    ret(0);
}

/* ---- house rules: actions between players ------------------------------------ */

/* An action arrives on a reader thread and is carried out on the game's, at
 * the park's frame, where the game is between steps. */
static struct { int from; net_action_t a; } todo[32];
static int ntodo;

void net_action_received(int from, const void* p, uint32_t len) {   /* lk held */
    if (len != sizeof(net_action_t) || ntodo == 32) return;
    todo[ntodo].from = from;
    memcpy(&todo[ntodo].a, p, sizeof(net_action_t));
    ntodo++;
}

void net_actions_apply(int now) {
    if (!now) return;
    EnterCriticalSection(&lk);
    int k = ntodo;
    struct { int from; net_action_t a; } list[32];
    memcpy(list, todo, sizeof list);
    ntodo = 0;
    LeaveCriticalSection(&lk);
    for (int i = 0; i < k; i++) net_action_do(list[i].from, &list[i].a);
    net_rules_frame();
}

void net_send_action(int to, const net_action_t* a) {
    EnterCriticalSection(&lk);
    if (to == net_me) net_action_received(net_me, a, sizeof *a);
    else to_player(to, ACTION, a, sizeof *a);
    LeaveCriticalSection(&lk);
}

int net_player_up(int i) {
    if (net_me == 0) return up[i];
    return i == net_me || (up[0] && !down_seen[i]);
}

/* For the settings menu: the size of the last game this machine hosted, or 0. */
int net_last_hosted(void) {
    int was = net_host_players, me, n;
    uint32_t game;
    net_host_players = 1;                 /* the host's file */
    int ok = load_last(&game, &me, &n);
    net_host_players = was;
    return ok && me == 0 ? n : 0;
}
