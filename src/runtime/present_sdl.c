/*
 * present_sdl.c - the window on Linux: present.c's job with SDL2. The same
 * keys, settings and themepark.ini:
 *
 *   F12              scaling: integer (default: pixels doubled up), sharp-bilinear, smooth, CRT, nearest
 *   F9               colour: natural / vivid
 *   Ctrl+PgUp/PgDn   speed: the game's frames per second, 12 to 120 (default 25)
 *   F11, Alt+Enter   fullscreen (the desktop's resolution)
 *   F10, right-click on the bars   the settings menu, as a row of dialogs
 *
 * The mouse is captured by a click in the picture (SDL's relative mode) and
 * freed by F10 or leaving the window: the game reads how far the mouse moved,
 * not where it is (present.c, capture()). Scaling is SDL's renderer: nearest
 * or linear, sharp-bilinear as a whole-multiple prescale then linear, CRT as a
 * multiply overlay of scanlines and an aperture grille on that, the formulas
 * of present_d3d.c's shader.
 */
#include <SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "compat.h"
#include "dos32.h"
#include "present.h"
#include "pad.h"

present_settings g_present = { SCALE_INTEGER, 0, 0, 1, 0 };

static const char* scale_names[SCALE_COUNT] = { "sharp", "smooth", "crt", "nearest", "integer" };
static const char* scale_labels[SCALE_COUNT] = { "Sharp", "Smooth", "CRT", "Nearest", "Integer" };
static char ini_path[MAX_PATH];
static SDL_Window* wnd;
static SDL_Renderer* ren;
static uint32_t frame[1024 * 768], vivid_buf[1024 * 768];
static int fw = 320, fh = 200;
static SDL_Rect dst;             /* where the picture lands in the window */
static int cw, ch;
static Uint32 title_until, hint_until;
extern double game_fps;          /* game.c: the speed control */
static const int speeds[] = { 12, 15, 20, 25, 30, 35, 40, 50, 60, 90, 120 };
static int captured, buttons, muted;
static volatile int closing;
extern volatile int state_request;              /* game.c: save states */
extern char state_note[];
extern volatile DWORD state_note_until;
void state_path(int slot, char* out, size_t n);
void sound_set_mute(int on);                    /* sound.c */

int present_scale_from_name(const char* name) {
    for (int i = 0; i < SCALE_COUNT; i++) if (!_stricmp(name, scale_names[i])) return i;
    return -1;
}

void present_load_settings(void) {
    char buf[64];
    exe_dir(ini_path, sizeof ini_path);
    strncat(ini_path, "themepark.ini", sizeof ini_path - strlen(ini_path) - 1);
    GetPrivateProfileStringA("display", "scale", "integer", buf, sizeof buf, ini_path);
    int s = present_scale_from_name(buf);
    g_present.scale = s < 0 ? SCALE_INTEGER : s;
    g_present.look = GetPrivateProfileIntA("display", "vivid", 0, ini_path);
    g_present.fullscreen = GetPrivateProfileIntA("display", "fullscreen", 0, ini_path);
    g_present.vsync = GetPrivateProfileIntA("display", "vsync", 1, ini_path);
}

static void save_settings(void) {
    WritePrivateProfileStringA("display", "scale", scale_names[g_present.scale], ini_path);
    WritePrivateProfileStringA("display", "vivid", g_present.look ? "1" : "0", ini_path);
    WritePrivateProfileStringA("display", "fullscreen", g_present.fullscreen ? "1" : "0", ini_path);
}

static void layout(void) {
    int w, h;
    if (g_present.scale == SCALE_INTEGER) {
        int k = cw / fw < ch / fh ? cw / fw : ch / fh;
        if (k < 1) k = 1;
        w = fw * k; h = fh * k;
    } else {                     /* 4:3, as present.c */
        w = cw; h = cw * 3 / 4;
        if (h > ch) { h = ch; w = ch * 4 / 3; }
    }
    dst.x = (cw - w) / 2; dst.y = (ch - h) / 2; dst.w = w; dst.h = h;
}

