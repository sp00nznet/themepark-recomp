/* Multiplayer (net.c): the links, and actions between players (house rules). */
#pragma once
#include <stdint.h>

extern int net_me, net_n;        /* this machine's player number (0 is the host); players, or 0 */

/* A house-rule action: carried to a player's machine and done there, on the
 * game thread at the park's frame (net_action_do, net_rules.c). */
typedef struct { uint8_t kind, pad[3]; int32_t amount; int32_t arg; } net_action_t;
enum {
    ACT_PAY = 1,      /* to yourself: give `amount` to player `arg` (pays, then sends ACT_GIFT) */
    ACT_GIFT,         /* `amount` from the sender, who has paid */
    ACT_GRANT,        /* the host's: `amount` from nowhere (a fine if negative) */
    ACT_RESEARCH,     /* item `arg` (table << 8 | index) is now available */
    ACT_LEVEL,        /* the host's: the game type is `arg` */
};
void net_bye(void);              /* leaving for good: the others play on */
void net_send_name(const char name[16]);   /* this player's name, to the others */
void net_name_of(int p, char out[16]);     /* player p's, or "" */
void net_names_restore(void);              /* alone again: the game's own names */

void net_send_action(int to, const net_action_t* a);
void net_action_received(int from, const void* p, uint32_t len);
void net_actions_apply(int now);
void net_action_do(int from, const net_action_t* a);
int  net_player_up(int i);
int  net_enabled(void);

/* House rules (net_rules.c), for the settings menu. */
extern int net_sharing[4];
void net_rules_frame(void);
void net_rules_give(int to, int amount);
void net_rules_grant(int to, int amount);
void net_rules_share(int to, int on);
int  net_last_hosted(void);      /* the size of the last game this machine hosted, or 0 */
extern int net_host_players;     /* nonzero on the host */
