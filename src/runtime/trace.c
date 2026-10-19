/*
 * The trace build (CMake -DTP_TRACE=ON): which lifted functions ran between
 * two guest times. TP_TRACE=T0,T1 counts every function entry in that window
 * and prints the list, in order of first entry, when it closes. For finding
 * the code behind something seen on screen (docs/testing.md).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LO 0x400000u
#define HI 0x4E0000u
uint32_t g_enter_trace[1024];
uint32_t g_enter_idx;
int trace_on;                     /* main.c sets it inside the window */
static uint32_t* count;
static uint32_t* order;
static uint32_t norder;

void recomp_trace_enter(uint32_t va) {
    if (!trace_on || va < LO || va >= HI) return;
    if (!count) { count = calloc(HI - LO, 4); order = calloc(1u << 16, 4); }
    if (!count[va - LO]++ && norder < (1u << 16)) order[norder++] = va;
}

void trace_report(void) {
    fprintf(stderr, "[trace] %u functions\n", norder);
    for (uint32_t i = 0; i < norder; i++)
        fprintf(stderr, "[trace] %08X %u\n", order[i], count[order[i] - LO]);
}
