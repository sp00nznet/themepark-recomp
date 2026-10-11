/*
 * pad.c - an Xbox controller as Theme Park's mouse and keys (XInput; on
 * Linux, any pad SDL knows as a GameController).
 *
 *   left stick        the pointer (faster the further it is pushed)
 *   A / B             left / right mouse button
 *   D-pad, right stick  scroll the park (the arrow keys)
 *   LB / RB           game speed down / up
 *   Y                 R: switch 320x200 / 640x480 in a park
 *   X                 Space    Start  Esc    Back  F1
 *
 * XInput is loaded at run time (xinput1_4, else 9_1_0), so a machine without
 * it simply has no pad. Polled from the window's loop; a still pad sends
 * nothing, so the real mouse keeps working beside it.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#else
#include <SDL.h>
#endif
#include <math.h>
#include <stdint.h>

#include "dos32.h"
#include "pad.h"

/* The pad as both systems give it: sticks -1..1 (y down), XInput's button bits. */
enum { B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_START = 0x10, B_BACK = 0x20,
       B_LB = 0x100, B_RB = 0x200, B_A = 0x1000, B_B = 0x2000, B_X = 0x4000, B_Y = 0x8000 };
typedef struct { float lx, ly, rx, ry; unsigned b; } pad_t;

#ifdef _WIN32
typedef DWORD (WINAPI *get_state_fn)(DWORD, XINPUT_STATE*);
static get_state_fn get_state;
static int tried;

static int read_pad(pad_t* p) {
    if (!tried) {
        tried = 1;
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        if (!x) x = LoadLibraryA("xinput9_1_0.dll");
        if (x) get_state = (get_state_fn)GetProcAddress(x, "XInputGetState");
    }
    XINPUT_STATE st;
    if (!get_state || get_state(0, &st) != ERROR_SUCCESS) return 0;
    XINPUT_GAMEPAD* g = &st.Gamepad;
    p->lx = g->sThumbLX / 32767.0f; p->ly = -g->sThumbLY / 32767.0f;
    p->rx = g->sThumbRX / 32767.0f; p->ry = -g->sThumbRY / 32767.0f;
    p->b = g->wButtons;   /* the same bits */
    return 1;
}
#else
static SDL_GameController* gc;

static int read_pad(pad_t* p) {
    if (!gc) {
        static int tried;
        if (!tried++) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        for (int i = 0; !gc && i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i)) gc = SDL_GameControllerOpen(i);
        if (!gc) return 0;
    }
    if (!SDL_GameControllerGetAttached(gc)) { SDL_GameControllerClose(gc); gc = NULL; return 0; }
    SDL_GameControllerUpdate();
    p->lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    p->ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    p->rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
    p->ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
    static const struct { SDL_GameControllerButton b; unsigned bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT },
        { SDL_CONTROLLER_BUTTON_START, B_START }, { SDL_CONTROLLER_BUTTON_BACK, B_BACK },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_RB },
        { SDL_CONTROLLER_BUTTON_A, B_A }, { SDL_CONTROLLER_BUTTON_B, B_B },
        { SDL_CONTROLLER_BUTTON_X, B_X }, { SDL_CONTROLLER_BUTTON_Y, B_Y },
    };
    p->b = 0;
    for (int i = 0; i < (int)(sizeof map / sizeof map[0]); i++)
        if (SDL_GameControllerGetButton(gc, map[i].b)) p->b |= map[i].bit;
    return 1;
}
#endif

static unsigned prev_buttons;
static int prev_dirs;

static void key(int sc, int down) { dos32_key(sc, down); }

void pad_poll(int fw, int fh, void (*speed_step)(int up)) {
    pad_t g;
    if (!read_pad(&g)) return;
    (void)fh;

    /* Pointer: a dead zone, then speed growing with the square of the push. */
    float mag = sqrtf(g.lx * g.lx + g.ly * g.ly);
    unsigned b = g.b;
    float dx = 0, dy = 0;
    if (mag > 0.24f) {
        float k = (mag - 0.24f) / 0.76f, v = 1.0f + k * k * 9.0f * fw / 320.0f;
        dx = g.lx / mag * k * v; dy = g.ly / mag * k * v;
    }
    int buttons = (b & B_A ? 1 : 0) | (b & B_B ? 2 : 0);
    int prev_mb = (prev_buttons & B_A ? 1 : 0) | (prev_buttons & B_B ? 2 : 0);
    if (dx || dy || buttons != prev_mb) dos32_mouse_move(dx, dy, buttons);

    /* Scrolling: the D-pad or the right stick, as the arrow keys. */
    int dirs = 0;
    if (b & B_UP || g.ry < -0.49f) dirs |= 1;
    if (b & B_DOWN || g.ry > 0.49f) dirs |= 2;
    if (b & B_LEFT || g.rx < -0.49f) dirs |= 4;
    if (b & B_RIGHT || g.rx > 0.49f) dirs |= 8;
    static const int arrows[4] = { 0xE048, 0xE050, 0xE04B, 0xE04D };
    for (int i = 0; i < 4; i++)
        if ((dirs ^ prev_dirs) & (1 << i)) key(arrows[i], (dirs >> i) & 1);
    prev_dirs = dirs;

    /* Buttons that are keys, on their press and release. */
    static const struct { unsigned bit; int sc; } keys[] = {
        { B_Y, 0x13 }, { B_X, 0x39 }, { B_START, 0x01 }, { B_BACK, 0x3B },
    };
    for (int i = 0; i < 4; i++)
        if ((b ^ prev_buttons) & keys[i].bit) key(keys[i].sc, (b & keys[i].bit) != 0);
    if ((b & ~prev_buttons) & B_LB) speed_step(0);
    if ((b & ~prev_buttons) & B_RB) speed_step(1);
    prev_buttons = b;
}
