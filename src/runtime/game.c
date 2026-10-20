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

/* Hold the caller until the next frame is due. */
static void pace_wait(void) {
    if (game_fps <= 0) return;   /* --fps 0: flat out, as on DOS */
    if (dos32_virtual_clock) { dos32_advance(1.0 / game_fps); return; }
    static LARGE_INTEGER f, t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    double now = (double)t.QuadPart / (double)f.QuadPart;
    if (next_s - now > 1.0 || now - next_s > 0.25) next_s = now;   /* first frame, or a long stall */
    if (next_s > now) dos32_idle(next_s - now);
    next_s += 1.0 / game_fps;
}

static int polled;   /* the input poll ran since the last present */

void pace_frame(void (*lifted)(void)) {
    int req = state_request;
    if (req) { state_request = 0; do_state(req); }
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
 * every loop runs at the frame rate and none is paced twice. */
void pace_present(void (*lifted)(void)) {
    if (!polled) pace_wait();
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
extern uint32_t g_cur_func;
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

/* ---- custom resolutions (#11) ------------------------------------------ */
extern uint32_t g_esp;
#define M32W(a) (*(uint32_t*)dos32_mem(a))
int wide_w = 0, wide_h = 0;    /* 0: the game's 640x480 */
int32_t tp_w = 640, tp_h = 480, tp_vh = 400;   /* the park view; lifted code reads them */

void vesa_set(void (*lifted)(void)) {
    if (wide_w && (M32W(g_esp + 4) & 0x1FF) == 0x101) M32W(g_esp + 4) = 0x103;
    lifted();
}

void hires_mode(void (*lifted)(void)) {
    lifted();
    if (!wide_w) return;
    tp_w = wide_w; tp_h = wide_h; tp_vh = wide_h - 80;
    M32W(0x4CE5B8) = wide_w; M32W(0x4CE5C0) = wide_h;               /* screen width, height */
    M32W(0x4CE5D0) = 0; M32W(0x4CE5E0) = 0;                          /* clip window */
    M32W(0x4CE5C8) = wide_w; M32W(0x4CE5F0) = wide_h;
    M32W(0x4CE5EC) = wide_w; M32W(0x4CE5D8) = wide_h;
}
