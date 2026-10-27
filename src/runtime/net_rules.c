/* House rules in a network game (docs/multiplayer.md): what players can do for
 * each other on top of the game's own rules, from the settings menu.
 *
 *   give money       from your own cash to another player
 *   grant or fine    the host only: money from nowhere, or taken away
 *   share research   every ride, shop and feature you find, that player gets too
 *
 * Each action is carried to the receiving player's machine (net.c) and done
 * there on the game thread, at the park's frame. Only that player's park
 * changes, which is the only one that machine simulates; the others see it in
 * the charts at the next sync. Money goes through the game's own ledger
 * (0x468943), so the cash, the finance screens and the year's accounts agree.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "compat.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "dos32.h"
#include "net.h"

#define M8(a)  (*(uint8_t*)dos32_mem(a))
#define M32(a) (*(uint32_t*)dos32_mem(a))

#define PARK      0x4C3808   /* pointer to this player's park record */
#define CASH      0x0A       /* +: cash, signed */
#define COMMITTED 0x0E       /* +: spent but not yet paid */
#define RESEARCH_ICON 0x4C7768   /* 1: the research button flashes */
#define CAT_AWARD 0x0F       /* ledger categories: "Cash award" */
#define CAT_MISC  0x18       /* "Misc." */
#define LEVEL     0x4C66F1   /* the game type: 0 sandbox, 1 simulation, 2 full (new games: 0x40A0C8) */
#define NAMES     0x4C7B98   /* LANG group 1: the parks' owners ("HUMAN", then the rivals') */
#define PLAYER_REC 0x4C443C  /* 0x6C2 a player, synced: +0x20 the park's name */
#define MY_NAME   0x4C66B0   /* this player's name, as typed at the start */
#define NAME_BUF  0x4C6586   /* 4 x 0x20: the end of the 5th player record, unused (4 players at most) */

static int host_level = -1;

extern char state_note[128];             /* game.c: the title bar */
extern volatile DWORD state_note_until;
void sub_00468943(void);                 /* the ledger: (category, amount) */

int net_sharing[4];                      /* this machine shares its research with player i */

static void note(const char* text) {
    snprintf(state_note, sizeof state_note, "%s", text);
    state_note_until = GetTickCount() + 5000;
    fprintf(stderr, "themepark: network: %s\n", text);
}

/* Book money through the game's ledger, as its own purchases do. */
static void ledger(int category, int32_t amount) {
    uint32_t r[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp }, cur = g_cur_func;
    g_esp -= 4; M32(g_esp) = (uint32_t)amount;
    g_esp -= 4; M32(g_esp) = (uint32_t)category;
    g_esp -= 4; M32(g_esp) = RECOMP_RETADDR;   /* its ret pops this */
    sub_00468943();
    g_esp += 8;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esi = r[4]; g_edi = r[5]; g_ebp = r[6];
    g_cur_func = cur;
}

static int32_t cash(void) { return (int32_t)M32(M32(PARK) + CASH); }
static int32_t spare(void) { return cash() - (int32_t)M32(M32(PARK) + COMMITTED); }

/* The item tables (each machine keeps its own; no sync carries them). */
static const struct { uint32_t base, stride, flag; int count; const char* what; } table[] = {
    { 0x4BCED0, 0xC8, 0xC5, 29, "ride" },
    { 0x4C3C62, 0x6B, 0x66, 17, "shop" },
    { 0x4BCA92, 0x38, 0x13, 19, "feature" },
};
#define FLAG(t, i) M8(table[t].base + (uint32_t)(i) * table[t].stride + table[t].flag)

void net_action_do(int from, const net_action_t* a) {
    char t[128];
    switch (a->kind) {
    case ACT_PAY:                        /* here: this player gives a->amount to player a->arg */
        if (a->amount <= 0 || a->arg < 0 || a->arg >= net_n || a->arg == net_me) return;
        if (spare() < a->amount) { snprintf(t, sizeof t, "Not enough money to give %d", a->amount); note(t); return; }
        ledger(CAT_MISC, -a->amount);
        net_send_action(a->arg, &(net_action_t){ ACT_GIFT, {0}, a->amount, 0 });
        snprintf(t, sizeof t, "You gave player %d %d", a->arg + 1, a->amount); note(t);
        break;
    case ACT_GIFT:
        if (a->amount <= 0) return;
        ledger(CAT_AWARD, a->amount);
        snprintf(t, sizeof t, "Player %d gave you %d", from + 1, a->amount); note(t);
        break;
    case ACT_GRANT:                      /* the host's: money from nowhere, or a fine */
        if (from != 0 || !a->amount) return;
        ledger(a->amount > 0 ? CAT_AWARD : CAT_MISC, a->amount);
        snprintf(t, sizeof t, a->amount > 0 ? "The host granted you %d" : "The host fined you %d",
                 a->amount > 0 ? a->amount : -a->amount);
        note(t);
        break;
    case ACT_RESEARCH: {
        int tb = a->arg >> 8, i = a->arg & 0xFF;
        if (tb < 0 || tb > 2 || i >= table[tb].count || (FLAG(tb, i) & 1)) return;
        FLAG(tb, i) |= 3;                /* available, and new: what the game's own research does */
        M8(RESEARCH_ICON) = 1;
        snprintf(t, sizeof t, "Player %d shared a new %s with you", from + 1, table[tb].what); note(t);
        break; }
    case ACT_LEVEL:
        if (from == 0 && a->arg >= 0 && a->arg <= 2) host_level = a->arg;
        break;
    }
}