static void set_title(void) {
    static char shown[192];
    char t[192];
    Uint32 now = SDL_GetTicks();
    if (GetTickCount() < state_note_until) snprintf(t, sizeof t, "Theme Park - %s", state_note);
    else if (now < title_until)
        snprintf(t, sizeof t, "Theme Park - %s%s - speed %.0f fps", scale_names[g_present.scale],
                 g_present.look ? ", vivid" : "", game_fps);
    else if (!captured) snprintf(t, sizeof t, "Theme Park - click to play");
    else if (now < hint_until) snprintf(t, sizeof t, "Theme Park - F10 frees the mouse");
    else snprintf(t, sizeof t, "Theme Park");
    if (strcmp(t, shown)) { SDL_SetWindowTitle(wnd, t); strcpy(shown, t); }
}

static void apply_fullscreen(int on) {
    SDL_SetWindowFullscreen(wnd, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    g_present.fullscreen = on;
}

static void capture(int on) {
    if (on == captured) return;
    captured = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    if (on) hint_until = SDL_GetTicks() + 4000;
    else { buttons = 0; dos32_mouse_move(0, 0, 0); }
    title_until = 0;
}

/* ---- drawing --------------------------------------------------------------- */
static SDL_Texture *tex, *mid, *crt;
static int tw, th, mw, mh, crt_w, crt_h, crt_sh;

static SDL_Texture* sized(SDL_Renderer* r, SDL_Texture** t, int* w0, int* h0, int w, int h, int access) {
    if (!*t || *w0 != w || *h0 != h) {
        if (*t) SDL_DestroyTexture(*t);
        *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB888, access, w, h);
        SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_NONE);
        *w0 = w; *h0 = h;
    }
    return *t;
}

/* present_d3d.c's vivid: 1.3x saturation, a little contrast. */
static const uint32_t* vivid(const uint32_t* f, int n) {
    for (int i = 0; i < n; i++) {
        float c[3] = { (f[i] >> 16 & 255) / 255.0f, (f[i] >> 8 & 255) / 255.0f, (f[i] & 255) / 255.0f };
        float l = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
        uint32_t o = 0xFF000000u;
        for (int k = 0; k < 3; k++) {
            float v = ((l + (c[k] - l) * 1.3f) - 0.5f) * 1.08f + 0.5f;
            o |= (uint32_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255.0f + 0.5f) << (16 - 8 * k);
        }
        vivid_buf[i] = o;
    }
    return vivid_buf;
}

/* CRT: present_d3d.c's beam and mask as a multiply overlay over the picture,
 * halved so the blend's 2 x src x dst can brighten as the shader does. */
static void crt_overlay(SDL_Renderer* r) {
    if (!crt || crt_w != dst.w || crt_h != dst.h || crt_sh != fh) {
        if (crt) SDL_DestroyTexture(crt);
        crt = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, dst.w, dst.h);
        crt_w = dst.w; crt_h = dst.h; crt_sh = fh;
        SDL_BlendMode twice = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_SRC_COLOR,
                                                         SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
                                                         SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        float scale = SDL_SetTextureBlendMode(crt, twice) == 0 ? 0.5f : 1.0f;
        if (scale == 1.0f) SDL_SetTextureBlendMode(crt, SDL_BLENDMODE_MOD);   /* darken only */
        float k = ((float)dst.h / fh - 1.5f) / 1.5f;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        uint32_t* px = malloc((size_t)dst.w * dst.h * 4);
        for (int y = 0; px && y < dst.h; y++) {
            float sy = (y + 0.5f) * fh / dst.h, d = sy - floorf(sy) - 0.5f;
            float beam = 1.0f + (expf(-d * d * 8.0f) * 1.6f - 1.0f) * k;
            for (int x = 0; x < dst.w; x++) {
                uint32_t o = 0xFF000000u;
                for (int c = 0; c < 3; c++) {
                    float m = 1.0f + ((x + dst.x) % 3 == c ? 0.25f : -0.13f) * k;
                    float v = beam * m * scale;
                    o |= (uint32_t)(v > 1 ? 255 : v * 255.0f + 0.5f) << (16 - 8 * c);
                }
                px[y * dst.w + x] = o;
            }
        }
        if (px) SDL_UpdateTexture(crt, NULL, px, dst.w * 4);
        free(px);
    }
    SDL_RenderCopy(r, crt, NULL, &dst);
}

