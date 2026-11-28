/*
 * The window: pcrecomp's presenter (runtime/present: the window, scaling,
 * fullscreen, the pad, themepark.ini) showing dos32's screen. This file is
 * what Theme Park adds to it:
 *
 *   Ctrl+PgUp/PgDn   speed: the game's frames per second, 12 to 120 (default 25)
 *   F10, right-click on the bars   the settings menu: the presenter's scaling,
 *                    color, fullscreen and mute, then save states, the network
 *                    game, the window size, speed, music, whether parks open in
 *                    640x480, the park's resolution, language, effects, rules
 *
 * The game reads how far the mouse moved, not where it is, so the presenter
 * captures it: a click in the picture takes it, Alt+Tab or F10 frees it. The
 * game's modes are 320x200 and 640x400 (both shown at 4:3, as their monitors
 * did) and 640x480; integer scaling (the default) keeps square pixels instead.
 */
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <SDL.h>
#include <time.h>
#endif

#include "dos32.h"
#include "present.h"
#include "pad.h"
#include "music.h"
#include "audio.h"
#include "net.h"
#include "rules.h"

extern double game_fps;          /* game.c: the speed control */
extern volatile int state_request;              /* game.c: save states */
extern char state_note[];
extern volatile DWORD state_note_until;
void state_path(int slot, char* out, size_t n);
void sound_set_mute(int on);     /* sound.c */
void sound_pick_music(const char* pack);
extern volatile int res_w;       /* game.c: the park's width (#11) */

static const int speeds[] = { 12, 15, 20, 25, 30, 35, 40, 50, 60, 90, 120 };
#define NSPEEDS (int)(sizeof speeds / sizeof speeds[0])
static uint32_t* frame;          /* dos32's screen through the palette; on the heap, not 8 MB in the image */
static int fw = 320, fh = 200;

/* ---- the picture, the keys and the pad ------------------------------------ */

static int tp_frame(present_frame* f) {
    int w, h;
    if (!frame && !(frame = calloc(1920 * 1080, 4))) return 0;
    if (dos32_frame(frame, 1920, 1080, &w, &h)) { fw = w; fh = h; lights_apply(frame, w, h); }
    int tall = (fw == 320 && fh == 200) || (fw == 640 && fh == 400);   /* 4:3 monitors, tall pixels */
    memset(f, 0, sizeof *f);
    f->px = frame; f->w = fw; f->h = fh; f->bpp = 32;
    f->aspect_w = tall ? 4 : 0; f->aspect_h = tall ? 3 : 0;
    return 1;
}

static void note_set(const char* fmt, int n, DWORD ms) {
    snprintf(state_note, 128, fmt, n);
    state_note_until = GetTickCount() + ms;
}

static void set_speed(int fps) {
    game_fps = fps;
    present_ini_set_int("game", "fps", fps);
    note_set("speed %d fps", fps, 2000);
}

static void speed_step(int up) {
    int k = 0;
    while (k < NSPEEDS - 1 && speeds[k] < game_fps) k++;
    k = up ? (k + 1 < NSPEEDS ? k + 1 : k) : (k > 0 ? k - 1 : 0);
    set_speed(speeds[k]);
}

static int ctrl_held(void) {
#ifdef _WIN32
    return GetKeyState(VK_CONTROL) < 0;
#else
    return (SDL_GetModState() & KMOD_CTRL) != 0;
#endif
}

static void tp_key(int sc, int down) {
    if ((sc == 0xE049 || sc == 0xE051) && ctrl_held()) {   /* Ctrl+PgUp/PgDn: the speed, not the game's */
        if (down) speed_step(sc == 0xE049);
        return;
    }
    dos32_key(sc, down);
}

static void tp_tick(void) { pad_poll(fw, fh, speed_step); }

static const char* tp_note(void) { return GetTickCount() < state_note_until ? state_note : NULL; }

/* ---- the window's size ------------------------------------------------------ */

/* Half the park's width (#11): 320 for 640x480, wider for a park of its own size. */
static int step_w(void) { return (res_w > 640 ? res_w : 640) / 2; }

