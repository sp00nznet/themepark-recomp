/* The window: shows dos32's screen and feeds it keys and the mouse (present.c). */
#pragma once
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

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

/* Close the window from another thread (the game exited). */
void present_close(void);

#ifdef _WIN32
/* present_d3d.c: scale w x h pixels (0x00RRGGBB) into rect r of a cw x ch
 * client area. Returns 0 if Direct3D is unavailable (use GDI). */
int d3d_present(HWND hw, const uint32_t* px, int w, int h, const RECT* r, int cw, int ch,
                int mode, int look, int vsync);
#endif
