/* Modern rules (F10 -> Modern rules, docs/rules.md): changes to how the 1994
 * game behaves, each one off unless the player turns it on, so the game as
 * Bullfrog shipped it is always one click away.
 *
 * Every rule changes the simulation, so in a network game all the machines
 * must agree: the host's rules travel in its welcome (net.c), and the menu
 * cannot change them during the game.
 *
 * The people (guests, staff, thugs) are records of 0xC8 bytes from
 * [0x4C3B10] to [0x4C3B78]. A record:
 *   +4  b  type: 2 guest, 8 staff, 0x1A thug (0 = a free record)
 *   +5  b  flags          +7/+9 b  tile x, y (words +6/+8: x, y in 1/256 tile)
 *   +A  w  sprite frame   +C  b  state       +D  b  state to go back to
 *   +12 b  facing (0-3)   +14 w  steps left in this walk
 *   +1E w  record index of the thug a guard holds
 *   +56/+57 b  tile a walk heads for (x, y)
 *   +59 b  staff: job (0x4C35C0 table: 1 mechanic, 2 handyman, 8 guard, others entertainers)
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "compat.h"
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "dos32.h"
#include "rules.h"

#define M8(a)  (*(uint8_t*)dos32_mem(a))
#define M16(a) (*(uint16_t*)dos32_mem(a))
#define M32(a) (*(uint32_t*)dos32_mem(a))

#define PEOPLE     0x4C3B10   /* first record (unused: index 0 means none) */
#define PEOPLE_END 0x4C3B78
#define REC        0xC8
#define STAFF_TABLE 0x4C35C0  /* 0x36 a job */
#define FRAMES     0x4C3B38   /* sprite frame table */

uint32_t tp_rules;

const rule_t rule_list[] = {
    { RULE_GUARDS, "guards", "Guards go after thugs nearby",
      "A patrolling guard heads for any thug within 12 tiles, not only one straight ahead on the path (#8)." },
    { RULE_STUCK, "unstick", "Stuck guests find their way out",
      "A guest stuck for a few seconds is moved out of the pocket it is fenced into, or along its way to the gate (#17)." },
    { RULE_ADVISOR, "advisor", "The advisor keeps advising",
      "Advice does not fade away over a long game, and the advisor warns of the year end and says what research has found (#16)." },
};
const int rule_count = sizeof rule_list / sizeof rule_list[0];

void rules_load(const char* ini) {
    tp_rules = 0;
    for (int i = 0; i < rule_count; i++)
        if (GetPrivateProfileIntA("rules", rule_list[i].key, 0, ini)) tp_rules |= rule_list[i].bit;
}

void rules_save(const char* ini) {
    for (int i = 0; i < rule_count; i++)
        WritePrivateProfileStringA("rules", rule_list[i].key, tp_rules & rule_list[i].bit ? "1" : "0", ini);
}

