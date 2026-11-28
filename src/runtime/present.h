/* The window: pcrecomp's runtime/present on Windows, Linux, macOS and the
 * browser (present.c is what Theme Park adds to it); present_3ds.c on the 3DS. */
#pragma once
#ifndef __3DS__
#include "present/present.h"
#else
#include <stdint.h>

enum { SCALE_SHARP, SCALE_SMOOTH, SCALE_CRT, SCALE_NEAREST, SCALE_INTEGER, SCALE_COUNT };

/* What themepark.ini (beside the exe) remembers; the command line overrides it. */
typedef struct {
    int scale;        /* SCALE_* */
    int look;         /* 0 natural, 1 vivid */
    int fullscreen;
    int vsync;
    int gdi;          /* 1: skip Direct3D */
} present_settings;
extern present_settings g_present;

void present_load_settings(void);
int  present_scale_from_name(const char* name);   /* -1 if unknown */

/* Open the window and run its loop until it closes. */
void present_run(void);

/* Render the current screen through every scaling mode into DIR/<mode>.bmp
 * with Direct3D and no visible window. Returns the number that failed. */
int present_test(const char* dir);

/* Close the window from another thread (the game exited). */
void present_close(void);
#endif