/* Window sizes in 320x240 steps (k); the menu offers the even ones, whole
 * multiples of the park's 640x480: 1x, 2x, 3x, each pixel doubled up. The
 * largest k whose window, title bar and borders too, fits the work area. */
static int max_window_k(void) {
    int k = 8;
#ifdef _WIN32
    RECT wa;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    for (; k > 1; k--) {
        RECT r = { 0, 0, step_w() * k, 240 * k };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) break;
    }
#else
    SDL_Window* wnd = present_window();
    SDL_Rect wa;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (!wnd || SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(wnd), &wa)) return 2;
    SDL_GetWindowBordersSize(wnd, &top, &left, &bottom, &right);
    for (; k > 1; k--)
        if (step_w() * k + left + right <= wa.w && 240 * k + top + bottom <= wa.h) break;
#endif
    return k;
}

static void size_window(int k) { present_resize(step_w() * k, 240 * k); }

/* The window's first size, the first time (then it opens where it was left):
 * the largest whole multiple of 640x480 that fits (2x, 1280x960, on a 1080p
 * screen). */
static void tp_start(int* w, int* h) {
    int most = max_window_k(), k = most >= 2 ? most & ~1 : most;
    *w = step_w() * k; *h = 240 * k;
}

/* The park resolutions the menu offers; 0 is "fit the screen": its shape at 480 high. */
static const int res_list[] = { 640, 800, 856, 1120, 0 };
#define NRES (int)(sizeof res_list / sizeof res_list[0])
static int res_fit(void) {
    int w, h;
#ifdef _WIN32
    w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN);
#else
    SDL_DisplayMode dm;
    w = 640; h = 480;
    if (present_window() && !SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(present_window()), &dm)) { w = dm.w; h = dm.h; }
#endif
    w = h ? (480 * w / h) & ~7 : 640;
    return w < 640 ? 640 : w > 1920 ? 1920 : w;
}

/* Takes effect when the game next sets 640x480 up: a park opening, or R. */
static void set_res(int w) {
    int cw, ch;
    char v[16];
    snprintf(v, sizeof v, "%dx480", w);
    present_ini_set("game", "res", v);
    res_w = w > 640 ? w : 0;
    note_set("park resolution %d x 480: at the next park, or R", w, 4000);
    present_client(&cw, &ch);
    if (!g_present.fullscreen && ch >= 480) size_window(ch / 240);   /* the same scale, the new shape */
}

/* A save state slot's line in the menu: when it was saved, or empty. */
static int slot_label(int slot, char* out, size_t n, const char* sep) {
    char path[MAX_PATH];
    state_path(slot, path, sizeof path);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    SYSTEMTIME u, t;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) { snprintf(out, n, "Slot %d%s(empty)", slot, sep); return 0; }
    FileTimeToSystemTime(&a.ftLastWriteTime, &u);
    SystemTimeToTzSpecificLocalTime(NULL, &u, &t);
    snprintf(out, n, "Slot %d%s%04d-%02d-%02d %02d:%02d", slot, sep, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
#else
    struct stat st;
    if (stat(path, &st)) { snprintf(out, n, "Slot %d%s(empty)", slot, sep); return 0; }
    struct tm* t = localtime(&st.st_mtime);
    snprintf(out, n, "Slot %d%s%04d-%02d-%02d %02d:%02d", slot, sep, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min);
#endif
    return 1;
}

static void set_music(const char* pack) {
    sound_pick_music(pack);
    present_ini_set("sound", "music", pack);
}

static void set_lang(int i) {
    char v[4] = { (char)('0' + i), 0 };
    present_ini_set("game", "lang", v);
    note_set("language: from the next start", 0, 4000);
}

static int hires_on(void) { char v[8]; present_ini_get("game", "hires", "1", v, sizeof v); return v[0] != '0'; }

static void rules_all(int on) {   /* all on, all off (#43) */
    tp_rules = 0;
    if (on) for (int i = 0; i < rule_count; i++) tp_rules |= rule_list[i].bit;
    rules_save(present_ini());
}

#ifdef _WIN32
/* ---- the menu: Windows -------------------------------------------------------- */

