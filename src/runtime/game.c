/*
 * Theme Park specifics: the host functions run_lift.py's OVERRIDES put in
 * place of lifted ones.
 *
 * Pacing. The DOS game has no frame limiter: it ran as fast as a 486 could
 * draw and simulate, and every timeout in it (the name prompt, the attract
 * demo's scenes, the simulation's days) counts frames. Native, it runs
 * hundreds of frames a second and all of those race. pace_frame holds each
 * present to the frame rate in game_fps, which is also the speed control.
 */
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dos32.h"
#include "rules.h"

double game_fps = 25.0;   /* about what a fast 486 managed; main.c --fps */

static double next_s;

/* Save states (the settings menu, --save/--load): carried out here, between
 * frames, on the guest thread. A state loads only where it was saved: from
 * this hook, at the same depth of the host's stack, which in practice means
 * inside a park (README, Save states). */
volatile int state_request;      /* slot to save, or -slot to load */
char state_note[128];            /* what happened, for the title bar */
volatile DWORD state_note_until;
void sound_reset(void);          /* sound.c */

void state_path(int slot, char* out, size_t n) {
    char dir[MAX_PATH];
    exe_dir(dir, sizeof dir);
    strncat(dir, "states", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s/slot%d.tps", dir, slot);
}

static void do_state(int req) {
    uint32_t where = 0x0040107Fu;   /* the input poll pace_frame stands in for */
    int slot = req < 0 ? -req : req, r;
    char path[MAX_PATH];
    state_path(slot, path, sizeof path);
    if (req > 0) {
        r = dos32_state_save(path, where);
        snprintf(state_note, sizeof state_note, r ? "Could not save slot %d" : "Saved slot %d", slot);
    } else {
        r = dos32_state_load(path, where);
        if (!r) sound_reset();
        snprintf(state_note, sizeof state_note, !r ? "Loaded slot %d" :
                 r == -2 ? "Slot %d was saved elsewhere in the game: load it from inside a park" :
                 "Slot %d is empty or from another build", slot);
    }
    fprintf(stderr, "[state] %s\n", state_note);
    state_note_until = GetTickCount() + 4000;
}

/* Hold the caller until `period` seconds after the last frame. */
static void pace_hold(double period) {
    if (dos32_virtual_clock) { dos32_advance(period); return; }
    static LARGE_INTEGER f, t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    double now = (double)t.QuadPart / (double)f.QuadPart;
    if (next_s - now > 1.0 || now - next_s > 0.25) next_s = now;   /* first frame, or a long stall */
    if (next_s > now) dos32_idle(next_s - now);
    next_s += period;
}

/* Hold the caller until the next frame is due. */
static void pace_wait(void) {
    if (game_fps > 0) pace_hold(1.0 / game_fps);   /* --fps 0: flat out, as on DOS */
}

static int polled;   /* the input poll ran since the last present */
extern uint32_t g_cur_func;   /* the lifted function running now */

void pace_frame(void (*lifted)(void)) {
    int req = state_request;
    if (req) { state_request = 0; do_state(req); }
    rules_frame();
    polled = 1;
    pace_wait();
    lifted();
}

/* The two presents (0x470210 copies the 320x200 back buffer to the screen,
 * 0x445507 draws the pointer and shows a 640x480 frame). Some loops present
 * without ever calling the input poll: the year-end report, which closes
 * itself after 1000 passes with the mouse still, and the staff and goods
 * negotiations, whose clock counts passes. On a 486 each pass redrew the
 * screen and took a good fraction of a second; native, all 1000 go by in one
 * frame. A frame that did not go through the poll is paced here instead, so
 * every loop runs at the frame rate and none is paced twice.
 *
 * Except the takeover and game-over films (0x44C7D1, issue #10): their loop
 * has a pace of its own, one retrace (70 Hz) a pass and in 320x200 a new
 * frame every 4th pass, 17.5 frames a second. In 640x480 it shows a new frame
 * every pass, which a 486 could only manage every few retraces; hold those to
 * the same 17.5. Paced at the game's frame rate instead, 320x200 played at
 * 6 frames a second. */
void pace_present(void (*lifted)(void)) {
    if (g_cur_func >= 0x0044C7D1u && g_cur_func < 0x0044CA6Bu) {
        if (!(*dos32_mem(0x4CE5F6) & 1)) pace_hold(4.0 / 70);   /* bit 0: 320x200 */
    }
    else if (!polled) pace_wait();
    polled = 0;
    lifted();
}

/* The wait for vertical retrace (0x470BE4). The staff and goods negotiation
 * (0x434EA5) has a loop of its own that copies each frame to the screen
 * itself and waits for one retrace a pass. It lasts 620 passes, the other
 * side answering every 62; on a 486 a pass took a frame or two, but native,
 * several passes fit in one retrace and the whole negotiation was over in
 * two or three seconds. Its passes are frames, paced like every other.
 * The palette fades wait for retrace too, many times a frame, and keep the
 * plain 70 Hz wait. */
void pace_retrace(void (*lifted)(void)) {
    if (g_cur_func >= 0x00434EA5u && g_cur_func < 0x00436F27u) {
        if (!polled) pace_wait();
        polled = 0;
    }
    lifted();
}

/* Bullfrog's memory manager (0x472950) takes every block malloc will give,
 * down to 16 bytes, and runs its own pools on them. Under DOS/4GW something
 * was left for the C library; here nothing is, and the next fopen (the sound
 * setup file) fails on its first malloc. Hold 1 MB of DPMI memory back while
 * the grab runs and give it back after: the C library's heap grows into it. */
void hold_back_memory(void (*lifted)(void)) {
    uint32_t held = dos32_heap_alloc(1u << 20);
    lifted();
    if (held) dos32_heap_free(held);
}

/* The cursor coming apart (issue #6). The mouse set-up (0x46DF44) takes three
 * 4 KB buffers for the pointer from Bullfrog's pools (0x46E8D0), and it runs
 * again at every change of resolution (0x400D42: R, and the screens that
 * switch). The 1994 game took three new ones each time and never gave them
 * back. GOG's patch meant to take them once: it rewrites the set-up's own
 * code after the first run to jump past the allocations, and drops the check
 * for an allocation that failed. A rewrite of code does nothing to the lifted
 * C, so here every switch took 12 KB more; after about 60 the pool was empty,
 * the buffers were at address 0, on top of the BIOS data area, and the
 * pointer drawn from them lost its rows. Do what GOG meant: while the set-up
 * runs, a buffer it already has (in its own variables, so a loaded save
 * state keeps working) is given again. */
#define MEM32(a) (*(uint32_t*)dos32_mem(a))
extern uint32_t g_esp, g_eax;
static int mouse_setup_allocs = -1;     /* allocations so far in this run of the set-up; -1 outside */
static const uint32_t mouse_bufs[3] = { 0x4CE590, 0x4CE564, 0x4CE554 };   /* in the order it takes them */

void mouse_setup(void (*lifted)(void)) {
    mouse_setup_allocs = 0;
    lifted();
    mouse_setup_allocs = -1;
}

void pool_alloc(void (*lifted)(void)) {   /* 0x46E8D0(bytes) */
    if (mouse_setup_allocs >= 0 && mouse_setup_allocs < 3 && MEM32(g_esp + 4) == 0x1000) {
        uint32_t had = MEM32(mouse_bufs[mouse_setup_allocs++]);
        if (had) { g_eax = had; g_esp += 4; return; }   /* return as its ret would */
    }
    lifted();
}
/* ---- custom resolutions (#11) ------------------------------------------ */
/* The park's screen, 640x480 in the game, at a size of the player's: the
 * game's 640x480 mode set (0x46ECF0) asks dos32 for its own-size VESA mode
 * instead, the game's screen size and clip are set to it, and the lifted
 * code's view sizes (run_lift.py SCREEN_SITES) read tp_w/tp_h/tp_vh. The
 * panel stays 640 wide at the bottom left. Height: 480 only for now (the
 * panel's place is fixed by many constants). docs/resolutions.md. */
extern uint32_t g_esp;
#define M32W(a) (*(uint32_t*)dos32_mem(a))
volatile int res_w;            /* the size a park opens at (menu, --res, [game] res); 0: 640 */
#define RES_MAX_W 1920        /* the widest the back buffers are made for */
int32_t tp_w = 640, tp_h = 480, tp_vh = 400;   /* the screen now; lifted code reads them */

void vesa_set(void (*lifted)(void)) {   /* 640x480 -> dos32's own-size mode 0x17F */
    if (tp_w != 640 && (M32W(g_esp + 4) & 0x1FF) == 0x101) M32W(g_esp + 4) = (M32W(g_esp + 4) & ~0x1FFu) | 0x17F;
    lifted();
}

/* 320x200 (R, or a screen that switches): the shared code's 640x480 back. */
void lores_mode(void (*lifted)(void)) {
    tp_w = 640; tp_h = 480; tp_vh = 400;
    lifted();
}

void hires_mode(void (*lifted)(void)) {
    int w = res_w;
    tp_w = w > 640 ? w : 640; tp_h = 480; tp_vh = 400;   /* before: it sets the mouse range */
    if (tp_w != 640) dos32_vesa_custom(tp_w, tp_h);
    lifted();
    if (tp_w == 640) return;
    M32W(0x4CE5B8) = tp_w; M32W(0x4CE5C0) = tp_h;                    /* screen width, height */
    M32W(0x4CE5D0) = 0; M32W(0x4CE5E0) = 0;                          /* clip window */
    M32W(0x4CE5C8) = tp_w; M32W(0x4CE5F0) = tp_h;
    M32W(0x4CE5EC) = tp_w; M32W(0x4CE5D8) = tp_h;
}

/* The size from "WxH": the width rounded down to a multiple of 8 (the game
 * copies rows in dwords and row pairs), 640..RES_MAX_W; the height must be
 * 480. 0 for 640x480 or anything else. */
int res_parse(const char* s) {
    int w = 0, h = 0;
    if (!s || sscanf(s, "%dx%d", &w, &h) != 2 || h != 480) return 0;
    w &= ~7;
    return w > 640 && w <= RES_MAX_W ? w : 0;
}

/* The park's two back buffers are one resource, *WB_SCREEN (0x4A10FA), of
 * 0x96000 bytes (size at 0x4A1140): two 640x480 screens. Sized for the chosen
 * resolution when the game loads it. */
void resource_load(void (*lifted)(void)) {
    /* Made for the widest at the start, so a size picked later in the menu fits;
     * + a 16-row strip of slack each. */
    if (M32W(g_esp + 4) == 0x4A10FA) M32W(0x4A1140) = 2u * RES_MAX_W * (480 + 16);
    lifted();
}

/* The game shows a 640x480 screen as five VESA bank copies, unrolled: four of
 * 64 KB and a fifth of 0xB000 bytes. These take over at the fifth (bank 4 is
 * selected, edi at the window, esi after the first four banks) and copy or
 * clear the rest of tp_w x tp_h, switching banks through the game's own call
 * (0x46E6BC). At 640x480 that is the same 0xB000 bytes. */
extern uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp;
void recomp_int(int n);

/* What the game's bank switch (0x46E6BC) does, without its check: past bank
 * 5 (beyond 640x480) it turns colour 0 red, a warning to its programmers.
 * The bank, then VESA 4F05 for windows A and B (granularity at 0x4CE5FA). */
static void guest_bank(uint32_t bank) {
    uint32_t r[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp };
    uint32_t gran = M32W(0x4CE5FA) >> 16;
    *(uint16_t*)dos32_mem(0x4AECDC) = (uint16_t)bank;
    for (uint32_t win = 0; win < 2; win++) {
        g_eax = 0x4F05; g_ebx = win; g_ecx = 0; g_edx = gran ? bank * 64 / gran : bank;
        recomp_int(0x10);
    }
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esi = r[4]; g_edi = r[5]; g_ebp = r[6];
}

static void bank_tail(int copy) {
    uint32_t left = (uint32_t)tp_w * (uint32_t)tp_h - 4u * 0x10000u, bank = 4, n = 0;
    for (;;) {
        n = left < 0x10000u ? left : 0x10000u;
        if (copy) { memcpy(dos32_mem(0xA0000), dos32_mem(g_esi), n); g_esi += n; }
        else memset(dos32_mem(0xA0000), (int)(g_eax & 0xFF), n);
        left -= n;
        if (!left) break;
        guest_bank(++bank);
    }
    g_edi = 0xA0000 + n;
    g_ecx = 0;   /* the rep that follows does nothing */
}
void tp_present_tail(void) { bank_tail(1); }
void tp_clear_tail(void) { bank_tail(0); }

/* The screen update (0x444ED6) copies only the 32x16 cells its dirty grid
 * marks, and the grid covers 640x480. Wider, take its own whole-screen branch
 * every time (0x4C6BCD set): one copy of the screen, which costs nothing now. */
void screen_update(void (*lifted)(void)) {
    if (tp_w != 640) *dos32_mem(0x4C6BCD) = 1;
    lifted();
}

/* The park's mouse zones (0x4A93C3, 12 bytes each: x0, y0, x1, y1, ...):
 * 0x43DFAC sets their heights for the mode; their widths are 640 in both
 * (320x200 counts in 640x400). The edge strips, the view and the message
 * bar reach the screen's width; the panel and the buttons at the top right
 * stay where they are. Set either way, so 640 puts the game's own back. */
#define ZONE(i, f) (*(int16_t*)dos32_mem(0x4A93C3 + (i) * 12 + (f) * 2))
void park_zones(void (*lifted)(void)) {
    lifted();
    int w = tp_w;
    ZONE(0, 2) = (int16_t)w;                                   /* top edge */
    ZONE(1, 0) = (int16_t)(w - 2); ZONE(1, 2) = (int16_t)(w + 2);   /* right edge */
    ZONE(2, 2) = (int16_t)w;                                   /* bottom edge */
    ZONE(29, 2) = (int16_t)(w - 2);                            /* the park */
    ZONE(30, 2) = (int16_t)(w - 4);                            /* the message bar */
}
