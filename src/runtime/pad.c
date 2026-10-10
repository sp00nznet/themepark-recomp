/*
 * pad.c - an Xbox controller as Theme Park's mouse and keys (XInput).
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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdint.h>

#include "dos32.h"
#include "pad.h"

typedef DWORD (WINAPI *get_state_fn)(DWORD, XINPUT_STATE*);
static get_state_fn get_state;
static int tried;
static WORD prev_buttons;
static int prev_dirs;

static void key(int sc, int down) { dos32_key(sc, down); }

void pad_poll(int fw, int fh, void (*speed_step)(int up)) {
    if (!tried) {
        tried = 1;
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        if (!x) x = LoadLibraryA("xinput9_1_0.dll");
        if (x) get_state = (get_state_fn)GetProcAddress(x, "XInputGetState");
    }
    XINPUT_STATE st;
    if (!get_state || get_state(0, &st) != ERROR_SUCCESS) return;
    XINPUT_GAMEPAD* g = &st.Gamepad;
    (void)fh;

    /* Pointer: a dead zone, then speed growing with the square of the push. */
    float sx = g->sThumbLX / 32767.0f, sy = -g->sThumbLY / 32767.0f;
    float mag = sqrtf(sx * sx + sy * sy);
    WORD b = g->wButtons;
    float dx = 0, dy = 0;
    if (mag > 0.24f) {
        float k = (mag - 0.24f) / 0.76f, v = 1.0f + k * k * 9.0f * fw / 320.0f;
        dx = sx / mag * k * v; dy = sy / mag * k * v;
    }
    int buttons = (b & XINPUT_GAMEPAD_A ? 1 : 0) | (b & XINPUT_GAMEPAD_B ? 2 : 0);
    int prev_mb = (prev_buttons & XINPUT_GAMEPAD_A ? 1 : 0) | (prev_buttons & XINPUT_GAMEPAD_B ? 2 : 0);
    if (dx || dy || buttons != prev_mb) dos32_mouse_move(dx, dy, buttons);

    /* Scrolling: the D-pad or the right stick, as the arrow keys. */
    int dirs = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || g->sThumbRY > 16000) dirs |= 1;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN || g->sThumbRY < -16000) dirs |= 2;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT || g->sThumbRX < -16000) dirs |= 4;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT || g->sThumbRX > 16000) dirs |= 8;
    static const int arrows[4] = { 0xE048, 0xE050, 0xE04B, 0xE04D };
    for (int i = 0; i < 4; i++)
        if ((dirs ^ prev_dirs) & (1 << i)) key(arrows[i], (dirs >> i) & 1);
    prev_dirs = dirs;

    /* Buttons that are keys, on their press and release. */
    static const struct { WORD bit; int sc; } keys[] = {
        { XINPUT_GAMEPAD_Y, 0x13 }, { XINPUT_GAMEPAD_X, 0x39 },
        { XINPUT_GAMEPAD_START, 0x01 }, { XINPUT_GAMEPAD_BACK, 0x3B },
    };
    for (int i = 0; i < 4; i++)
        if ((b ^ prev_buttons) & keys[i].bit) key(keys[i].sc, (b & keys[i].bit) != 0);
    if ((b & ~prev_buttons) & XINPUT_GAMEPAD_LEFT_SHOULDER) speed_step(0);
    if ((b & ~prev_buttons) & XINPUT_GAMEPAD_RIGHT_SHOULDER) speed_step(1);
    prev_buttons = b;
}

