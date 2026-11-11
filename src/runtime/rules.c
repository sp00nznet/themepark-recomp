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
#define TILES      0x4C3B08   /* 64x64 tiles of 4 bytes: +0 w first person on it, +2 b type */
#define TILE_FLAGS 0x4A44D9   /* per tile type, 4 bytes: +0 w flags */
#define TICKS      0x4C3AF8   /* the simulation's tick count */
extern int32_t tp_vh;          /* game.c: the park view's height in 640x480 */

static uint32_t guest_call(uint32_t addr, int n, const uint32_t* a);

uint32_t tp_rules;

const rule_t rule_list[] = {
    { RULE_GUARDS, "guards", "Guards go after thugs nearby",
      "A patrolling guard heads for any thug within 12 tiles, not only one straight ahead on the path, "
      "and a thug whose guard was moved or fired is let go instead of pacing round her for ever (#8, #29)." },
    { RULE_STUCK, "unstick", "Stuck guests find their way out",
      "A guest stuck for a few seconds is moved out of the pocket it is fenced into, or along its way to the gate (#17)." },
    { RULE_ADVISOR, "advisor", "The advisor keeps advising",
      "Advice does not fade away over a long game, and the advisor warns of the year end and says what research has found (#16)." },
    { RULE_ENTERTAIN, "entertainers", "Entertainers entertain",
      "No flashes between performances or flicker when fully trained, the panel counts the people entertained and made happy, "
      "and staff keep their training speed when moved (#29)." },
    { RULE_HANDYMEN, "handymen", "Handymen stay on the job",
      "Back from a strike or off the paths, a handyman goes to the nearest path (and his patrol) instead of mowing far away, "
      "and patrols take the shortest way between waypoints, however many (#7)." },
    { RULE_BUILD, "building", "More room to build",
      "Hedges, walls and fences go under raised track and where people stand, lamp posts and fountains go on paths, "
      "the camera scrolls 3 rows past the back of the park (and the cursor keys stop at its other edges), and track still builds in a full park "
      "(the guests leave room for 150 more pieces) (#23, #21, #15)." },
    { RULE_LIGHTS, "lights", "Lamps light up in the dark",
      "When rain and storms darken the park, lamp posts glow; the darker, the brighter. Only the picture changes (#18)." },
    { RULE_ESC, "esc", "Esc closes windows",
      "Esc closes the open window, as the tick does: status, finance and the rest, not only the staff window (#43)." },
    { RULE_DEAL, "deal", "Handshake sounds wait for the deal",
      "In a negotiation your hand moves twice as fast, the heartbeat keeps going and the cheer waits until the hands "
      "have shaken, not just come close (#49)." },
    { RULE_ENGINEERS, "engineers", "Engineers reach their rides",
      "Engineers take the shortest way to a broken ride and round it, past hedges, walls and fences, put the hoardings up "
      "and down twice as fast, and after a repair walk to the nearest free patch of grass (#40)." },
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

/* A caught thug (state 8, 0x43D88D) follows the guard in its +0x61 until that
 * guard reaches the gate (state 0x38) and throws it out; nothing else lets it
 * go, and guards do not catch a thug already caught. So a guard picked up and
 * dropped elsewhere leaves her thug pacing round her for good, and a guard
 * fired or quitting leaves it frozen (her record is cleared: it follows a
 * speed of 0). With the rule, a thug whose guard is no longer walking it out
 * is let go (state 3, wandering), and a guard can catch it again (#29). */
static int escorting(uint32_t thug, uint32_t g) {
    if (M8(g + 4) != 8 || M8(g + 0x59) != 8) return 0;
    if (!(M8(g + 0xC) == 0x38 || (M8(g + 0xC) == 0x36 && M8(g + 0xD) == 0x38))) return 0;
    uint32_t want = (thug - M32(PEOPLE)) / REC;
    uint16_t i = M16(g + 0x1E);
    for (int hops = 0; i && hops < 16; hops++) {   /* the guard's chain of thugs, +0x1E each */
        if (i == want) return 1;
        i = M16(M32(PEOPLE) + i * REC + 0x1E);
    }
    return 0;
}

void thug_caught(void (*lifted)(void)) {
    uint32_t t = M32(g_esp + 4);
    if (tp_rules & RULE_GUARDS) {
        uint16_t gi = M16(t + 0x61);
        uint32_t g = M32(PEOPLE) + gi * REC;
        if (!gi || g >= M32(PEOPLE_END) || !escorting(t, g)) {
            if (gi && g < M32(PEOPLE_END) && M8(g + 4) == 8 && M8(g + 0x59) == 8 && M8(g + 0xC) == 0x34)
                M16(g + 0x1E) = 0;                     /* a guard back on patrol holds no one */
            M8(t + 0xC) = 3;
            M16(t + 0x14) = 0;
            M8(t + 0x13) = 0x10;
            M16(t + 0x1E) = 0;
            rlog("thug %u let go: its guard %u is not walking it out", (t - M32(PEOPLE)) / REC, gi);
            g_eax = 0;
            g_esp += 4;                                /* return as its ret would */
            return;
        }
    }
    lifted();
}

/* ---- entertainers (#29) ------------------------------------------------- */
/* An entertainer performs in place (state 2, 0x4226DA), walks (4, 0x422A06),
 * or performs for one guest (6, 0x422BCF): every 16 ticks it cheers up the
 * first guest on its target tile (+0x4D) no happier than the park's
 * threshold. Three faults:
 *  - when that guest has gone, it drops to state 2 for one tick (facing forced
 *    round, a frame of another animation) before the next tick's look for
 *    guests (0x422E81) sends it back to 6: a flash every 16 ticks;
 *  - its PEOPLE ENTERTAINED count (+0x5A) misses most guests (it counts only
 *    a guest in another column of tiles), and PEOPLE MADE HAPPY (+0x42) is
 *    never written at all, so the panel shows 0;
 *  - walking, every tile restarts the walk animation; fully trained (speed
 *    0x80, a tile every 2 ticks) it never gets past the first frame and the
 *    sprite flickers between facings.
 * And picking staff up and dropping them (0x4248ED) sets their speed back to
 * untrained until the next training step.
 * With the rule: the look for guests happens at once instead of a tick later,
 * each guest cheered counts, a guest cheered past the threshold counts as made
 * happy, a walk keeps its animation going along a straight line, and a dropped
 * member of staff keeps the speed its training gave. */
#define THRESHOLD() ((int16_t)M16(0x4BF1E0 + M8(0x4C447D + M16(0x4C665C) * 0x6C2u) * 0x250u))
#define JOB(p, off) M16(STAFF_TABLE + M8((p) + 0x59) * 0x36u + (off))

static uint32_t first_on_tile(int tile, int16_t thr) {   /* the guest 0x422BCF serves */
    if (tile < 0 || tile >= 4096) return 0;
    uint16_t i = M16(M32(TILES) + 4u * (uint32_t)tile);
    for (int hops = 0; i && hops < 64; hops++) {
        uint32_t q = M32(PEOPLE) + i * REC;
        if (M8(q + 4) == 2 && (int16_t)M16(q + 0x54) <= thr) return q;
        i = M16(q + 2);
    }
    return 0;
}

void ent_perform(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4);
    if (!(tp_rules & RULE_ENTERTAIN)) { lifted(); return; }
    int16_t thr = THRESHOLD();
    int serving = (int16_t)M16(p + 0x14) <= 1;
    uint32_t guest = serving ? first_on_tile(M16(p + 0x4D), thr) : 0;
    uint8_t counted = M8(p + 0x5A);
    lifted();
    if (counted == 255) M8(p + 0x5A) = 255;                         /* its own count wraps round */
    if (M8(p + 0xC) == 6 && M16(p + 0x14) == 0x10 && guest) {      /* it cheered `guest` up */
        if (M8(p + 0x5A) == counted && counted < 255) M8(p + 0x5A)++;
        if ((int16_t)M16(guest + 0x54) > thr) M16(p + 0x42)++;
    } else if (serving && M8(p + 0xC) == 2) {                       /* nobody left: look now */
        uint32_t r = guest_call(0x422E81, 1, &p);
        if ((int16_t)r >= 0 && (int16_t)r < 4) {
            M8(p + 0x12) = (uint8_t)r;
            M16(p + 0xA) = M16(M32(FRAMES) + 2 * (JOB(p, 0x28) + (uint32_t)r));
        }
    }
}

