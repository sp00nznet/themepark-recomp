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