/* A one-line question with OK and Cancel. 1 with the answer in buf, else 0. */
static int ask_done;
static LRESULT CALLBACK ask_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        ask_done = LOWORD(wp) == IDOK ? 1 : -1;
        return 0;
    }
    if (m == WM_CLOSE) { ask_done = -1; return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

static int ask_text(const char* title, const char* prompt, char* buf, int n) {
    static int registered;
    HWND wnd = present_window();
    HINSTANCE hi = GetModuleHandleA(NULL);
    if (!registered) {
        WNDCLASSA c = { 0 };
        c.lpfnWndProc = ask_proc; c.hInstance = hi; c.lpszClassName = "TPAsk";
        c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); c.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&c);
        registered = 1;
    }
    RECT r = { 0, 0, 380, 110 }, p;
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    GetWindowRect(wnd, &p);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    HWND d = CreateWindowExA(WS_EX_DLGMODALFRAME, "TPAsk", title, WS_CAPTION | WS_SYSMENU | WS_POPUP,
                             (p.left + p.right - w) / 2, (p.top + p.bottom - hgt) / 2, w, hgt, wnd, NULL, hi, NULL);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND kids[4] = {
        CreateWindowA("STATIC", prompt, WS_CHILD | WS_VISIBLE, 12, 10, 356, 20, d, NULL, hi, NULL),
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        12, 34, 356, 24, d, (HMENU)100, hi, NULL),
        CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 196, 72, 80, 26, d, (HMENU)IDOK, hi, NULL),
        CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 288, 72, 80, 26, d, (HMENU)IDCANCEL, hi, NULL) };
    for (int i = 0; i < 4; i++) SendMessageA(kids[i], WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageA(kids[1], EM_SETSEL, 0, -1);
    ShowWindow(d, SW_SHOW);
    SetFocus(kids[1]);
    EnableWindow(wnd, FALSE);
    ask_done = 0;
    MSG msg;
    while (!ask_done && GetMessageA(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageA(d, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (ask_done > 0) GetWindowTextA(kids[1], buf, n);
    EnableWindow(wnd, TRUE);
    DestroyWindow(d);
    SetForegroundWindow(wnd);
    return ask_done > 0;
}

/* Start again with these options: a network game is set up at start-up. */
static void relaunch(const char* args) {
    if (MessageBoxA(present_window(), "This starts Theme Park again for the network game.\n"
                    "The park you are playing now is not kept (save a state first if you want it).",
                    "Network game", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
    char exe[MAX_PATH], cmd[MAX_PATH + 320];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --nointro %s", exe, args);   /* seen once already (#37) */
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxA(present_window(), "Could not start Theme Park again.", "Network game", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ExitProcess(0);
}

enum { ID_SPEED = 200, ID_HIRES = 303, ID_SIZE = 400, ID_SAVE = 500, ID_LOAD = 600, ID_NET = 700,
       ID_NET_JOIN = 710, ID_NET_RESUME, ID_NET_P = 720, ID_MUSIC = 800, ID_RES = 900, ID_RULES = 1000,
       ID_LANG = 1100, ID_FX = 1200 };   /* ID_NET + players: host */
static const int give_amounts[4] = { 1000, 5000, 10000, 50000 }, grant_amounts[3] = { 1000, 10000, 50000 };
static const char* const langs[5] = { "English", "Deutsch", "Italiano", "Espa\xF1ol", "Fran\xE7" "ais" };
static char packs[32][64];
static int npacks;

/* The "Network game" submenu: start one, or, in one, the house rules. */
static HMENU network_menu(void) {
    HMENU nm = CreatePopupMenu();
    char t[96];
    if (!net_n) {
        for (int k = 2; k <= 4; k++) {
            snprintf(t, sizeof t, "Host a game for %d players", k);
            AppendMenuA(nm, MF_STRING, ID_NET + k, t);
        }
        AppendMenuA(nm, MF_STRING, ID_NET_JOIN, "Join a game...");
        int last = net_last_hosted();
        snprintf(t, sizeof t, "Resume the %d-player game I hosted", last);
        AppendMenuA(nm, MF_STRING | (last ? 0 : MF_GRAYED), ID_NET_RESUME, last ? t : "Resume the game I hosted");
        return nm;
    }
    snprintf(t, sizeof t, "You are player %d of %d", net_me + 1, net_n);
    AppendMenuA(nm, MF_STRING | MF_GRAYED, 0, t);
    for (int j = 0; j < net_n; j++) {
        if (j == net_me) continue;
        HMENU pm = CreatePopupMenu();
        for (int k = 0; k < 4; k++) {
            snprintf(t, sizeof t, "Give %d", give_amounts[k]);
            AppendMenuA(pm, MF_STRING, ID_NET_P + j * 20 + k, t);
        }
        AppendMenuA(pm, MF_STRING | (net_sharing[j] ? MF_CHECKED : 0), ID_NET_P + j * 20 + 4, "Share my research with them");
        if (net_me == 0) {
            AppendMenuA(pm, MF_SEPARATOR, 0, NULL);
            for (int k = 0; k < 3; k++) {
                snprintf(t, sizeof t, "Grant %d (host)", grant_amounts[k]);
                AppendMenuA(pm, MF_STRING, ID_NET_P + j * 20 + 5 + k, t);
            }
            for (int k = 0; k < 3; k++) {
                snprintf(t, sizeof t, "Fine %d (host)", grant_amounts[k]);
                AppendMenuA(pm, MF_STRING, ID_NET_P + j * 20 + 8 + k, t);
            }
        }
        snprintf(t, sizeof t, "Player %d%s", j + 1, net_player_up(j) ? "" : " (away)");
        AppendMenuA(nm, MF_POPUP, (UINT_PTR)pm, t);
    }
    return nm;
}

static void network_command(int cmd) {
    char args[320], addr[256];
    if (cmd > ID_NET && cmd <= ID_NET + 4) { snprintf(args, sizeof args, "--host %d", cmd - ID_NET); relaunch(args); }
    else if (cmd == ID_NET_JOIN) {
        present_ini_get("net", "join", "127.0.0.1", addr, sizeof addr);
        if (!ask_text("Join a network game", "The host's address (name or IP, and :port if not 7790):", addr, sizeof addr) || !addr[0]) return;
        present_ini_set("net", "join", addr);
        snprintf(args, sizeof args, "--join %s", addr);
        relaunch(args);
    }
    else if (cmd == ID_NET_RESUME) { snprintf(args, sizeof args, "--host %d --resume", net_last_hosted()); relaunch(args); }
    else if (cmd >= ID_NET_P && cmd < ID_NET_P + 80) {
        int j = (cmd - ID_NET_P) / 20, k = (cmd - ID_NET_P) % 20;
        if (k < 4) net_rules_give(j, give_amounts[k]);
        else if (k == 4) net_rules_share(j, !net_sharing[j]);
        else if (k < 8) net_rules_grant(j, grant_amounts[k - 5]);
        else if (k < 11) net_rules_grant(j, -grant_amounts[k - 8]);
    }
}

/* After the presenter's scaling, color and fullscreen. */
static void tp_menu(void* mv) {
    HMENU m = mv, sp = CreatePopupMenu(), sz = CreatePopupMenu(), sv = CreatePopupMenu(), ld = CreatePopupMenu(),
          rs = CreatePopupMenu(), mu = CreatePopupMenu(), ru = CreatePopupMenu(), la = CreatePopupMenu(),
          fx = CreatePopupMenu();
    char t[96];
    int cw, ch, lang = present_ini_int("game", "lang", 0);
    present_client(&cw, &ch);
    for (int slot = 1; slot <= 9; slot++) {
        int used = slot_label(slot, t, sizeof t, "\t");
        AppendMenuA(sv, MF_STRING, ID_SAVE + slot, t);
        AppendMenuA(ld, MF_STRING | (used && !net_n ? 0 : MF_GRAYED), ID_LOAD + slot, t);   /* not in a network game: it would leave the others */
    }
    for (int k = 2; k <= max_window_k(); k += 2) {
        snprintf(t, sizeof t, "%dx\t%d x %d", k / 2, step_w() * k, 240 * k);
        AppendMenuA(sz, MF_STRING | (!g_present.fullscreen && cw == step_w() * k && ch == 240 * k ? MF_CHECKED : 0), ID_SIZE + k, t);
    }
    for (int k = 0; k < NSPEEDS; k++) {
        snprintf(t, sizeof t, "%d fps%s", speeds[k], speeds[k] == 25 ? " (original)" : "");
        AppendMenuA(sp, MF_STRING | ((int)game_fps == speeds[k] ? MF_CHECKED : 0), ID_SPEED + k, t);
    }
    npacks = music_packs(packs, 32);
    AppendMenuA(mu, MF_STRING | (!music_pack()[0] ? MF_CHECKED : 0), ID_MUSIC, "The game's own (MIDI)");
    for (int i = 0; i < npacks; i++)
        AppendMenuA(mu, MF_STRING | (!_stricmp(music_pack(), packs[i]) ? MF_CHECKED : 0), ID_MUSIC + 1 + i, packs[i]);
    if (!npacks) AppendMenuA(mu, MF_STRING | MF_GRAYED, 0, "No packs in the music folder (music\\README.md)");
    char res_ini[16];
    int cur = 640, ticked = 0;
    present_ini_get("game", "res", "640x480", res_ini, sizeof res_ini);
    sscanf(res_ini, "%d", &cur);
    static const char* res_names[NRES] = { "640 x 480 (original)", "800 x 480", "856 x 480 (16:9)", "1120 x 480 (21:9)", "Fit the screen" };
    for (int i = 0; i < NRES; i++) {
        int w = res_list[i] ? res_list[i] : res_fit(), on = !ticked && w == cur;
        if (res_list[i]) snprintf(t, sizeof t, "%s", res_names[i]);
        else snprintf(t, sizeof t, "%s (%d x 480)", res_names[i], w);
        AppendMenuA(rs, MF_STRING | (on ? MF_CHECKED : 0), ID_RES + i, t);
        ticked |= on;
    }
    for (int i = 0; i < 5; i++) AppendMenuA(la, MF_STRING | (lang == i ? MF_CHECKED : 0), ID_LANG + i, langs[i]);
    AppendMenuA(la, MF_SEPARATOR, 0, NULL);
    AppendMenuA(la, MF_STRING | MF_GRAYED, 0, "From the next start");
    for (int i = 0; i < effect_group_count; i++)   /* effects to turn off (#41) */
        AppendMenuA(fx, MF_STRING | (effects_muted >> i & 1 ? 0 : MF_CHECKED), ID_FX + i, effect_groups[i].label);
    /* Modern rules (rules.c): in a network game, the host's, and fixed. */
    for (int i = 0; i < rule_count; i++)
        AppendMenuA(ru, MF_STRING | (tp_rules & rule_list[i].bit ? MF_CHECKED : 0) | (net_n ? MF_GRAYED : 0),
                    ID_RULES + i, rule_list[i].label);
    AppendMenuA(ru, MF_SEPARATOR, 0, NULL);
    AppendMenuA(ru, MF_STRING | (net_n ? MF_GRAYED : 0), ID_RULES + RULE_MAX, "Turn all on");
    AppendMenuA(ru, MF_STRING | (net_n ? MF_GRAYED : 0), ID_RULES + RULE_MAX + 1, "Turn all off");
    AppendMenuA(ru, MF_STRING | MF_GRAYED, 0, net_n ? "The host's rules, for the whole game" :
                "Off: the game as in 1994 (docs\\rules.md)");

    AppendMenuA(m, MF_POPUP, (UINT_PTR)sv, "Save state");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ld, "Load state");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)network_menu(), "Network game");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sz, "Window size");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sp, "Speed\tCtrl+PgUp/PgDn");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)mu, "Music");
    AppendMenuA(m, MF_STRING | (hires_on() ? MF_CHECKED : 0), ID_HIRES, "Open parks in 640x480 (R switches in a park)");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)rs, "Park resolution");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)la, "Language");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)fx, "Sound effects");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ru, "Modern rules");
}