static void draw(SDL_Renderer* r, int mode, int look) {
    SDL_Texture* t = sized(r, &tex, &tw, &th, fw, fh, SDL_TEXTUREACCESS_STREAMING);
    SDL_UpdateTexture(t, NULL, look ? vivid(frame, fw * fh) : frame, fw * 4);
    if (mode == SCALE_SHARP || mode == SCALE_CRT) {   /* whole multiple, nearest; then linear the rest */
        int kx = dst.w / fw > 1 ? dst.w / fw : 1, ky = dst.h / fh > 1 ? dst.h / fh : 1;
        SDL_Texture* m = sized(r, &mid, &mw, &mh, fw * kx, fh * ky, SDL_TEXTUREACCESS_TARGET);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
        SDL_SetRenderTarget(r, m);
        SDL_RenderCopy(r, t, NULL, NULL);
        SDL_SetRenderTarget(r, NULL);
        SDL_SetTextureScaleMode(m, SDL_ScaleModeLinear);
        t = m;
    } else SDL_SetTextureScaleMode(t, mode == SCALE_SMOOTH ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_RenderCopy(r, t, NULL, &dst);
    if (mode == SCALE_CRT) crt_overlay(r);
}

/* ---- the settings ----------------------------------------------------------- */
static void set_speed(int fps) {
    char v[16];
    game_fps = fps;
    snprintf(v, sizeof v, "%d", fps);
    WritePrivateProfileStringA("game", "fps", v, ini_path);
    title_until = SDL_GetTicks() + 2000;
}

static void speed_step(int up) {
    int n = sizeof speeds / sizeof speeds[0], k = 0;
    while (k < n - 1 && speeds[k] < game_fps) k++;
    k = up ? (k + 1 < n ? k + 1 : k) : (k > 0 ? k - 1 : 0);
    set_speed(speeds[k]);
}

/* Window sizes in 320x240 steps (k), as present.c: the even ones are whole
 * multiples of the park's 640x480. */
static int max_window_k(void) {
    SDL_Rect wa;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(wnd), &wa)) return 2;
    SDL_GetWindowBordersSize(wnd, &top, &left, &bottom, &right);
    int k = 8;
    for (; k > 1; k--)
        if (320 * k + left + right <= wa.w && 240 * k + top + bottom <= wa.h) break;
    return k;
}

static void size_window(int k) {
    if (g_present.fullscreen) apply_fullscreen(0);
    SDL_RestoreWindow(wnd);
    SDL_SetWindowSize(wnd, 320 * k, 240 * k);
    SDL_SetWindowPosition(wnd, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(wnd)),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(wnd)));
}

static int slot_label(int slot, char* out, size_t n) {
    char path[MAX_PATH];
    struct stat st;
    state_path(slot, path, sizeof path);
    if (stat(path, &st)) { snprintf(out, n, "Slot %d   (empty)", slot); return 0; }
    struct tm* t = localtime(&st.st_mtime);
    snprintf(out, n, "Slot %d   %04d-%02d-%02d %02d:%02d", slot, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min);
    return 1;
}

/* A dialog: text and a row of buttons. Returns the button's index, -1 for
 * none (Esc, or the window closed). */
