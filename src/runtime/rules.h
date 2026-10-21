/* Modern rules (rules.c, docs/rules.md): opt-in changes to the game's behaviour. */
#pragma once
#include <stdint.h>

enum { RULE_GUARDS = 1, RULE_STUCK = 2, RULE_ADVISOR = 4, RULE_ENTERTAIN = 8, RULE_HANDYMEN = 16, RULE_BUILD = 32, RULE_LIGHTS = 64 };
#define RULE_MAX 16              /* the menus have room for this many */

typedef struct { uint32_t bit; const char* key; const char* label; const char* about; } rule_t;
extern const rule_t rule_list[];
extern const int rule_count;
extern uint32_t tp_rules;        /* the rules on now: the host's in a network game */

void rules_load(const char* ini);
void rules_save(const char* ini);
void rules_frame(void);
void lights_apply(uint32_t* px, int w, int h);   /* presenters: on each new frame */
void lights_shown(int other);                     /* game.c: a frame shown (other: not by a present) */          /* between frames, on the game thread */
void rules_queue_call(uint32_t addr, int n, const uint32_t* a);   /* --call */