static void tp_command(int cmd) {
    if (cmd >= ID_NET && cmd < ID_NET_P + 80) network_command(cmd);
    else if (cmd > ID_SAVE && cmd <= ID_SAVE + 9) state_request = cmd - ID_SAVE;
    else if (cmd > ID_LOAD && cmd <= ID_LOAD + 9) state_request = -(cmd - ID_LOAD);
    else if (cmd >= ID_SIZE && cmd < ID_SAVE) size_window(cmd - ID_SIZE);
    else if (cmd >= ID_SPEED && cmd < ID_SPEED + NSPEEDS) set_speed(speeds[cmd - ID_SPEED]);
    else if (cmd == ID_HIRES) present_ini_set("game", "hires", hires_on() ? "0" : "1");
    else if (cmd >= ID_RES && cmd < ID_RES + NRES) set_res(res_list[cmd - ID_RES] ? res_list[cmd - ID_RES] : res_fit());
    else if (cmd >= ID_FX && cmd < ID_FX + effect_group_count) { effects_muted ^= 1u << (cmd - ID_FX); effects_save(present_ini()); }
    else if (cmd >= ID_LANG && cmd < ID_LANG + 5) set_lang(cmd - ID_LANG);
    else if (cmd >= ID_RULES && cmd < ID_RULES + rule_count && !net_n) { tp_rules ^= rule_list[cmd - ID_RULES].bit; rules_save(present_ini()); }
    else if ((cmd == ID_RULES + RULE_MAX || cmd == ID_RULES + RULE_MAX + 1) && !net_n) rules_all(cmd == ID_RULES + RULE_MAX);
    else if (cmd >= ID_MUSIC && cmd <= ID_MUSIC + npacks) set_music(cmd == ID_MUSIC ? "" : packs[cmd - ID_MUSIC - 1]);
}