static int ask(const char* text, const char* const* labels, int n) {
    SDL_MessageBoxButtonData b[16];
    for (int i = 0; i < n && i < 16; i++) { b[i].flags = 0; b[i].buttonid = i; b[i].text = labels[i]; }
    b[n - 1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;   /* the last is always Close or Cancel */
    const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, wnd,
                                   "Theme Park settings", text, n, b, NULL };
    int id = -1;
    if (SDL_ShowMessageBox(&d, &id) < 0) return -1;
    return id == n - 1 ? -1 : id;
}

static void state_menu(int load) {
    char text[640] = "", lines[9][64];
    const char* labels[10];
    int slots[10], n = 0;
    for (int s = 1; s <= 9; s++) {
        int used = slot_label(s, lines[s - 1], sizeof lines[0]);
        strncat(text, lines[s - 1], sizeof text - strlen(text) - 2);
        strcat(text, "\n");
        if (load && !used) continue;
        static const char* digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
        labels[n] = digits[s - 1]; slots[n++] = s;
    }
    if (load && !n) { const char* ok[] = { "Close" }; ask("No saved states yet.", ok, 1); return; }
    labels[n++] = "Cancel";
    int c = ask(text, labels, n);
    if (c >= 0) state_request = load ? -slots[c] : slots[c];
}

/* F10, or a right-click on the bars. */
static void settings_menu(void) {
    char text[512], ini_hires[8], sizes[4][32];
    capture(0);
    GetPrivateProfileStringA("game", "hires", "1", ini_hires, sizeof ini_hires, ini_path);
    int hires = ini_hires[0] != '0';
    snprintf(text, sizeof text,
             "Scaling: %s\nColour: %s\nSpeed: %.0f fps\nSound: %s\nParks open in 640x480: %s\n\n"
             "F12 scaling, F9 colour, F11 fullscreen, Ctrl+PgUp/PgDn speed, R 640x480 in a park",
             scale_labels[g_present.scale], g_present.look ? "vivid" : "natural", game_fps,
             muted ? "muted" : "on", hires ? "yes" : "no");
    const char* top[] = { "Save state", "Load state", "Window size", "Scaling", "Speed", "More", "Close" };
    switch (ask(text, top, 7)) {
    case 0: state_menu(0); return;
    case 1: state_menu(1); return;
    case 2: {
        const char* l[6];
        int n = 0, ks[4];
        for (int k = 2; k <= max_window_k() && n < 4; k += 2) {
            snprintf(sizes[n], sizeof sizes[0], "%dx  %d x %d", k / 2, 320 * k, 240 * k);
            l[n] = sizes[n]; ks[n++] = k;
        }
        l[n] = g_present.fullscreen ? "Windowed" : "Fullscreen";
        l[n + 1] = "Cancel";
        int c = ask("Window size", l, n + 2);
        if (c >= 0 && c < n) {
            char v[8];
            size_window(ks[c]);
            snprintf(v, sizeof v, "%d", ks[c]);
            WritePrivateProfileStringA("display", "window", v, ini_path);
        } else if (c == n) apply_fullscreen(!g_present.fullscreen);
        break; }
    case 3: {
        const char* l[SCALE_COUNT + 1];
        for (int i = 0; i < SCALE_COUNT; i++) l[i] = scale_labels[i];
        l[SCALE_COUNT] = "Cancel";
        int c = ask("Scaling. Integer doubles the pixels up; sharp keeps them crisp at any size.", l, SCALE_COUNT + 1);
        if (c >= 0) { g_present.scale = c; layout(); }
        break; }
    case 4: {
        static char t[11][8];
        const char* l[12];
        int n = sizeof speeds / sizeof speeds[0];
        for (int k = 0; k < n; k++) { snprintf(t[k], sizeof t[0], "%d", speeds[k]); l[k] = t[k]; }
        l[n] = "Cancel";
        int c = ask("The game's speed in frames per second (25 is the original's).", l, n + 1);
        if (c >= 0) set_speed(speeds[c]);
        break; }
    case 5: {
        const char* l[] = { g_present.look ? "Natural colour" : "Vivid colour", muted ? "Sound on" : "Mute",
                            hires ? "Parks in 320x200" : "Parks in 640x480", "Cancel" };
        int c = ask("Colour, sound, and the park's resolution when one opens (R switches in a park).", l, 4);
        if (c == 0) g_present.look ^= 1;
        else if (c == 1) { muted ^= 1; sound_set_mute(muted); WritePrivateProfileStringA("sound", "mute", muted ? "1" : "0", ini_path); }
        else if (c == 2) WritePrivateProfileStringA("game", "hires", hires ? "0" : "1", ini_path);
        break; }
    default: return;
    }
    save_settings();
    title_until = SDL_GetTicks() + 2000;
}