void ent_walk(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4);
    if (!(tp_rules & RULE_ENTERTAIN)) { lifted(); return; }
    uint16_t frame = M16(p + 0xA);
    uint8_t facing = M8(p + 0x12);
    lifted();
    if (M8(p + 0xC) == 4 && M8(p + 0x12) == facing && M16(p + 0xA) != frame &&
        M16(p + 0xA) == M16(M32(FRAMES) + 2 * (JOB(p, 0x26) + facing)))
        M16(p + 0xA) = frame;                     /* a new tile the same way: the animation goes on */
}

void staff_drop(void (*lifted)(void)) {           /* 0x4248ED(tile, record index) */
    uint32_t i = M32(g_esp + 8) & 0xFFFF, p = M32(PEOPLE) + i * REC;
    lifted();
    if ((tp_rules & RULE_ENTERTAIN) && i && p < M32(PEOPLE_END) && M8(p + 4) == 8)
        M8(p + 0x13) = M8(STAFF_TABLE + M8(p + 0x59) * 0x36u + 0x31);   /* its trained speed */
}

/* ---- handymen (#7) ------------------------------------------------------ */
/* A handyman's states (job 2): 0x16 decide by the tile (0x4517E0: on a path
 * wander it, 0x18; on grass mow it, 0x1E; anywhere else walk to the lawn at
 * 32,16 and mow that), 0x18 wander and sweep, 0x1A find the nearest path,
 * 0x30 patrol: up to 8 waypoints (+0x9C, words; +0xAC the next), each leg by
 * 0x453E5D, a greedy step and a 50-step wall-follow. Faults:
 *  - after a strike (picketing at the gate, 0x3A..0x3E) he walks back and
 *    takes up 0x16 where the walk ends: often grass, so he mows, patrol or not;
 *  - off the paths on anything but grass he goes to mow 32,16, far away;
 *  - a patrol leg the wall-follow cannot do is skipped, and setting a patrol
 *    leaves the preview walk's wall-follow state behind (+5 bits 0x30), so the
 *    patrol wanders off: with more than 4 waypoints, most of the time.
 * With the rule: back from a strike, or off the paths, he goes to the nearest
 * path (and from there his patrol, if he has one); a handyman with a patrol
 * does not mow his way out of it; and each patrol leg takes the shortest way
 * along the paths. A handyman dropped on a lawn with no patrol still mows it. */
#define HANDY_PATH 0x1A       /* tile flags a handyman walks (0x4517E0) */

static uint16_t tflags(int t) { return M16(TILE_FLAGS + 4 * M8(M32(TILES) + 4 * (uint32_t)t + 2)); }
static int tile_of(uint32_t p) { return M8(p + 9) * 64 + M8(p + 7); }
static int handyman(uint32_t p) { return M8(p + 4) == 8 && M8(p + 0x59) == 2; }

void strike_return(void (*lifted)(void)) {   /* 0x42542D: state 0x3E, back from the picket line */
    uint32_t p = M32(g_esp + 4);
    lifted();
    if (!(tp_rules & RULE_HANDYMEN) || !handyman(p) || M8(p + 0xC) == 0x3E) return;   /* still walking */
    M8(p + 5) &= (uint8_t)~0x30;                  /* the walk's wall-follow state */
    if (tflags(tile_of(p)) & HANDY_PATH) return;  /* on a path: wander, or patrol */
    if (tile_of(p) == M16(p + 0x4D) && M16(p + 0x9C) == 0xFFFF) return;   /* back at his lawn: mow on */
    M8(p + 0xC) = 0x1A;                           /* to the nearest path */
    M8(p + 0xD) = 0x18;
    rlog("handyman %u back from the strike, off the paths at %d,%d: to the nearest path",
         (p - M32(PEOPLE)) / REC, M8(p + 7), M8(p + 9));
}