#else
/* ---- the menu: SDL, as a row of dialogs ------------------------------------ */

static void state_menu(int load) {
    char text[640] = "", lines[9][64];
    const char* labels[10];
    int slots[10], n = 0;
    for (int s = 1; s <= 9; s++) {
        int used = slot_label(s, lines[s - 1], sizeof lines[0], "   ");
        strncat(text, lines[s - 1], sizeof text - strlen(text) - 2);
        strcat(text, "\n");
        if (load && !used) continue;
        static const char* digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
        labels[n] = digits[s - 1]; slots[n++] = s;
    }
    if (load && !n) { const char* ok[] = { "Close" }; present_ask("No saved states yet.", ok, 1); return; }
    labels[n++] = "Cancel";
    int c = present_ask(text, labels, n);
    if (c >= 0) state_request = load ? -slots[c] : slots[c];
}

/* The park's resolution (#11): takes effect at the next park, or R. */
static void res_menu(void) {
    static char fit[40];
    snprintf(fit, sizeof fit, "Fit the screen (%d x 480)", res_fit());
    const char* l[] = { "640 x 480 (original)", "800 x 480", "856 x 480 (16:9)", "1120 x 480 (21:9)", fit, "Cancel" };
    int c = present_ask("The park's resolution, from the next park (or R). The panel stays 640 wide.", l, 6);
    if (c >= 0) set_res(res_list[c] ? res_list[c] : res_fit());
}