/* ---- input ------------------------------------------------------------------ */
/* SDL scancodes (USB HID usages) to PC set-1 make codes; 0xE0xx extended. */
static int pc_scancode(SDL_Scancode s) {
    static const uint16_t hid[0x66] = {
        [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31,
        0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,          /* a-z */
        [0x1E] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,          /* 1-0 */
        [0x28] = 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29,
        0x33, 0x34, 0x35, 0x3A,                                                      /* Enter..Caps */
        [0x3A] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,   /* F1-F12 */
        [0x46] = 0xE037, 0x46, 0,
        [0x49] = 0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051, 0xE04D, 0xE04B, 0xE050, 0xE048,
        [0x53] = 0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47,
        0x48, 0x49, 0x52, 0x53, 0x56,                                                /* keypad */
    };
    switch (s) {
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_RCTRL: return 0xE01D;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_RALT: return 0xE038;
    default: return (unsigned)s < 0x66 ? hid[s] : 0;
    }
}

static int in_dst(int x, int y) { return x >= dst.x && y >= dst.y && x < dst.x + dst.w && y < dst.y + dst.h; }

static void event(const SDL_Event* e) {
    switch (e->type) {
    case SDL_QUIT: closing = 1; break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) capture(0);
        break;
    case SDL_KEYDOWN: {
        SDL_Keycode k = e->key.keysym.sym;
        int mod = e->key.keysym.mod;
        if ((k == SDLK_RETURN && (mod & KMOD_ALT)) || k == SDLK_F11) {
            if (!e->key.repeat) { apply_fullscreen(!g_present.fullscreen); save_settings(); }
            return;
        }
        if (k == SDLK_F10) { if (!e->key.repeat) settings_menu(); return; }
        if (k == SDLK_F12) {
            if (!e->key.repeat) { g_present.scale = (g_present.scale + 1) % SCALE_COUNT; layout(); save_settings(); title_until = SDL_GetTicks() + 2000; }
            return;
        }
        if (k == SDLK_F9) { if (!e->key.repeat) { g_present.look ^= 1; save_settings(); title_until = SDL_GetTicks() + 2000; } return; }
        if ((k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) && (mod & KMOD_CTRL)) { speed_step(k == SDLK_PAGEUP); return; }
        int sc = pc_scancode(e->key.keysym.scancode);
        if (sc) dos32_key(sc, 1);   /* repeats too, as on a PC keyboard */
        break; }
    case SDL_KEYUP: {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_F9 || k == SDLK_F10 || k == SDLK_F11 || k == SDLK_F12) return;
        int sc = pc_scancode(e->key.keysym.scancode);
        if (sc) dos32_key(sc, 0);
        break; }
    case SDL_MOUSEMOTION:
        if (captured && dst.w > 0 && dst.h > 0)
            dos32_mouse_move((double)e->motion.xrel * fw / dst.w, (double)e->motion.yrel * fh / dst.h, buttons);
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int bit = e->button.button == SDL_BUTTON_LEFT ? 1 : e->button.button == SDL_BUTTON_RIGHT ? 2
                : e->button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
        if (!captured) {         /* the click that takes the mouse is not the game's */
            if (e->type == SDL_MOUSEBUTTONDOWN && bit == 1 && in_dst(e->button.x, e->button.y)) capture(1);
            else if (e->type == SDL_MOUSEBUTTONDOWN && bit == 2 && !in_dst(e->button.x, e->button.y)) settings_menu();
            return;
        }
        buttons = e->type == SDL_MOUSEBUTTONDOWN ? buttons | bit : buttons & ~bit;
        dos32_mouse_move(0, 0, buttons);
        break; }
    }
}