void handyman_decide(void (*lifted)(void)) {  /* 0x4517E0: state 0x16 */
    uint32_t p = M32(g_esp + 4);
    if ((tp_rules & RULE_HANDYMEN) && handyman(p)) {
        uint16_t f = tflags(tile_of(p));
        if (!(f & HANDY_PATH) && (f != 0 || M16(p + 0x9C) != 0xFFFF)) {   /* not a path; not his lawn */
            M8(p + 0xC) = 0x1A;
            M8(p + 0xD) = 0x18;
            M8(p + 0x13) = M8(STAFF_TABLE + 2 * 0x36 + 0x31);   /* walking speed: a handyman's */
            M16(p + 0x14) = 0;
            g_eax = (g_eax & ~0xFFu) | 1;         /* as the original returns: al = 1 */
            g_esp += 4;                           /* and its ret */
            return;
        }
    }
    lifted();
}

/* 0x453E5D(p, depth): the next step of a patrol leg toward +0x56/+0x57: a
 * direction 0-3 (tile offsets -64, +1, +64, -1, table 0x4A4920), 4 there, -2
 * no way. With the rule: the first step of the shortest way along the paths,
 * the way it faces first when two are as short (so it is the same on every
 * machine). */
void patrol_step(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4);
    if (!(tp_rules & RULE_HANDYMEN)) { lifted(); return; }
    static int16_t dist[4096];
    static uint16_t q[4096];
    static const int off[4] = { -64, 1, 64, -1 };
    int cur = tile_of(p), tgt = M8(p + 0x57) * 64 + M8(p + 0x56), best = -2, bd = 0x7FFF;
    M8(p + 5) &= (uint8_t)~0x30;
    if (cur == tgt) best = 4;
    else if (tgt >= 0 && tgt < 4096 && (tflags(tgt) & HANDY_PATH)) {
        for (int i = 0; i < 4096; i++) dist[i] = -1;
        int h = 0, n = 0;
        dist[tgt] = 0; q[n++] = (uint16_t)tgt;
        while (h < n) {
            int t = q[h++];
            for (int k = 0; k < 4; k++) {
                int u = t + off[k];
                if (u < 0 || u >= 4096 || ((k & 1) && u / 64 != t / 64) || dist[u] >= 0 || !(tflags(u) & HANDY_PATH)) continue;
                dist[u] = (int16_t)(dist[t] + 1);
                q[n++] = (uint16_t)u;
            }
        }
        for (int j = 0; j < 4; j++) {
            int k = (M8(p + 0x12) + j) & 3, u = cur + off[k];
            if (u < 0 || u >= 4096 || ((k & 1) && u / 64 != cur / 64)) continue;
            if (dist[u] >= 0 && dist[u] < bd) { bd = dist[u]; best = k; }
        }
    }
    g_eax = (uint32_t)best;
    g_esp += 4;                                   /* its ret */
}

/* ---- engineers (#40) ---------------------------------------------------- */
/* An engineer's states (job 1): 8 lunch, and every 200 ticks look for the
 * nearest ride (marker records of type 6 or 0x1C, +0x16 the ride) worn past
 * 0x27 with nobody on it; 0x12 the call; 0xA walk to +0x56/+0x57, then the
 * state in +0xD; 0xC close the ride and put the hoarding up, a walk (+0x20
 * the point, from the ride's table) to each point round it; 0xE repair; 0x10
 * take the hoarding down, the same round; 0x14 look for lunch: walk to a
 * random tile within 8 (0x423E3F), until on grass with no staff. Every walk
 * is a step at a time by 0x4249EB, a greedy step and a wall-follow, so a
 * hedge or fence in the way sends him the long way round, or, when he has no
 * way at all, makes him give up: walking to the ride he gives up the call and
 * has lunch, then takes the same call again, for ever; on the round, he
 * leaves the ride closed with half its hoarding up, and no one comes for it
 * (0x423133 skips a closed ride). He has lunch on grass, where gardens get
 * hedged in round him. The lunch walk tries random tiles until one is free
 * grass.
 * With the rule: an engineer's walk takes the shortest way (along any tile
 * the game lets him walk); with no way from where he is (hedged in), he is
 * put on the nearest tile that has one, as stuck guests are; the hoarding
 * walks go twice as fast; and lunch is the nearest free grass he can walk to. */
static int engineer(uint32_t p) { return M8(p + 4) == 8 && M8(p + 0x59) == 1; }
static int hoarding_leg(uint32_t p) {   /* walking round the ride, not to it */
    int ret = M8(p + 0xD);
    return M8(p + 0xC) == 0xA && (ret == 0xC || ret == 0x10) && M32(p + 0x20) > 0;
}

static int eng_walks(int t, uint16_t mask) {   /* as 0x4249EB: flags in the mask block, but bit 2 lets through */
    uint16_t f = tflags(t);
    return !(f & mask) || (f & 2);
}

/* Steps to tgt from every tile (-1: no way). */
static void eng_bfs(int16_t* dist, int tgt, uint16_t mask) {
    static uint16_t q[4096];
    static const int off[4] = { -64, 1, 64, -1 };
    int h = 0, n = 0;
    for (int i = 0; i < 4096; i++) dist[i] = -1;
    dist[tgt] = 0; q[n++] = (uint16_t)tgt;
    while (h < n) {
        int t = q[h++];
        for (int k = 0; k < 4; k++) {
            int u = t + off[k];
            if (u < 0 || u >= 4096 || ((k & 1) && u / 64 != t / 64) || dist[u] >= 0 || !eng_walks(u, mask)) continue;
            dist[u] = (int16_t)(dist[t] + 1);
            q[n++] = (uint16_t)u;
        }
    }
}

/* 0x4249EB(p, depth, mask): the next step toward +0x56/+0x57: 0-3 (tile
 * offsets -64, +1, +64, -1, table 0x4A4920), 4 there, negative no way. */
