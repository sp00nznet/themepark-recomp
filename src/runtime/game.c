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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "dos32.h"

double game_fps = 25.0;   /* about what a fast 486 managed; main.c --fps */

static double next_s;

void pace_frame(void (*lifted)(void)) {
    if (game_fps <= 0) { lifted(); return; }   /* --fps 0: flat out, as on DOS */
    if (dos32_virtual_clock) { dos32_advance(1.0 / game_fps); lifted(); return; }
    static LARGE_INTEGER f, t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    double now = (double)t.QuadPart / (double)f.QuadPart;
    if (next_s - now > 1.0 || now - next_s > 0.25) next_s = now;   /* first frame, or a long stall */
    if (next_s > now) dos32_idle(next_s - now);
    next_s += 1.0 / game_fps;
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