void present_run(void) {
    /* Relative motion from the pointer as the desktop moves it (its speed and
     * acceleration), warped back to the middle, as present.c does on Windows. */
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "themepark: no display (%s)\n", SDL_GetError()); return; }
    wnd = SDL_CreateWindow("Theme Park", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!wnd) { fprintf(stderr, "themepark: no window (%s)\n", SDL_GetError()); return; }
    Uint32 rf = g_present.gdi ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
    if (g_present.vsync && !g_present.gdi) rf |= SDL_RENDERER_PRESENTVSYNC;
    ren = SDL_CreateRenderer(wnd, -1, rf);
    if (!ren) ren = SDL_CreateRenderer(wnd, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "themepark: no renderer (%s)\n", SDL_GetError()); return; }
    /* The size chosen in the menu, else the largest whole multiple of
     * 640x480 that fits the desktop. */
    int k = GetPrivateProfileIntA("display", "window", 0, ini_path), most = max_window_k();
    if (k < 1 || k > most) k = most >= 2 ? most & ~1 : most;
    int fs = g_present.fullscreen;
    size_window(k);
    SDL_ShowWindow(wnd);
    if (fs) apply_fullscreen(1);
    SDL_ShowCursor(SDL_DISABLE);   /* the game draws its own */
    if ((muted = GetPrivateProfileIntA("sound", "mute", 0, ini_path))) sound_set_mute(1);
    while (!closing) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) event(&e);
        if (SDL_GetWindowFlags(wnd) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }
        pad_poll(fw, fh, speed_step);
        int w, h, ow, oh;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        if (dos32_frame(frame, 1024, 768, &w, &h) && (w != fw || h != fh)) { fw = w; fh = h; cw = 0; }
        if (ow != cw || oh != ch) { cw = ow; ch = oh; layout(); }
        set_title();
        draw(ren, g_present.scale, g_present.look);
        SDL_RenderPresent(ren);
        if (!g_present.vsync || g_present.gdi) SDL_Delay(g_present.gdi ? 16 : 1);
    }
    SDL_DestroyWindow(wnd);
}

/* The game thread ended: stop the loop. */
void present_close(void) {
    closing = 1;
    SDL_Event e = { .type = SDL_QUIT };
    SDL_PushEvent(&e);
}

/* --present-test DIR: the run's last screen through each scaling mode at
 * 1440x1080, SDL's software renderer into DIR/<mode>.bmp, no window. */
int present_test(const char* dir) {
    int w, h, bad = 0;
    if (dos32_frame(frame, 1024, 768, &w, &h)) { fw = w; fh = h; }
    cw = 1440; ch = 1080;
    for (int m = 0; m < SCALE_COUNT; m++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/%s.bmp", dir, scale_names[m]);
        SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, SDL_PIXELFORMAT_RGB888);
        SDL_Renderer* r = sf ? SDL_CreateSoftwareRenderer(sf) : NULL;
        tex = mid = crt = NULL; tw = th = mw = mh = crt_w = crt_h = 0;
        g_present.scale = m;
        layout();
        if (r) { draw(r, m, 0); SDL_RenderPresent(r); }
        if (!r || SDL_SaveBMP(sf, path)) bad++;
        else fprintf(stderr, "present-test: %s\n", path);
        if (r) SDL_DestroyRenderer(r);   /* and its textures */
        SDL_FreeSurface(sf);
    }
    return bad;
}