void walk_step(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4);
    uint16_t mask = (uint16_t)M32(g_esp + 12);
    int st = M8(p + 0xC);
    if (!(tp_rules & RULE_ENGINEERS) || !engineer(p) || (st != 0xA && st != 0x14)) { lifted(); return; }
    static int16_t dist[4096];
    static const int off[4] = { -64, 1, 64, -1 };
    int cur = tile_of(p), tx = M8(p + 0x56), ty = M8(p + 0x57), best = -1, bd = 0x7FFF;
    M8(p + 5) &= (uint8_t)~0x30;                  /* the wall-follow's state */
    if (tx > 63 || ty > 63) { g_eax = (uint32_t)-1; g_esp += 4; return; }   /* 0x423E3F's random tile can be off the map */
    int tgt = ty * 64 + tx;
    eng_bfs(dist, tgt, mask);
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        if (cur == tgt) { best = 4; break; }
        for (int j = 0; j < 4; j++) {             /* the way it faces first, when two are as short */
            int k = (M8(p + 0x12) + j) & 3, u = cur + off[k];
            if (u < 0 || u >= 4096 || ((k & 1) && u / 64 != cur / 64)) continue;
            if (dist[u] >= 0 && dist[u] < bd) { bd = dist[u]; best = k; }
        }
        if (best >= 0 || pass || st != 0xA) break;
        /* No way from here (hedged in): the nearest tile with one, as stuck guests. */
        int to = tgt, td = 0x7FFF;
        for (int u = 0; u < 4096; u++) {
            int dd = abs(u % 64 - cur % 64) + abs(u / 64 - cur / 64);
            if (dist[u] >= 0 && dd < td) { td = dd; to = u; }
        }
        uint32_t a[3] = { p, (uint32_t)((to % 64) << 8 | 0x80), (uint32_t)((to / 64) << 8 | 0x80) };
        guest_call(0x408A44, 3, a);
        rlog("engineer %u had no way to %d,%d from %d,%d: put on %d,%d", (p - M32(PEOPLE)) / REC, tx, ty,
             cur % 64, cur / 64, to % 64, to / 64);
        cur = to;
    }
    g_eax = (uint32_t)best;
    g_esp += 4;                                   /* its ret */
}

/* 0x423482: state 0xA, a walk. The hoarding's walks twice a tick. */
void eng_walk(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4), ra = M32(g_esp);
    int twice = (tp_rules & RULE_ENGINEERS) && engineer(p) && hoarding_leg(p);
    lifted();
    if (twice && hoarding_leg(p)) { g_esp -= 4; M32(g_esp) = ra; lifted(); }
}

static int free_grass(uint32_t p, int t) {     /* what ends the lunch walk (0x423F14) */
    if (tflags(t) != 0) return 0;
    for (int16_t i = (int16_t)M16(M32(TILES) + 4 * (uint32_t)t); i > 0; i = (int16_t)M16(M32(PEOPLE) + (uint32_t)i * REC + 2)) {
        uint32_t q = M32(PEOPLE) + (uint32_t)i * REC;
        if (M8(q + 4) == 8 && q != p) return 0;
    }
    return 1;
}

/* 0x423E3F(p): the work is done; off to lunch at a random tile. */
void eng_lunch(void (*lifted)(void)) {
    uint32_t p = M32(g_esp + 4);
    lifted();
    if (!(tp_rules & RULE_ENGINEERS) || !engineer(p) || M8(p + 0xC) != 0x14) return;
    static int16_t dist[4096];
    int best = -1;
    eng_bfs(dist, tile_of(p), 0x39C1);            /* from him: the mask of the lunch walk */
    for (int t = 0; t < 4096; t++)                /* nearest; the lowest tile when two are as near */
        if (dist[t] >= 0 && (best < 0 || dist[t] < dist[best]) && free_grass(p, t)) best = t;
    if (best < 0) return;                         /* none: as the game, at random */
    M8(p + 0x56) = (uint8_t)(best % 64);
    M8(p + 0x57) = (uint8_t)(best / 64);
}

/* ---- building (#23, #21, #15) ------------------------------------------- */
/* A tile: +0 w the first of the people-array records on it (chained by +2;
 * the chain ends in 0, or in a tree or lamp: -1..-11), +2 b ground type, +3 b
 * bit 8 locked. Raised track (coasters, big dipper, water splash, monorail)
 * is records of type 0x12 in the chain over ground type 0x68 (flags 0x400).
 *  - Fences (0x4187D8(base, tile, commit), also the pointer's preview) need
 *    ground flags 0 and nothing on the tile, so never go under track, though
 *    paths and queues do (#23).
 *  - A lamp (0x430DA6(tile), the selected feature at 0x4C6646) needs ground
 *    flags 0: never on a path, as in the prototypes (#21).
 *  - Track pieces are records in the same 1199 as the guests, and the guests
 *    may take all but 100 (0x41845A; the second spawner 0x4564F1 all of
 *    them). In a busy park a track piece shows it fits and then the click does
 *    nothing: there is no record for it (#15).
 * With the rule: a fence goes under raised track (the game draws it, though
 * the track's pillars are not drawn on it); a lamp goes on a plain path, and
 * guests walk past it; and the guests leave BUILD_RESERVE more records for
 * building. */
#define BUILD_RESERVE 150
#define FEATURE_PICKED 0x4C6646   /* w: the feature the player is placing */
#define LAMP 2

static int16_t chain_tail(int tile, int only_track) {   /* what ends the chain; -32768 if a record isn't track */
    int16_t i = (int16_t)M16(M32(TILES) + 4u * (uint32_t)tile);
    for (int hops = 0; i > 0 && hops < 256; hops++) {
        uint32_t q = M32(PEOPLE) + (uint32_t)i * REC;
        if (only_track && M8(q + 4) != 0x12) return -32768;
        i = (int16_t)M16(q + 2);
    }
    return i;
}

static int is_person(uint8_t ty) { return ty == 2 || ty == 8 || ty == 0x1A; }   /* guest, staff, thug */

/* Whoever stands on tile t steps off to the nearest tile that is path (or,
 * with grass, bare grass) and that skip(u) allows: 0 if there is none. */