/* Start again with these options: a network game is set up at start-up. */
static void relaunch(const char* a1, const char* a2, const char* a3) {
    const char* l[] = { "Start again", "Cancel" };
    if (present_ask("This starts Theme Park again for the network game.\n"
                    "The park you are playing now is not kept (save a state first if you want it).", l, 2) != 0) return;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char* argv[] = { exe, "--nointro", (char*)a1, (char*)a2, (char*)a3, NULL };   /* seen once already (#37) */
    execv(exe, argv);
}

static void network_menu(void) {
    char text[400], addr[256], hosts[3][8], last[48];
    if (!net_n) {
        present_ini_get("net", "join", "127.0.0.1", addr, sizeof addr);
        int had = net_last_hosted();
        snprintf(text, sizeof text, "A network game for 2 to 4 players. One hosts (TCP port 7790), the others join.\n\n"
                 "Join connects to %s: change it with [net] join= in themepark.ini.", addr);
        snprintf(last, sizeof last, "Resume my %d-player game", had);
        const char* l[] = { "Host 2", "Host 3", "Host 4", "Join", last, "Cancel" };
        int c = present_ask(text, l, had ? 6 : 5);
        if (!had && c == 4) c = -1;
        if (c >= 0 && c < 3) { snprintf(hosts[c], sizeof hosts[c], "%d", c + 2); relaunch("--host", hosts[c], NULL); }
        else if (c == 3) relaunch("--join", addr, NULL);
        else if (c == 4) { snprintf(hosts[0], sizeof hosts[0], "%d", had); relaunch("--host", hosts[0], "--resume"); }
        return;
    }
    static char names[4][16];
    const char* l[5];
    int who[4], n = 0;
    for (int j = 0; j < net_n; j++) {
        if (j == net_me) continue;
        snprintf(names[n], sizeof names[n], "Player %d%s", j + 1, net_player_up(j) ? "" : " (away)");
        l[n] = names[n]; who[n++] = j;
    }
    l[n] = "Cancel";
    snprintf(text, sizeof text, "You are player %d of %d. House rules: give money, share research%s.",
             net_me + 1, net_n, net_me == 0 ? ", and as the host grant or fine" : "");
    int c = present_ask(text, l, n + 1);
    if (c < 0) return;
    int j = who[c];
    const char* a[] = { "Give 1000", "Give 10000", "Give 50000", net_sharing[j] ? "Stop sharing research" : "Share research",
                        "Grant 10000", "Fine 10000", "Cancel" };
    if (net_me != 0) a[4] = "Cancel";
    snprintf(text, sizeof text, "Player %d", j + 1);
    int k = present_ask(text, a, net_me == 0 ? 7 : 5);
    static const int give[] = { 1000, 10000, 50000 };
    if (k >= 0 && k < 3) net_rules_give(j, give[k]);
    else if (k == 3) net_rules_share(j, !net_sharing[j]);
    else if (k == 4 && net_me == 0) net_rules_grant(j, 10000);
    else if (k == 5 && net_me == 0) net_rules_grant(j, -10000);
}