/* The game type (sandbox, simulation, full) is each machine's own in the 1994
 * game: here the host's is everyone's (#37). Its ticks in the menu, too. */
static void set_level(int l) {
    M8(LEVEL) = (uint8_t)l;
    M8(0x4C7761) = l == 0; M8(0x4C7759) = l == 1; M8(0x4C775E) = l == 2;
}

static void level_frame(void) {
    static int sent = -1;
    if (net_me == 0) {
        int l = M8(LEVEL);
        if (l != sent)
            for (int p = 1; p < net_n; p++) net_send_action(p, &(net_action_t){ ACT_LEVEL, {0}, 0, l });
        sent = l;
    } else if (host_level >= 0 && M8(LEVEL) != host_level) {
        set_level(host_level);
        note("The host picks the game type");
    }
}

/* A player's park was "HUMAN" to the others (the first of the owners' names,
 * LANG group 1, picked by the park's +1 byte, which is its number), as in
 * "HUMAN HAS JUST BOUGHT SOME SHARES". Now the name each player typed at the
 * start, which only that machine knows, so it is sent round; until it comes,
 * their park's name, from their record, which every sync carries (#37). */
static uint32_t name_was[4];             /* the game's own pointers, for a game alone again */

void net_names_restore(void) {
    uint32_t t = M32(NAMES);
    for (int p = 0; p < 4 && t; p++) if (name_was[p]) M32(t + 4u * (uint32_t)p) = name_was[p], name_was[p] = 0;
}

static void names_frame(void) {
    static char sent[16];
    static int every;
    char mine[16], n[16];
    for (int p = 0; p < net_n && M32(NAMES); p++) {   /* the game's own, also after a language change */
        uint32_t cur = M32(M32(NAMES) + 4u * (uint32_t)p);
        if (cur != NAME_BUF + 0x20u * (uint32_t)p && cur != PLAYER_REC + 0x6C2u * (uint32_t)p + 0x20) name_was[p] = cur;
    }
    memcpy(mine, dos32_mem(MY_NAME), 15); mine[15] = 0;
    if (mine[0] && (strcmp(mine, sent) || ++every % 500 == 0)) {   /* again now and then: a rejoined player */
        net_send_name(mine);
        memcpy(sent, mine, 16);
    }
    uint32_t t = M32(NAMES);
    for (int p = 0; p < net_n && t; p++) {
        uint32_t buf = NAME_BUF + 0x20u * (uint32_t)p, park = PLAYER_REC + 0x6C2u * (uint32_t)p + 0x20;
        if (p == net_me) memcpy(n, mine, 16); else net_name_of(p, n);
        if (n[0]) { memcpy(dos32_mem(buf), n, 16); M32(t + 4u * (uint32_t)p) = buf; }
        else if (M8(park)) M32(t + 4u * (uint32_t)p) = park;
    }
}

/* Every park frame: anything that became available here goes to whoever this
 * player shares research with. */
static uint8_t seen[3][32];
static int seen_ready;

void net_rules_frame(void) {
    for (int tb = 0; tb < 3; tb++)
        for (int i = 0; i < table[tb].count; i++) {
            int now = FLAG(tb, i) & 1;
            if (now && !seen[tb][i] && seen_ready)
                for (int p = 0; p < net_n; p++)
                    if (p != net_me && net_sharing[p])
                        net_send_action(p, &(net_action_t){ ACT_RESEARCH, {0}, 0, tb << 8 | i });
            seen[tb][i] = (uint8_t)now;
        }
    seen_ready = 1;
    level_frame();
    names_frame();
}

/* From the settings menu (the window's thread): queued for the game's. */
void net_rules_give(int to, int amount) { net_send_action(net_me, &(net_action_t){ ACT_PAY, {0}, amount, to }); }
void net_rules_grant(int to, int amount) { if (net_me == 0) net_send_action(to, &(net_action_t){ ACT_GRANT, {0}, amount, 0 }); }
void net_rules_share(int to, int on) {
    if (to < 0 || to >= 4 || to == net_me) return;
    net_sharing[to] = on;
    char t[96];
    snprintf(t, sizeof t, on ? "Sharing your research with player %d" : "No longer sharing research with player %d", to + 1);
    note(t);
}