static int step_off(int t, int grass, int (*skip)(int u, int c), int c) {
    for (int guard = 0; guard < 64; guard++) {
        int16_t i = (int16_t)M16(M32(TILES) + 4u * (uint32_t)t);
        while (i > 0 && !is_person(M8(M32(PEOPLE) + (uint32_t)i * REC + 4)))   /* track stays */
            i = (int16_t)M16(M32(PEOPLE) + (uint32_t)i * REC + 2);
        if (i <= 0) return 1;
        uint32_t q = M32(PEOPLE) + (uint32_t)i * REC;
        int best = -1, bd = 999;
        for (int u = 0; u < 4096; u++) {   /* ponytail: a whole-map scan, once per person moved */
            if (u == t || skip(u, c) || (M8(M32(TILES) + 4u * (uint32_t)u + 3) & 8)) continue;
            if (!(tflags(u) & 2) && !(grass && M8(M32(TILES) + 4u * (uint32_t)u + 2) == 0 && chain_tail(u, 0) == 0)) continue;
            int d = abs(u % 64 - t % 64) + abs(u / 64 - t / 64);
            if (d < bd) { bd = d; best = u; }
        }
        if (best < 0) return 0;
        uint32_t a[3] = { q, (uint32_t)((best % 64) << 8 | 0x80), (uint32_t)((best / 64) << 8 | 0x80) };
        guest_call(0x408A44, 3, a);
    }
    return 0;
}
static int skip_none(int u, int c) { (void)u; (void)c; return 0; }

/* Fences also go where people stand (the grass by the gate, which guests
 * cross all day, #15): they step aside when it is built. */
void fence_place(void (*lifted)(void)) {
    int tile = (int)(M32(g_esp + 8) & 0xFFFF), commit = (int)M32(g_esp + 12);
    uint32_t t = M32(TILES) + 4u * (uint32_t)(tile & 4095);
    uint16_t f = tflags(tile & 4095);
    int16_t head = (int16_t)M16(t);
    if (!(tp_rules & RULE_BUILD) || tile < 0 || tile >= 4096 || (M8(t + 3) & 8) || (f != 0 && f != 0x400) || head <= 0) {
        lifted(); return;
    }
    int track = 0;
    for (int16_t i = head, hops = 0; i > 0 && hops < 256; hops++) {   /* only track and people on it */
        uint8_t ty = M8(M32(PEOPLE) + (uint32_t)i * REC + 4);
        if (ty == 0x12) track = 1;
        else if (!is_person(ty)) { lifted(); return; }
        i = (int16_t)M16(M32(PEOPLE) + (uint32_t)i * REC + 2);
    }
    if (chain_tail(tile, 0) != 0 || (f == 0x400 && !track)) { lifted(); return; }
    if (commit && !step_off(tile, 1, skip_none, 0)) { lifted(); return; }
    head = (int16_t)M16(t);                        /* the track, or nothing, once they have gone */
    uint8_t type = M8(t + 2);
    M16(t) = 0; M8(t + 2) = 0;                    /* as if bare grass */
    lifted();
    M16(t) = (uint16_t)head;                       /* the track stays on it */
    if (!commit || !g_eax) M8(t + 2) = type;       /* no fence after all */
    else rlog("fence %s at %d,%d", track ? "under the track" : "where people stood", tile % 64, tile / 64);
}

void lamp_place(void (*lifted)(void)) {
    int tile = (int)(M32(g_esp + 4) & 0xFFFF);
    uint32_t t = M32(TILES) + 4u * (uint32_t)(tile & 4095);
    if (!(tp_rules & RULE_BUILD) || tile < 0 || tile >= 4096 || M16(FEATURE_PICKED) != LAMP ||
        tflags(tile) != 2 || (M8(t + 3) & 8) || chain_tail(tile, 0) != 0) { lifted(); return; }
    uint16_t head = M16(t);
    uint8_t type = M8(t + 2);
    M16(t) = 0; M8(t + 2) = 0;
    lifted();
    int16_t lamp = (int16_t)M16(t);                /* what it put: the lamp, -3, or nothing */
    M16(t) = head; M8(t + 2) = type;               /* the path and whoever is on it */
    if (!g_eax || lamp >= 0) return;
    int16_t i = (int16_t)head;                     /* the lamp ends the chain */
    if (i <= 0) { M16(t) = (uint16_t)lamp; }
    else {
        for (int hops = 0; hops < 256; hops++) {
            uint32_t q = M32(PEOPLE) + (uint32_t)i * REC;
            if ((int16_t)M16(q + 2) <= 0) { M16(q + 2) = (uint16_t)lamp; break; }
            i = (int16_t)M16(q + 2);
        }
    }
    rlog("lamp on the path at %d,%d", tile % 64, tile / 64);
}

static void with_reserve(void (*lifted)(void)) {   /* a guest spawner, sees fewer records */
    if (!(tp_rules & RULE_BUILD)) { lifted(); return; }
    uint32_t end = M32(PEOPLE_END);
    M32(PEOPLE_END) = end - BUILD_RESERVE * REC;
    lifted();
    M32(PEOPLE_END) = end;
}
void guest_spawn(void (*lifted)(void)) { with_reserve(lifted); }    /* 0x41845A */
void guest_spawn2(void (*lifted)(void)) { with_reserve(lifted); }   /* 0x4564F1 */

/* The camera past the back of the park (#15). The camera's row (0x4C6666)
 * stops at 0, so the back wall sits under the date and the staff buttons and
 * the rows by it are awkward to build on. run_lift.py lets it go 3 rows
 * further with the rule (the edge scroll 0x4043EE, the keys' clamp 0x408583),
 * and the pointer above the map picks row 0, not a row before it (0x404B90).
 * The park view (0x40B837) would draw those rows from the memory before the
 * tile table, another of the game's blocks: while it draws, they read as bare
 * grass. */
#define CAM_ROW 0x4C6666

/* The cursor keys' clamp (0x4084DF) lets the camera go to column and row 63,
 * far off the right of the park and below the gate, and to column 0, past
 * the left; the edge scroll stops at the park's edges (0x404397..0x40444D).
 * With the rule the keys stop where the edge scroll does (#15): 'l'eft,
 * 'r'ight, 'b'ottom, each the last column or row the camera may take. */
