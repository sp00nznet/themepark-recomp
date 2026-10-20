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

/* The park's two back buffers are one resource, *WB_SCREEN (0x4A10FA), of
 * 0x96000 bytes (size at 0x4A1140): two 640x480 screens. Sized for the chosen
 * resolution when the game loads it. */
void resource_load(void (*lifted)(void)) {
    if (wide_w && M32W(g_esp + 4) == 0x4A10FA) M32W(0x4A1140) = 2u * wide_w * (wide_h + 16);   /* + a 16-row strip of slack */
    lifted();
}

/* The game shows a 640x480 screen as five VESA bank copies, unrolled: four of
 * 64 KB and a fifth of 0xB000 bytes. These take over at the fifth (bank 4 is
 * selected, edi at the window, esi after the first four banks) and copy or
 * clear the rest of tp_w x tp_h, switching banks through the game's own call
 * (0x46E6BC). At 640x480 that is the same 0xB000 bytes. */
extern uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
void sub_0046E6BC(void);

static void guest_bank(uint32_t bank) {
    uint32_t r[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp }, cur = g_cur_func;
    g_esp -= 4; M32W(g_esp) = bank;
    g_esp -= 4; M32W(g_esp) = 0xDEAD0000u;   /* RECOMP_RETADDR: its ret pops this */
    sub_0046E6BC();
    g_esp += 4;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esi = r[4]; g_edi = r[5]; g_ebp = r[6];
    g_cur_func = cur;
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
    if (wide_w) *dos32_mem(0x4C6BCD) = 1;
    lifted();
}
