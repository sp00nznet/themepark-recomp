/*
 * present_3ds.c - the Nintendo 3DS: the whole picture on the bottom (touch)
 * screen, where the stylus is the mouse, and the top screen a 1:1 close-up of
 * wherever the stylus last was. Straight to libctru's framebuffers, no SDL.
 *
 *   stylus        move and left-click (hold L or R: right-click)
 *   d-pad         arrow keys (scroll the park)
 *   A / B / Y     Enter / Esc / space
 *   X             the on-screen keyboard (your name)
 *   START         Esc     SELECT + START  quit
 */
#ifdef __3DS__
#include <3ds.h>
#include <ctype.h>
#include <string.h>
#include "compat.h"
#include "dos32.h"
#include "present.h"
#include "rules.h"

present_settings g_present = { SCALE_NEAREST, 0, 0, 1, 0 };
static uint32_t frame[1024 * 768];
static int fw = 320, fh = 200, gx, gy;   /* the frame's size; the pointer, in game pixels */
static volatile int closing;

int present_scale_from_name(const char* name) { (void)name; return -1; }
void present_load_settings(void) {}
int present_test(const char* dir) { (void)dir; return 0; }
void present_close(void) { closing = 1; }
/* --record pipes to ffmpeg; there are no processes here. */
FILE* popen(const char* cmd, const char* mode) { (void)cmd; (void)mode; return NULL; }
int pclose(FILE* f) { (void)f; return -1; }

/* The framebuffers are BGR8 and turned a quarter: column-major, bottom row first. */
static inline void put(u8* fb, int x, int y, uint32_t c) {
    u8* p = fb + (x * 240 + 239 - y) * 3;
    p[0] = (u8)c; p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16);
}

/* The whole frame into 320x240 (4:3 either way: 320x200 stretches as on a CRT).
 * 640x480 averages each 2x2; anything else is nearest. */
static void draw_bottom(void) {
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    for (int y = 0; y < 240; y++) {
        int sy = y * fh / 240;
        for (int x = 0; x < 320; x++) {
            uint32_t c;
            if (fw == 640 && fh == 480) {
                const uint32_t* s = frame + y * 2 * 640 + x * 2;
                uint32_t a = s[0], b = s[1], d = s[640], e = s[641];
                c = ((((a & 0xFF00FF) + (b & 0xFF00FF) + (d & 0xFF00FF) + (e & 0xFF00FF)) >> 2) & 0xFF00FF) |
                    ((((a & 0x00FF00) + (b & 0x00FF00) + (d & 0x00FF00) + (e & 0x00FF00)) >> 2) & 0x00FF00);
            } else c = frame[sy * fw + x * fw / 320];
            put(fb, x, y, c);
        }
    }
}

/* 400x240 of the frame at 1:1, around the pointer. */
static void draw_top(void) {
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    int ox = fw <= 400 ? (fw - 400) / 2 : gx - 200, oy = fh <= 240 ? (fh - 240) / 2 : gy - 120;
    if (fw > 400) ox = ox < 0 ? 0 : ox > fw - 400 ? fw - 400 : ox;
    if (fh > 240) oy = oy < 0 ? 0 : oy > fh - 240 ? fh - 240 : oy;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 400; x++) {
            int sx = x + ox, sy = y + oy;
            put(fb, x, y, sx >= 0 && sx < fw && sy >= 0 && sy < fh ? frame[sy * fw + sx] : 0);
        }
}

static void key_tap(int sc) { dos32_key(sc, 1); svcSleepThread(60000000); dos32_key(sc, 0); }

/* Set-1 scancodes for what the keyboard can give a name. */
static int scancode_of(char c) {
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    const char* p;
    c = (char)tolower((unsigned char)c);
    if ((p = strchr(row1, c)) && c) return 0x10 + (int)(p - row1);
    if ((p = strchr(row2, c)) && c) return 0x1E + (int)(p - row2);
    if ((p = strchr(row3, c)) && c) return 0x2C + (int)(p - row3);
    if (c >= '1' && c <= '9') return 0x02 + c - '1';
    if (c == '0') return 0x0B;
    if (c == ' ') return 0x39;
    return 0;
}

static void type_text(void) {
    SwkbdState kb;
    char text[32] = { 0 };
    swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, (int)sizeof text - 1);
    swkbdSetHintText(&kb, "Your name");
    if (swkbdInputText(&kb, text, sizeof text) != SWKBD_BUTTON_CONFIRM) return;
    for (char* c = text; *c; c++) {
        int sc = scancode_of(*c);
        if (!sc) continue;
        if (isupper((unsigned char)*c)) dos32_key(0x2A, 1);
        key_tap(sc);
        if (isupper((unsigned char)*c)) dos32_key(0x2A, 0);
    }
}

void present_run(void) {
    static const struct { u32 bit; int sc; } keys[] = {
        { KEY_DUP, 0x48 }, { KEY_DDOWN, 0x50 }, { KEY_DLEFT, 0x4B }, { KEY_DRIGHT, 0x4D },
        { KEY_A, 0x1C }, { KEY_B, 0x01 }, { KEY_Y, 0x39 }, { KEY_START, 0x01 },
    };
    gfxInitDefault();
    while (!closing && aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), up = hidKeysUp(), held = hidKeysHeld();
        if ((held & KEY_SELECT) && (held & KEY_START)) break;
        if (down & KEY_X) type_text();
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            if (down & keys[i].bit) dos32_key(keys[i].sc, 1);
            if (up & keys[i].bit) dos32_key(keys[i].sc, 0);
        }
        if (held & KEY_TOUCH) {
            touchPosition t;
            hidTouchRead(&t);
            gx = t.px * fw / 320; gy = t.py * fh / 240;
            dos32_mouse(gx, gy, (held & (KEY_L | KEY_R)) ? 2 : 1);
        } else if (up & KEY_TOUCH) dos32_mouse(gx, gy, 0);
        int w, h;
        if (dos32_frame(frame, 1024, 768, &w, &h)) {
            if (w != fw || h != fh) { gx = gx * w / fw; gy = gy * h / fh; fw = w; fh = h; }
            lights_apply(frame, w, h);
        }
        draw_bottom();
        draw_top();
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    dos32_quit = 1;
    gfxExit();
}
#endif