int32_t tp_cam_limit(int edge) {
    extern int32_t tp_w, tp_h;
    int32_t w = (int16_t)M16(0x4C663A), h = (int16_t)M16(0x4C6654);
    if (edge == 'l') return tp_rules & RULE_BUILD ? 4 : 0;
    if (!(tp_rules & RULE_BUILD) || w <= 0 || h <= 0) return 0x3F;
    if (edge == 'r') return 64 - tp_w / w;
    return 64 - (h + ((M8(0x4CE5F6) & 1) ? 400 : tp_h) - 1) / h;
}
/* The row inside the front wall (row 55, the wall is 56). Making a park
 * (0x409077) locks it, all but the gate's columns 31-33, so nothing can be
 * built along the front (#15). With the rule it is unlocked; a save keeps the
 * lock, so this runs every frame, and the rule turned off locks the bare grass
 * again. Checked by the wall (type 0xCB) being where it should be. */
#define FRONT_ROW 55
static void front_row(void) {
    static int open;
    uint32_t t = M32(TILES);
    if (!t || M8(t + 4u * (FRONT_ROW + 1) * 64 + 4 * 10 + 2) != 0xCB) return;   /* not in a park */
    int want = !!(tp_rules & RULE_BUILD);
    if (!want && !open) return;
    for (int x = 1; x < 63; x++) {
        if (x >= 31 && x <= 33) continue;
        uint32_t a = t + 4u * (FRONT_ROW * 64 + x);
        if (want) M8(a + 3) &= (uint8_t)~8;
        else if (M8(a + 2) == 0 && M16(a) == 0) M8(a + 3) |= 8;
    }
    open = want;
}
#define ABOVE_ROWS 4         /* the 3 rows, and one for anything tall */
static void view_above(void (*lifted)(void)) {
    if ((int16_t)M16(CAM_ROW) >= 0) { lifted(); return; }
    static uint32_t keep[ABOVE_ROWS * 64];
    uint32_t a = M32(TILES) - sizeof keep;
    for (int k = 0; k < ABOVE_ROWS * 64; k++) { keep[k] = M32(a + 4u * k); M32(a + 4u * k) = 0; }
    lifted();
    for (int k = 0; k < ABOVE_ROWS * 64; k++) M32(a + 4u * k) = keep[k];
}

/* Fountains on paths (#21). A fountain is an object (type 0x18, +0xC kind 4)
 * on the middle tile of a 3x2 footprint (offsets at 0x4A8FBE: -65 -64 -63 -1
 * 0 +1); placing it (0x430E92(tile), the feature picked at 0x4C6646) needs
 * all six tiles bare ground, and makes them solid (type 0x67); removing it
 * (0x43121E(record)) makes them grass. With the rule a fountain also goes
 * where its footprint is plain path (or path and grass): whoever stands there
 * steps off to the nearest path, the paths round it join up again, and the
 * fountain remembers which tiles were path (+0x14, which only toilets use:
 * 0xF000 | one bit a tile) so taking it away lays them back. */
static const int foot[6] = { -65, -64, -63, -1, 0, 1 };
#define FOUNTAIN_KIND 4
#define PATH_TYPE 0x55            /* a path tile; 0x418D5A picks the variant that joins its neighbours */
#define PATH_MARK 0xF000

static void join_paths(int tile) { uint32_t a = (uint32_t)tile; if (tile >= 0 && tile < 4096) guest_call(0x418D5A, 1, &a); }

static void join_round(int centre) {          /* the paths on the footprint and the ring round it */
    for (int dy = -2; dy <= 1; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = centre % 64 + dx, y = centre / 64 + dy;
            if (x >= 0 && x < 64 && y >= 0 && y < 64 && (tflags(y * 64 + x) & 2)) join_paths(y * 64 + x);
        }
}

static int in_foot(int centre, int t) { for (int k = 0; k < 6; k++) if (centre + foot[k] == t) return 1; return 0; }
static int skip_foot(int u, int c) { return in_foot(c, u); }

void fountain_place(void (*lifted)(void)) {
    int c = (int)(M32(g_esp + 4) & 0xFFFF), paths = 0;
    if (!(tp_rules & RULE_BUILD) || M16(0x4BCA9E + M16(FEATURE_PICKED) * 0x38u) != FOUNTAIN_KIND || c < 65 || c >= 4096 - 1 ||
        (M8(M32(TILES) + 4u * (uint32_t)c + 3) & 8)) { lifted(); return; }
    for (int k = 0; k < 6; k++) {
        int t = c + foot[k];
        uint16_t f = tflags(t);
        if (f != 0 && f != 2) { rlog("fountain: %d,%d is not grass or path", t % 64, t / 64); lifted(); return; }
        if (chain_tail(t, 0) != 0) { rlog("fountain: a tree or lamp at %d,%d", t % 64, t / 64); lifted(); return; }
        for (int16_t i = (int16_t)M16(M32(TILES) + 4u * (uint32_t)t), hops = 0; i > 0 && hops < 256; hops++) {
            uint32_t q = M32(PEOPLE) + (uint32_t)i * REC;
            uint8_t ty = M8(q + 4);
            if (ty != 2 && ty != 8 && ty != 0x1A) {   /* track, another object */
                rlog("fountain: a type %#x object at %d,%d", ty, t % 64, t / 64); lifted(); return;
            }
            i = (int16_t)M16(q + 2);
        }
        if (f == 2) paths |= 1 << k;
    }
    if (!paths) { lifted(); return; }
    /* Off the footprint, to the nearest path outside it. */
    for (int k = 0; k < 6; k++)
        if (!step_off(c + foot[k], 0, skip_foot, c)) { lifted(); return; }
    uint8_t type[6];
    for (int k = 0; k < 6; k++) { uint32_t t = M32(TILES) + 4u * (uint32_t)(c + foot[k]); type[k] = M8(t + 2); if (paths >> k & 1) M8(t + 2) = 0; }
    lifted();
    if (!g_eax) {                                  /* not placed: the paths as they were */
        for (int k = 0; k < 6; k++) M8(M32(TILES) + 4u * (uint32_t)(c + foot[k]) + 2) = type[k];
        return;
    }
    for (int16_t i = (int16_t)M16(M32(TILES) + 4u * (uint32_t)c), hops = 0; i > 0 && hops < 256; hops++) {
        uint32_t q = M32(PEOPLE) + (uint32_t)i * REC;
        if (M8(q + 4) == 0x18 && M8(q + 0xC) == FOUNTAIN_KIND) { M16(q + 0x14) = (uint16_t)(PATH_MARK | paths); break; }
        i = (int16_t)M16(q + 2);
    }
    join_round(c);
    rlog("fountain on the path at %d,%d", c % 64, c / 64);
}

