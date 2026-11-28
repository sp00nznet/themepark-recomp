/*
 * pad.c - an Xbox controller as Theme Park's mouse and keys (read by
 * pcrecomp's presenter: XInput, or on Linux any pad SDL knows as a
 * GameController).
 *
 *   left stick        the pointer (faster the further it is pushed)
 *   A / B             left / right mouse button
 *   D-pad, right stick  scroll the park (the arrow keys)
 *   LB / RB           game speed down / up
 *   Y                 R: switch 320x200 / 640x480 in a park
 *   X                 Space    Start  Esc    Back  F1
 *
 * Polled from the window's loop; a still pad sends nothing, so the real mouse
 * keeps working beside it. Unplugged, or with the window in the background,
 * the pad reads as released: what it held is let go.
 */
#include <math.h>
#include <stdint.h>

#include "dos32.h"
#include "pad.h"
#include "present.h"

static unsigned prev_buttons;
static int prev_dirs;

static void key(int sc, int down) { dos32_key(sc, down); }

void pad_poll(int fw, int fh, void (*speed_step)(int up)) {
    present_pad g;
    if (!present_pad_read(&g) && !prev_buttons && !prev_dirs) return;
    (void)fh;

    /* Pointer: a dead zone, then speed growing with the square of the push. */
    float mag = sqrtf(g.lx * g.lx + g.ly * g.ly);
    unsigned b = g.buttons;
    float dx = 0, dy = 0;
    if (mag > 0.24f) {
        float k = (mag - 0.24f) / 0.76f, v = 1.0f + k * k * 9.0f * fw / 320.0f;
        dx = g.lx / mag * k * v; dy = g.ly / mag * k * v;
    }
    int buttons = (b & PRESENT_PAD_A ? 1 : 0) | (b & PRESENT_PAD_B ? 2 : 0);
    int prev_mb = (prev_buttons & PRESENT_PAD_A ? 1 : 0) | (prev_buttons & PRESENT_PAD_B ? 2 : 0);
    if (dx || dy || buttons != prev_mb) dos32_mouse_move(dx, dy, buttons);

    /* Scrolling: the D-pad or the right stick, as the arrow keys. */
    int dirs = 0;
    if (b & PRESENT_PAD_UP || g.ry < -0.49f) dirs |= 1;
    if (b & PRESENT_PAD_DOWN || g.ry > 0.49f) dirs |= 2;
    if (b & PRESENT_PAD_LEFT || g.rx < -0.49f) dirs |= 4;
    if (b & PRESENT_PAD_RIGHT || g.rx > 0.49f) dirs |= 8;
    static const int arrows[4] = { 0xE048, 0xE050, 0xE04B, 0xE04D };
    for (int i = 0; i < 4; i++)
        if ((dirs ^ prev_dirs) & (1 << i)) key(arrows[i], (dirs >> i) & 1);
    prev_dirs = dirs;

    /* Buttons that are keys, on their press and release. */
    static const struct { unsigned bit; int sc; } keys[] = {
        { PRESENT_PAD_Y, 0x13 }, { PRESENT_PAD_X, 0x39 }, { PRESENT_PAD_START, 0x01 }, { PRESENT_PAD_BACK, 0x3B },
    };
    for (int i = 0; i < 4; i++)
        if ((b ^ prev_buttons) & keys[i].bit) key(keys[i].sc, (b & keys[i].bit) != 0);
    if ((b & ~prev_buttons) & PRESENT_PAD_LB) speed_step(0);
    if ((b & ~prev_buttons) & PRESENT_PAD_RB) speed_step(1);
    prev_buttons = b;
}