/* Sound effects, each on or off (#41). */
static void effects_menu(void) {
    for (;;) {
        char t[32][64];
        const char* l[33];
        for (int i = 0; i < effect_group_count && i < 32; i++) {
            snprintf(t[i], sizeof t[i], "%s %s", effects_muted >> i & 1 ? "[off]" : "[on] ", effect_groups[i].label);
            l[i] = t[i];
        }
        l[effect_group_count] = "Close";
        int c = present_ask("Sound effects: pick one to turn it on or off.", l, effect_group_count + 1);
        if (c < 0 || c >= effect_group_count) return;
        effects_muted ^= 1u << c;
        effects_save(present_ini());
    }
}

/* Modern rules (rules.c): each on or off; the host's in a network game. */
static void rules_menu(void) {
    for (;;) {
        char text[4096], t[RULE_MAX][96];
        const char* l[RULE_MAX + 3];
        int n = snprintf(text, sizeof text, "%s", net_n ? "In a network game everyone plays by the host's rules.\n\n" :
                         "Changes to how the 1994 game behaves, each off unless you turn it on.\n\n");
        for (int i = 0; i < rule_count && n < (int)sizeof text; i++) {
            int on = (tp_rules & rule_list[i].bit) != 0;
            n += snprintf(text + n, sizeof text - n, "%s %s\n   %s\n", on ? "[on] " : "[off]", rule_list[i].label, rule_list[i].about);
            snprintf(t[i], sizeof t[i], "%s: %s", on ? "Turn off" : "Turn on", rule_list[i].label);
            l[i] = t[i];
        }
        l[rule_count] = "Turn all on"; l[rule_count + 1] = "Turn all off"; l[rule_count + 2] = "Close";
        int c = net_n ? (present_ask(text, &l[rule_count + 2], 1), -1) : present_ask(text, l, rule_count + 3);
        if (c < 0 || c >= rule_count + 2) return;
        if (c < rule_count) { tp_rules ^= rule_list[c].bit; rules_save(present_ini()); }
        else rules_all(c == rule_count);
    }
}

enum { P_SAVE = 1, P_LOAD, P_NET, P_SIZE, P_SPEED, P_MUSIC, P_MORE, P_RULES };