static int log_on = -1;
static void rlog(const char* fmt, ...) {
    if (log_on < 0) log_on = getenv("TP_RULESLOG") != NULL;
    if (!log_on) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[rules] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- guards (#8) -------------------------------------------------------- */
/* A patrolling guard (state 0x34, 0x424149) stands where it was put, turning
 * now and then. Every 400 ticks (16 s at 25 fps) it looks once, in the way it
 * faces, one to five tiles along the path, and only a thug standing there sets
 * it walking; it catches a thug on its own or the next tile (0x42436C) and
 * walks it out of the park. So in practice guards stand still while thugs go
 * by. With the rule, about once a second (each guard on its own tick) a guard
 * heads for the nearest thug within GUARD_RANGE tiles, through the game's own
 * walk-to-a-tile (state 0x36), as when it sees one itself. */
#define GUARD_RANGE 12
#define GUARD_EVERY 25        /* ticks between looks */
#define TICKS 0x4C3AF8        /* the simulation's tick count */

static int free_thug(uint32_t p) {   /* a thug a guard may take: as 0x42436C tests */
    uint8_t s = M8(p + 0xC);
    return M8(p + 4) == 0x1A && s != 1 && s != 7 && s != 8 && (M8(p + 5) & 2);
}

void guard_patrol(void (*lifted)(void)) {
    uint32_t g = M32(g_esp + 4), me = (g - M32(PEOPLE)) / REC;
    lifted();
    if (!(tp_rules & RULE_GUARDS) || M8(g + 0xC) != 0x34 || (M32(TICKS) + me) % GUARD_EVERY) return;
    int gx = M8(g + 7), gy = M8(g + 9), best = GUARD_RANGE + 1;
    uint32_t thug = 0;
    for (uint32_t p = M32(PEOPLE) + REC; p < M32(PEOPLE_END); p += REC) {
        if (!free_thug(p)) continue;
        int d = abs(M8(p + 7) - gx) + abs(M8(p + 9) - gy);
        if (d < best) { best = d; thug = p; }
    }
    if (!thug) return;
    /* What 0x42427B does on a sighting. */
    M8(g + 0x56) = M8(thug + 7);
    M8(g + 0x57) = M8(thug + 9);
    M8(g + 0xC) = 0x36;
    M8(g + 0xD) = 0x34;
    M16(g + 0x14) = 0;
    M16(g + 0xA) = M16(M32(FRAMES) + 2 * (M16(STAFF_TABLE + M8(g + 0x59) * 0x36 + 0x26) + M8(g + 0x12)));
    g_eax = 1;
    rlog("guard %u at %d,%d heads for thug %u at %d,%d", me, gx, gy,
         (thug - M32(PEOPLE)) / REC, M8(thug + 7), M8(thug + 9));
}

/* ---- stuck guests (#17) ------------------------------------------------- */
/* A guest whose way out the game cannot find (0x412BA6) goes to state 0x20,
 * "stuck" (0x41276C): every 50 ticks it asks the pathfinder again, and the
 * advisor says some bad planning has got one stuck. Fenced into a pocket, it
 * stays there until the player clears a way. With the rule, after a few failed
 * tries the guest is moved: out of a pocket, to the nearest tile with a way to
 * the gate; on a tile that has a way the game's short search missed, 8 tiles
 * along the shortest way to the gate. Then it walks on (state 6), as when the
 * pathfinder finds a way. The decision uses only the game's own state, so all
 * the machines of a network game make it alike. */
#define TILES      0x4C3B08   /* 64x64 tiles of 4 bytes: +2 tile type */
#define TILE_FLAGS 0x4A44D9   /* per type, 4 bytes: +0 w flags */
#define BLOCKS_WALK 0x29C1    /* flags a guest walking out may not cross (0x4127AD) */
#define ENTRANCE   0x4C4706   /* w: the gate's tile, player record +0x2CA */
#define STUCK_TRIES 4         /* tries (50 ticks each) before a move */
#define STUCK_HOP  8          /* tiles along the way, when the tile has one */

static uint32_t guest_call(uint32_t addr, int n, const uint32_t* a);

static int walkable(int t) {
    return !(M16(TILE_FLAGS + 4 * M8(M32(TILES) + 4 * (uint32_t)t + 2)) & BLOCKS_WALK);
}

/* Steps from the gate to every walkable tile (-1: no way). */
static void way_out(int16_t* dist) {
    static int16_t q[4096];
    int head = 0, tail = 0, gate = M16(ENTRANCE + M16(0x4C665C) * 0x6C2);
    for (int t = 0; t < 4096; t++) dist[t] = -1;
    if (gate < 0 || gate >= 4096) return;
    dist[gate] = 0; q[tail++] = (int16_t)gate;
    while (head < tail) {
        int t = q[head++], x = t % 64, y = t / 64;
        const int nb[4][2] = { { x + 1, y }, { x - 1, y }, { x, y + 1 }, { x, y - 1 } };
        for (int k = 0; k < 4; k++) {
            int nx = nb[k][0], ny = nb[k][1], n = ny * 64 + nx;
            if (nx < 0 || nx > 63 || ny < 0 || ny > 63 || dist[n] >= 0 || !walkable(n)) continue;
            dist[n] = (int16_t)(dist[t] + 1);
            q[tail++] = (int16_t)n;
        }
    }
}

void guest_stuck(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4), me = (p - M32(PEOPLE)) / REC;
    int tries = (int16_t)M16(p + 0x14) <= 1;      /* this pass asks the pathfinder */
    lifted();
    if (!(tp_rules & RULE_STUCK) || !tries || M8(p + 0xC) != 0x20 || (M32(TICKS) / 50 + me) % STUCK_TRIES) return;
    static int16_t dist[4096];
    way_out(dist);
    int x = M8(p + 7), y = M8(p + 9), here = y * 64 + x, to = -1;
    if (dist[here] > 0) {                          /* a way the search missed: along it */
        to = here;
        for (int s = 0; s < STUCK_HOP && dist[to] > 0; s++) {
            const int nb[4] = { to + 1, to - 1, to + 64, to - 64 };
            for (int k = 0; k < 4; k++) {
                int n = nb[k];
                if (n < 0 || n >= 4096 || (k < 2 && n / 64 != to / 64) || dist[n] != dist[to] - 1) continue;
                to = n; break;
            }
        }
    } else {                                       /* a pocket: the nearest tile with a way */
        int best = 1 << 30;
        for (int t = 0; t < 4096; t++) {
            if (dist[t] < 0) continue;
            int d = abs(t % 64 - x) + abs(t / 64 - y);
            if (d < best) { best = d; to = t; }
        }
    }
    if (to < 0 || to == here) return;
    uint32_t a[3] = { p, (uint32_t)((to % 64) << 8 | 0x80), (uint32_t)((to / 64) << 8 | 0x80) };
    guest_call(0x408A44, 3, a);                    /* move: off the old tile's list, onto the new */
    /* What 0x4127DD does when the pathfinder finds a way. */
    M8(p + 0xC) = 6;
    M16(p + 0x14) = 0;
    M16(p + 0xA) = M16(M32(FRAMES) + 2 * (M16(0x4A49F4 + M8(p + 0x59) * 0x24) + M8(p + 0x12)));
    rlog("stuck guest %u at %d,%d moved to %d,%d (%s)", me, x, y, to % 64, to / 64,
         dist[here] > 0 ? "along the way out" : "out of a pocket");
}

/* ---- the advisor (#16) -------------------------------------------------- */
/* The advisor (0x44CBDB, every frame in a park) walks a table of 34 pieces of
 * advice at 0x4AA144, 12 bytes each: +0 w message (text 665 + n), +3 w how
 * long to wait between showings, +5 w how long to show it, +7 b times shown,
 * +8 d when last shown (the calendar, 0x4C3AFC: 1000 a month). The wait grows
 * with the times shown, which nothing resets, not even a new game: an hour in,
 * most advice has gone quiet. With the rule, the count stops at ADVICE_CAP, so
 * advice comes back about monthly, and the advisor says two new things when it
 * has nothing else to say: that the year ends next month (mid November), and
 * what the researchers have just finished. The game's own message area shows
 * them (0x44E2C5, given the text). This is all on the screen of this machine
 * only; nothing the simulation reads changes. */
#define ADVICE      0x4AA144
#define ADVICE_CAP  4
#define ADVICE_NOW  0x4C661C  /* w: the entry being shown, -1 none */
#define CALENDAR    0x4C3AFC  /* d: months * 1000 */
#define ADVICE_TEXT 0x4BE7D0  /* the advisor's own text buffer */
#define SAY_FOR     350       /* calendar units a message stays (about 5 s) */
#define PRICE_TOTAL 1600      /* what the shop-price advice may see of park +0xB6 */

static struct { char text[160]; uint32_t by; } say_queue[8];   /* by: too late after this */
static int say_n;
static uint32_t say_until;

static void say(const char* text, uint32_t by) {
    if (say_n < 8) { snprintf(say_queue[say_n].text, sizeof say_queue[0].text, "%s", text); say_queue[say_n++].by = by; }
    rlog("advisor: %s", text);
}

static void advisor_news(void) {
    uint32_t now = M32(CALENDAR);
    static uint32_t warned_year = 0xFFFFFFFF;
    if (now % 12000 >= 10500 && now % 12000 < 11000 && warned_year != now / 12000) {
        warned_year = now / 12000;
        say("THE YEAR ENDS NEXT MONTH. YOUR ACCOUNTS AND SHARE VALUE WILL BE DRAWN UP AT THE END OF DECEMBER.",
            now - now % 12000 + 11300);   /* not after early December */
    }
    /* Research: an item is new when it becomes available and not yet looked at
     * (flag bits 1 and 2, research_notes). Each is told once a session. */
    static const struct { uint32_t base, stride, flag; int count; const char* what; } t[] = {
        { 0x4BCED0, 0xC8, 0xC5, 29, "RIDE" }, { 0x4C3C62, 0x6B, 0x66, 17, "SHOP" } };
    static uint8_t told[2][32], primed;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < t[k].count; i++) {
            uint32_t r = t[k].base + (uint32_t)i * t[k].stride;
            int fresh = (M8(r + t[k].flag) & 3) == 3;
            if (fresh && !told[k][i] && primed) {
                char line[160];
                snprintf(line, sizeof line, "YOUR RESEARCHERS HAVE COME UP WITH A NEW %s: THE %.20s. IT IS IN THE BUILD MENU NOW.",
                         t[k].what, (const char*)dos32_mem(r));
                say(line, now + 3000);
            }
            told[k][i] = (uint8_t)fresh;
        }
    primed = 1;               /* what was new before this session is not news */
}