/* The bulldozer (0x4105D7(tile)) takes a fountain away from any of its tiles,
 * and leaves the middle one grass after the fountain's own removal. A lamp on
 * a path goes before the path (#21): the game takes the lamp only when the
 * chain starts with it, so with someone on the tile it took the path instead
 * and left the lamp on the grass. With the rule the bulldozer sees the lamp
 * alone, and whoever stands there stays. */
void bulldoze(void (*lifted)(void)) {
    int t = (int)(M32(g_esp + 4) & 0xFFFF), c = -1;
    uint32_t q = 0;
    uint16_t mark = 0;
    if ((tp_rules & RULE_BUILD) && t < 4096 && (tflags(t) & 2)) {
        uint32_t a = M32(TILES) + 4u * (uint32_t)t;
        int16_t head = (int16_t)M16(a), tail = chain_tail(t, 0);
        if (head > 0 && tail < 0 && tail >= -11) {
            M16(a) = (uint16_t)tail;
            lifted();
            uint16_t left = M16(a);                    /* 0 once the lamp has gone */
            M16(a) = (uint16_t)head;
            int16_t i = head;
            for (int hops = 0; hops < 256; hops++) {   /* the last person now ends in it */
                uint32_t r = M32(PEOPLE) + (uint32_t)i * REC;
                if ((int16_t)M16(r + 2) <= 0) { M16(r + 2) = left; break; }
                i = (int16_t)M16(r + 2);
            }
            rlog("bulldozer: the lamp at %d,%d before the path", t % 64, t / 64);
            return;
        }
    }
    for (int k = 0; k < 6 && c < 0 && (tp_rules & RULE_BUILD); k++) {   /* the fountain this tile is part of */
        int cc = t - foot[k];
        if (cc < 0 || cc >= 4096) continue;
        for (int16_t i = (int16_t)M16(M32(TILES) + 4u * (uint32_t)cc), hops = 0; i > 0 && hops < 256; hops++) {
            uint32_t r = M32(PEOPLE) + (uint32_t)i * REC;
            if (M8(r + 4) == 0x18 && M8(r + 0xC) == FOUNTAIN_KIND && (M16(r + 0x14) & 0xF000) == PATH_MARK) {
                c = cc; q = r; mark = M16(r + 0x14); break;
            }
            i = (int16_t)M16(r + 2);
        }
    }
    lifted();
    if (c < 0 || M8(q + 4) == 0x18) return;       /* no fountain of ours, or it is still there */
    for (int k = 0; k < 6; k++)
        if (mark >> k & 1) M8(M32(TILES) + 4u * (uint32_t)(c + foot[k]) + 2) = PATH_TYPE;
    join_round(c);
    rlog("fountain taken away at %d,%d: the path is back", c % 64, c / 64);
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
#define BLOCKS_WALK 0x29C1    /* flags a guest walking out may not cross (0x4127AD) */
#define ENTRANCE   0x4C4706   /* w: the gate's tile, player record +0x2CA */
#define STUCK_TRIES 4         /* tries (50 ticks each) before a move */
#define STUCK_HOP  8          /* tiles along the way, when the tile has one */

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
        g_esp += 4;                               /* the original's ret, which did not run */
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
        g_esp += 4;                               /* the original's ret */
    }
    else lifted();
    M32(park + 0xB6) = total;
    static int16_t shown = -1;
    if ((int16_t)M16(ADVICE_NOW) != shown && (shown = (int16_t)M16(ADVICE_NOW)) >= 0)
        rlog("advisor (game): %.150s", (const char*)dos32_mem(ADVICE_TEXT));
}

/* ---- lights (#18) ------------------------------------------------------- */
/* In rain and storms the game darkens the whole palette (0x437D38(dst, src,
 * amount): every colour times (256 - amount)/256), and nothing lights up.
 * With the rule, lamp posts glow, more the darker it is. A lamp is a tile
 * whose chain ends in -3 (feature 2); the view puts tile (x, y) at
 * col = x - [0x4C667A], row = y - [0x4C6666],
 *   screen x = s * (col*W + row*W/3 + [0x4C6624]),  screen y = s * row*H,
 * W [0x4C663A] and H [0x4C6654] (24 and 16 in 640x480; 48 and 32 in 320x200,
 * where s is 1/2). The globe is 20 right and 19 up of that. Only the picture
 * changes: the simulation never sees it. */
#define LAMP_TAIL   -3
#define MAX_LIGHTS  256
#define GLOW_R      22             /* pixels */

static struct { volatile uint32_t seq; int n, dark, view_h; int16_t x[MAX_LIGHTS], y[MAX_LIGHTS]; } lights;
static int darkness;              /* the game's own: 0 bright .. 255 */

static int park_drawn;            /* the park view was drawn since the last frame (0x40B837) */
static int park_since;            /* ... and since the last frame shown */
static volatile int lights_valid;  /* the frame on the screen is the park's */

void park_view(void (*lifted)(void)) { park_drawn = park_since = 1; view_above(lifted); }

/* game.c: the game shows a frame (its presents), or another screen draws its
 * own (the negotiations): the lights belong to a frame with the park in it. */
void lights_shown(int other) { lights_valid = !other && park_since; park_since = 0; }

void palette_dim(void (*lifted)(void)) {   /* 0x437D38(dst, src, amount) */
    darkness = (int)(M32(g_esp + 12) & 0xFF);
    lifted();
}