/* After the presenter's Display page. */
static void tp_menu(void* m) {
    static char speed[32], hires[48];
    snprintf(speed, sizeof speed, "Speed: %.0f fps", game_fps);
    snprintf(hires, sizeof hires, "Parks open in 640x480: %s", hires_on() ? "yes" : "no");
    present_menu_add(m, 0, speed);
    present_menu_add(m, 0, hires);
    present_menu_add(m, 0, "Ctrl+PgUp/PgDn speed, R 640x480 in a park");
    present_menu_add(m, P_SAVE, "Save state");
    present_menu_add(m, P_LOAD, "Load state");
    present_menu_add(m, P_NET, "Network");
    present_menu_add(m, P_SIZE, "Window size");
    present_menu_add(m, P_SPEED, "Speed");
    present_menu_add(m, P_MUSIC, "Music");
    present_menu_add(m, P_MORE, "More");
    present_menu_add(m, P_RULES, "Modern rules");
}

static void tp_command(int page) {
    switch (page) {
    case P_SAVE: state_menu(0); break;
    case P_LOAD:
        if (net_n) { const char* ok[] = { "Close" }; present_ask("Loading a state is off in a network game: it would leave the others behind.", ok, 1); }
        else state_menu(1);
        break;
    case P_NET: network_menu(); break;
    case P_SIZE: {
        static char sizes[4][32];
        const char* l[5];
        int n = 0, ks[4];
        for (int k = 2; k <= max_window_k() && n < 4; k += 2) {
            snprintf(sizes[n], sizeof sizes[0], "%dx  %d x %d", k / 2, step_w() * k, 240 * k);
            l[n] = sizes[n]; ks[n++] = k;
        }
        l[n] = "Cancel";
        int c = present_ask("Window size", l, n + 1);
        if (c >= 0) size_window(ks[c]);
        break; }
    case P_SPEED: {
        static char t[NSPEEDS][8];
        const char* l[NSPEEDS + 1];
        for (int k = 0; k < NSPEEDS; k++) { snprintf(t[k], sizeof t[0], "%d", speeds[k]); l[k] = t[k]; }
        l[NSPEEDS] = "Cancel";
        int c = present_ask("The game's speed in frames per second (25 is the original's).", l, NSPEEDS + 1);
        if (c >= 0) set_speed(speeds[c]);
        break; }
    case P_MUSIC: {
        static char packs[14][64];
        const char* l[16];
        int n = music_packs(packs, 14);
        l[0] = "The game's own (MIDI)";
        for (int i = 0; i < n; i++) l[1 + i] = packs[i];
        l[n + 1] = "Cancel";
        char t[160];
        snprintf(t, sizeof t, "Music: %s\n\nA pack is a folder of recordings in music/ (music/README.md).",
                 music_pack()[0] ? music_pack() : "the game's own (MIDI)");
        int c = present_ask(t, l, n + 2);
        if (c >= 0) set_music(c == 0 ? "" : packs[c - 1]);
        break; }
    case P_MORE: {
        int hires = hires_on();
        const char* l[] = { hires ? "Parks in 320x200" : "Parks in 640x480", "Park resolution", "Language",
                            "Sound effects", "Cancel" };
        int c = present_ask("The park's resolution when one opens (R switches in a park), the language, the effects.", l, 5);
        if (c == 0) present_ini_set("game", "hires", hires ? "0" : "1");
        else if (c == 1) res_menu();
        else if (c == 2) {                        /* the game's languages (LANGn-0.DAT), read at start-up */
            const char* g[] = { "English", "Deutsch", "Italiano", "Español", "Français", "Cancel" };
            int k = present_ask("The game's language, from the next start.", g, 6);
            if (k >= 0 && k < 5) set_lang(k);
        }
        else if (c == 3) effects_menu();
        break; }
    case P_RULES: rules_menu(); break;
    }
}
#endif

present_hooks present_title = {
    .name = "Theme Park",
    .section = "display",
    .scale = PRESENT_INTEGER,
    .start = tp_start,
    .frame = tp_frame,
    .key = tp_key,
    .mouse_move = dos32_mouse_move,
    .tick = tp_tick,
    .menu = tp_menu,
    .command = tp_command,
    .note = tp_note,
    .mute = sound_set_mute,
};