void advisor(void (*lifted)(void)) {
    /* As 0x44CBDB starts: the advisor turned off (0x4C778D), or busy with
     * something of its own (0x4AA0F8 >= 0). */
    if (!(tp_rules & RULE_ADVISOR) || !M8(0x4C778D) || (int8_t)M8(0x4AA0F8) >= 0) { lifted(); return; }
    for (uint32_t e = ADVICE; M16(e); e += 12)
        if (M8(e + 7) > ADVICE_CAP) M8(e + 7) = ADVICE_CAP;
    advisor_news();
    uint32_t now = M32(CALENDAR);
    if (say_until && now < say_until && now + SAY_FOR + 1 > say_until) {   /* ours, on screen */
        uint32_t a = ADVICE_TEXT;
        guest_call(0x44E2C5, 1, &a);
        return;
    }
    say_until = 0;
    /* Shop prices (0x44CFA4): the "right" price is the base price times
     * (100 + park +6 + (shop's two bytes * 8 + park +0xB6 / 16) / 2) / 100, and
     * +0xB6 is a running total that only grows (about 200 a second), so an hour
     * in it calls for prices twenty times the base. While the advisor thinks it
     * sees at most PRICE_TOTAL (about twice the base at most); then the real
     * value is put back. */
    uint32_t park = 0x4BF1E0 + (uint32_t)M16(M32(0x4C3B00) + M8(M32(0x4C3814) + 0x40) * 0x222u + 2) * 0x250u;
    uint32_t total = M32(park + 0xB6);
    if (total > PRICE_TOTAL) M32(park + 0xB6) = PRICE_TOTAL;
    while (say_n && say_queue[0].by < now) memmove(say_queue, say_queue + 1, sizeof say_queue[0] * (size_t)--say_n);
    if (say_n && (int16_t)M16(ADVICE_NOW) < 0) {     /* the advisor has nothing to say: ours */
        snprintf((char*)dos32_mem(ADVICE_TEXT), 160, "%s", say_queue[0].text);
        memmove(say_queue, say_queue + 1, sizeof say_queue[0] * (size_t)--say_n);
        say_until = now + SAY_FOR;
        uint32_t a = ADVICE_TEXT;
        guest_call(0x44E2C5, 1, &a);
    }
    else lifted();
    M32(park + 0xB6) = total;
    static int16_t shown = -1;
    if ((int16_t)M16(ADVICE_NOW) != shown && (shown = (int16_t)M16(ADVICE_NOW)) >= 0)
        rlog("advisor (game): %.150s", (const char*)dos32_mem(ADVICE_TEXT));
}