static void lights_frame(void) {
    int on = (tp_rules & RULE_LIGHTS) && park_drawn && M8(0x4A14CC) && darkness > 0;   /* not on other screens */
    park_drawn = 0;
    if (!M8(0x4A14CC)) darkness = 0;   /* the weather is over: the game put the palette back */
    int n = 0;
    if (on) {
        int W = (int16_t)M16(0x4C663A), H = (int16_t)M16(0x4C6654), off = (int16_t)M16(0x4C6624);
        int cx = (int16_t)M16(0x4C667A), cy = (int16_t)M16(0x4C6666);
        if (W <= 0 || H <= 0) on = 0;
        for (int t = 0; on && t < 4096 && n < MAX_LIGHTS; t++) {
            if (chain_tail(t, 0) != LAMP_TAIL) continue;
            int col = t % 64 - cx, row = t / 64 - cy;
            int sx = (col * W + row * W / 3 + off) * 24 / W + 20, sy = row * H * 24 / W - 19;
            if (sx < -GLOW_R || sx > 1920 + GLOW_R || sy < -GLOW_R || sy > 1080) continue;
            lights.x[n] = (int16_t)sx; lights.y[n] = (int16_t)sy; n++;
        }
        lights.view_h = W == 48 ? 176 : tp_vh;   /* above the panel */
    }
    lights.dark = on ? darkness : 0;
    lights.n = n;
    lights.seq++;
}

/* The presenters, on each new frame (XRGB, w x h): a warm glow round each
 * lamp, added to the darkened picture. */
void lights_apply(uint32_t* px, int w, int h) {
    int dark = lights.dark, n = lights.n, vh = lights.view_h < h ? lights.view_h : h;
    if (dark <= 0 || n <= 0 || !lights_valid) return;   /* another screen: a report, a negotiation */
    dark = dark * 3 > 255 ? 255 : dark * 3;             /* storms darken by a third or so: full glow by then */
    static const int glow[3] = { 255, 214, 140 };   /* lamplight */
    for (int i = 0; i < n && i < MAX_LIGHTS; i++) {
        int cx = lights.x[i], cy = lights.y[i];
        for (int y = cy - GLOW_R; y <= cy + GLOW_R; y++) {
            if (y < 0 || y >= vh) continue;
            for (int x = cx - GLOW_R; x <= cx + GLOW_R; x++) {
                if (x < 0 || x >= w) continue;
                int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy) * 2;   /* a little flatter than round */
                if (d2 >= GLOW_R * GLOW_R) continue;
                int f = GLOW_R * GLOW_R - d2;                              /* 0 .. R^2 */
                int a = f * f / (GLOW_R * GLOW_R) * dark / 255 * 200 / (GLOW_R * GLOW_R);   /* 0..200 */
                uint32_t c = px[y * w + x];
                int r = (int)(c >> 16 & 255) + glow[0] * a / 255, g = (int)(c >> 8 & 255) + glow[1] * a / 255,
                    b = (int)(c & 255) + glow[2] * a / 255;
                px[y * w + x] = (uint32_t)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b));
            }
        }
    }
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

/* The buttons' actions (0x4A7537, 6 bytes each: two sprites, a hotkey, a
 * kind). A key press runs the action whose hotkey it is; the staff window's
 * close has Esc (1), the other windows' ticks have none (0xFF). */
static const uint8_t esc_actions[] = { 0x03, 0x18 };
static void esc_keys(void) {
    for (unsigned i = 0; i < sizeof esc_actions; i++)
        M8(0x4A7537 + esc_actions[i] * 6 + 4) = (tp_rules & RULE_ESC) ? 0x01 : 0xFF;
}

/* Esc on the full screens, which the ticks' hotkey doesn't reach (#43):
 * - research (0x4E) and the bank statement (0x40) close on Enter, not Esc;
 * - the stock market (0x44) and the world map (from the park, not the new
 *   game's) close only on a click on their tick, so Esc clicks it (the
 *   pointer moves there). */
static int in_world_map;
void world_map(void (*lifted)(void)) {         /* 0x4460D2(): the world map's loop */
    in_world_map = 1;
    lifted();
    in_world_map = 0;
}

static void esc_click(int x, int y) {
    M16(0x4AECAC) = (uint16_t)x; M16(0x4AECAE) = (uint16_t)y; M16(0x4CE5AC) = 1;
    M8(0x4CE688) = 0; g_eax &= ~0xFFu;
}

void key_read(void (*lifted)(void)) {          /* 0x46E888(): the poll reads a key */
    lifted();
    if (!(tp_rules & RULE_ESC) || M8(0x4CE688) != 1) return;
    uint16_t s = M16(0x4C6652);
    rlog("esc: screen %02x%s", s, in_world_map ? ", world map" : "");
    if (s == 0x4E || s == 0x40) { M8(0x4CE688) = 0x1C; g_eax = (g_eax & ~0xFFu) | M8(0x4AECE0 + 0x1C); }
    else if (s == 0x44) esc_click(0x238 + 24, 0x148 + 15);                 /* 0x43BC83 */
    else if (s == 0 && in_world_map) esc_click(0x238 + 29, 0x16E + 10);    /* 0x44644F */
}

void rules_frame(void) {   /* game.c, between frames on the game thread */
    lights_frame();
    esc_keys();
    front_row();
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

/* ---- negotiation sounds (#49) -------------------------------------------- */
/* The negotiation loop (0x434EA5): once the offer (0x4C7804) comes down to the
 * ask (0x4C77FC) it stops the heartbeat and cheers (0x436243) at once, but the
 * deal only closes when the hands drawn ([ebp-0x20]) have slid to where they
 * should be ([ebp-0x44]). Pull back before then and the heartbeat is gone for
 * good, and every frame the two meet the cheer is asked for again. With the
 * rule both wait for the deal, which runs the same branch once it closes. */
static int deal_pending(void) {
    extern uint32_t g_cur_func;                    /* in an override: the caller; staff and goods alike */
    return (tp_rules & RULE_DEAL) && g_cur_func >= 0x434EA5u && g_cur_func < 0x436F27u && (int32_t)M32(g_ebp - 0x50) > 0 &&
           (int32_t)M32(0x4C7804) <= (int32_t)M32(0x4C77FC) && (int16_t)M16(g_ebp - 0x44) > (int16_t)M16(g_ebp - 0x20);
}

void song_stop(void (*lifted)(void)) {          /* 0x470990() */
    if (deal_pending()) { g_esp += 4; return; }  /* its ret */
    lifted();
}

void effect_start(void (*lifted)(void)) {       /* 0x470FE0(pan, effect): 0x27 the cheer */
    if (M32(g_esp + 8) == 0x27 && deal_pending()) { rlog("deal: cheer held"); g_esp += 4; return; }
    extern void effect_note(void (*)(void));     /* sound.c */
    effect_note(lifted);
}
