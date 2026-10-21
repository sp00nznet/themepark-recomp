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