/* ---- tests -------------------------------------------------------------- */
/* --call T:ADDR[:A1[:A2[:A3]]] (main.c): a game function, called between
 * frames as its own callers would. For tests: hire staff (0x422232 TILE JOB),
 * send a thug (0x43C592 0). TP_PEOPLELOG=N: every N frames, each guard and
 * thug, its state and tile. */
static struct { uint32_t addr, n, a[3]; } calls[16];
static int ncalls;

void rules_queue_call(uint32_t addr, int n, const uint32_t* a) {
    if (ncalls >= 16) return;
    calls[ncalls].addr = addr; calls[ncalls].n = (uint32_t)n;
    memcpy(calls[ncalls].a, a, sizeof calls[ncalls].a);
    ncalls++;
}

static recomp_func_t lookup(uint32_t addr) {
    for (uint32_t i = 0; i < recomp_dispatch_count; i++)
        if (recomp_dispatch_table[i].address == addr) return recomp_dispatch_table[i].func;
    return NULL;
}

static uint32_t guest_call(uint32_t addr, int n, const uint32_t* a) {
    recomp_func_t f = lookup(addr);
    if (!f) { fprintf(stderr, "[rules] no function at %08x\n", addr); return 0; }
    uint32_t r[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp }, cur = g_cur_func, ret;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; M32(g_esp) = a[i]; }
    g_esp -= 4; M32(g_esp) = RECOMP_RETADDR;   /* its ret pops this */
    f();
    g_esp += 4 * (uint32_t)n;
    ret = g_eax;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esi = r[4]; g_edi = r[5]; g_ebp = r[6];
    g_cur_func = cur;
    return ret;
}

void rules_frame(void) {   /* game.c, between frames on the game thread */
    for (int i = 0; i < ncalls; i++) {
        uint32_t r = guest_call(calls[i].addr, (int)calls[i].n, calls[i].a);
        fprintf(stderr, "[rules] call %08x -> %08x\n", calls[i].addr, r);
    }
    ncalls = 0;
    static int every = -1, frame;
    if (every < 0) every = getenv("TP_PEOPLELOG") ? atoi(getenv("TP_PEOPLELOG")) : 0;
    if (every <= 0 || ++frame % every) return;
    for (uint32_t p = M32(PEOPLE) + REC; p < M32(PEOPLE_END); p += REC) {
        int t = M8(p + 4);
        if (t == 0x1A || (t == 8 && M8(p + 0x59) == 8))
            fprintf(stderr, "[people] f%d %s %u state %02x/%02x at %d,%d flags %02x hold %u\n", frame,
                    t == 0x1A ? "thug" : "guard", (p - M32(PEOPLE)) / REC, M8(p + 0xC), M8(p + 0xD),
                    M8(p + 7), M8(p + 9), M8(p + 5), M16(p + 0x1E));
    }
}
