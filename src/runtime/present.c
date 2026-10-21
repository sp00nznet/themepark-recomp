/*
 * The window: dos32's screen through our own presenter, and the player's keys
 * and mouse handed back to the game.
 *
 *   F12              scaling: integer (default: pixels doubled up), sharp-bilinear, smooth, CRT, nearest
 *   F9               colour: natural / vivid
 *   Ctrl+PgUp/PgDn   speed: the game's frames per second, 12 to 120 (default 25)
 *   F11, Alt+Enter   borderless fullscreen
 *   F10, right-click on the bars   the settings menu (all of the above, save
 *                    states, window size, mute, and whether parks open in 640x480)
 *
 * The mouse is captured by a click in the picture and freed by Alt+Tab or F10:
 * the game reads how far the mouse moved, not where it is (see capture()).
 *
 * The picture is scaled by Direct3D 11 (present_d3d.c), or by GDI if that is
 * unavailable. Choices are kept in themepark.ini beside the exe. The game's
 * modes are 320x200 and 640x400 (both shown at 4:3, as their monitors did) and
 * 640x480; integer scaling keeps square pixels instead.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dos32.h"
#include "present.h"
#include "pad.h"
#include "music.h"
#include "net.h"
#include "rules.h"

present_settings g_present = { SCALE_INTEGER, 0, 0, 1, 0 };

static const char* scale_names[SCALE_COUNT] = { "sharp", "smooth", "crt", "nearest", "integer" };
static char ini_path[MAX_PATH];
static HWND wnd;
static uint32_t frame[1920 * 1080];
static int fw = 320, fh = 200;
static RECT dst;                 /* where the picture lands in the client area */
static int cw, ch;
static WINDOWPLACEMENT saved_place = { sizeof saved_place };
static DWORD title_until;        /* show the scaling mode in the title for a moment */
extern double game_fps;          /* game.c: the speed control */
static const int speeds[] = { 12, 15, 20, 25, 30, 35, 40, 50, 60, 90, 120 };
static int captured, buttons;    /* the mouse: below */
static DWORD hint_until;
static void clip(void);
extern volatile int state_request;              /* game.c: save states */
extern char state_note[];
extern volatile DWORD state_note_until;
void state_path(int slot, char* out, size_t n);

int present_scale_from_name(const char* name) {
    for (int i = 0; i < SCALE_COUNT; i++) if (!_stricmp(name, scale_names[i])) return i;
    return -1;
}

void present_load_settings(void) {
    char buf[64];
    GetModuleFileNameA(NULL, ini_path, MAX_PATH);
    strcpy(strrchr(ini_path, '\\') + 1, "themepark.ini");
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
    } else if ((fw == 320 && fh == 200) || (fw == 640 && fh == 400)) {
        /* 4:3: 320x200 and 640x400 were shown on 4:3 monitors with tall pixels. */
        w = cw; h = cw * 3 / 4;
        if (h > ch) { h = ch; w = ch * 4 / 3; }
    } else {                         /* square pixels: 640x480, and a park's own size */
        w = cw; h = cw * fh / fw;
        if (h > ch) { h = ch; w = ch * fw / fh; }
    }
    dst.left = (cw - w) / 2; dst.top = (ch - h) / 2;
    dst.right = dst.left + w; dst.bottom = dst.top + h;
    if (captured) clip();
}

static void set_title(void) {
    char t[192];
    DWORD now = GetTickCount();
    if (now < state_note_until)
        snprintf(t, sizeof t, "Theme Park - %s", state_note);
    else if (now < title_until)
        snprintf(t, sizeof t, "Theme Park - %s%s - speed %.0f fps", scale_names[g_present.scale],
                 g_present.look ? ", vivid" : "", game_fps);
    else if (!captured)
        snprintf(t, sizeof t, "Theme Park - click to play");
    else if (now < hint_until)
        snprintf(t, sizeof t, "Theme Park - Alt+Tab or F10 frees the mouse");
    else
        snprintf(t, sizeof t, "Theme Park");
    SetWindowTextA(wnd, t);
}

static void apply_fullscreen(int on) {
    DWORD st = GetWindowLong(wnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = { sizeof mi };
        if (st & WS_OVERLAPPEDWINDOW) GetWindowPlacement(wnd, &saved_place);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!(st & WS_OVERLAPPEDWINDOW)) {
        SetWindowLong(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &saved_place);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_present.fullscreen = on;
}

/* The mouse is captured while the window has it: clicking into the picture
 * takes it, and losing focus (Alt+Tab, the Windows key) or the settings menu
 * gives it back. Captured, the Windows pointer (hidden, with the player's own
 * speed and acceleration) is held inside the picture and its motion goes to the
 * game as a real mouse's would: the game moves its own cursor by how far the
 * mouse went, and scrolls the park when that pushes past the edge of the
 * screen. The pointer is put back in the middle before it reaches an edge, so
 * motion past the edge still counts. */
static POINT last;               /* where the pointer was, client pixels */

static void clip(void) {
    RECT r = dst;
    MapWindowPoints(wnd, NULL, (POINT*)&r, 2);
    ClipCursor(&r);
}

static void capture(int on) {
    if (on == captured) return;
    captured = on;
    if (on) { clip(); GetCursorPos(&last); ScreenToClient(wnd, &last); hint_until = GetTickCount() + 4000; }
    else { ClipCursor(NULL); buttons = 0; dos32_mouse_move(0, 0, 0); }
    title_until = 0;
}

static void pointer_moved(int x, int y) {
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return;
    if (x != last.x || y != last.y)
        dos32_mouse_move((double)(x - last.x) * fw / dw, (double)(y - last.y) * fh / dh, buttons);
    last.x = x; last.y = y;
    if (x < dst.left + dw / 4 || x >= dst.right - dw / 4 || y < dst.top + dh / 4 || y >= dst.bottom - dh / 4) {
        POINT c = { (dst.left + dst.right) / 2, (dst.top + dst.bottom) / 2 };
        last = c;                    /* the move SetCursorPos makes is then no motion */
        ClientToScreen(wnd, &c);
        SetCursorPos(c.x, c.y);
    }
}

/* GDI: the fallback when Direct3D 11 is unavailable. */
static void gdi_paint(void) {
    HDC dc = GetDC(wnd);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), fw, -fh, 1, 32, BI_RGB } };
    HBRUSH black = GetStockObject(BLACK_BRUSH);
    RECT bars[4] = { { 0, 0, cw, dst.top }, { 0, dst.bottom, cw, ch },
                     { 0, dst.top, dst.left, dst.bottom }, { dst.right, dst.top, cw, dst.bottom } };
    for (int i = 0; i < 4; i++) FillRect(dc, &bars[i], black);
    SetStretchBltMode(dc, g_present.scale == SCALE_SMOOTH ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                  0, 0, fw, fh, frame, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(wnd, dc);
}

static void set_speed(int fps);
static void speed_step(int up) {
    int n = sizeof speeds / sizeof speeds[0], k = 0;
    while (k < n - 1 && speeds[k] < game_fps) k++;
    k = up ? (k + 1 < n ? k + 1 : k) : (k > 0 ? k - 1 : 0);
    set_speed(speeds[k]);
}

static void render(void) {
    pad_poll(fw, fh, speed_step);
    int w, h;
    int got = dos32_frame(frame, 1920, 1080, &w, &h);
    if (got && (w != fw || h != fh)) { fw = w; fh = h; layout(); }
    if (got) lights_apply(frame, w, h);
    set_title();
    if (g_present.gdi || !d3d_present(wnd, frame, fw, fh, &dst, cw, ch, g_present.scale, g_present.look,
                                      g_present.vsync)) {
        g_present.gdi = 1;
        gdi_paint();
        Sleep(16);
    } else if (!g_present.vsync) Sleep(1);
}

static void key(LPARAM lp, int down) {
    dos32_key(((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0xE000 : 0), down);
}

void sound_set_mute(int on);           /* sound.c */
void sound_pick_music(const char* pack);
static int muted;

static void set_speed(int fps) {
    char v[16];
    game_fps = fps;
    snprintf(v, sizeof v, "%d", fps);
    WritePrivateProfileStringA("game", "fps", v, ini_path);
    title_until = GetTickCount() + 2000;
}

/* Half the park's width (#11): 320 for 640x480, wider for a park of its own size. */
extern volatile int res_w;   /* game.c */
static int step_w(void) { return (res_w > 640 ? res_w : 640) / 2; }

/* The park resolutions the menu offers; 0 is "fit the screen": its shape at 480 high. */
static const int res_list[] = { 640, 800, 856, 1120, 0 };
static const char* res_names[] = { "640 x 480 (original)", "800 x 480", "856 x 480 (16:9)",
                                   "1120 x 480 (21:9)", "Fit the screen" };
static int res_fit(void) {
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    w = h ? (480 * w / h) & ~7 : 640;
    return w < 640 ? 640 : w > 1920 ? 1920 : w;
}

/* Window sizes, in 320x240 steps (k); the menu offers the even ones, whole
 * multiples of the park's 640x480: 1x, 2x, 3x, each pixel doubled up. */
static int max_window_k(void) {
    RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    int k = 8;
    for (; k > 1; k--) {             /* the whole window, title bar and borders too */
        RECT r = { 0, 0, step_w() * k, 240 * k };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) break;
    }
    return k;
}

static void size_window(int k) {
    RECT r = { 0, 0, step_w() * k, 240 * k };
    if (g_present.fullscreen) apply_fullscreen(0);
    ShowWindow(wnd, SW_RESTORE);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);   /* centred, so all of it is on screen */
    SetWindowPos(wnd, NULL, wa.left + (wa.right - wa.left - (r.right - r.left)) / 2,
                 wa.top + (wa.bottom - wa.top - (r.bottom - r.top)) / 2,
                 r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
}

/* A save state slot's line in the menu: when it was saved, or empty. */
static int slot_label(int slot, char* out, size_t n) {
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    SYSTEMTIME u, t;
    state_path(slot, path, sizeof path);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) { snprintf(out, n, "Slot %d\t(empty)", slot); return 0; }
    FileTimeToSystemTime(&a.ftLastWriteTime, &u);
    SystemTimeToTzSpecificLocalTime(NULL, &u, &t);
    snprintf(out, n, "Slot %d\t%04d-%02d-%02d %02d:%02d", slot, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return 1;
}

/* ---- the network game, from the menu -------------------------------------- */

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
    if (MessageBoxA(wnd, "This starts Theme Park again for the network game.\n"
                    "The park you are playing now is not kept (save a state first if you want it).",
                    "Network game", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
    char exe[MAX_PATH], cmd[MAX_PATH + 320];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --nointro %s", exe, args);   /* seen once already (#37) */
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxA(wnd, "Could not start Theme Park again.", "Network game", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ExitProcess(0);
}

enum { ID_NET = 700, ID_NET_JOIN = 710, ID_NET_RESUME, ID_NET_P = 720 };   /* ID_NET + players: host */
static const int give_amounts[4] = { 1000, 5000, 10000, 50000 }, grant_amounts[3] = { 1000, 10000, 50000 };

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
        GetPrivateProfileStringA("net", "join", "127.0.0.1", addr, sizeof addr, ini_path);
        if (!ask_text("Join a network game", "The host's address (name or IP, and :port if not 7790):", addr, sizeof addr) || !addr[0]) return;
        WritePrivateProfileStringA("net", "join", addr, ini_path);
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

/* F10, or a right-click on the bars: every setting in one native menu. */
static void settings_menu(int sx, int sy) {
    enum { ID_SCALE = 100, ID_SPEED = 200, ID_VIVID = 300, ID_FULL, ID_MUTE, ID_HIRES,
           ID_SIZE = 400, ID_SAVE = 500, ID_LOAD = 600, ID_MUSIC = 800, ID_RES = 900, ID_RULES = 1000, ID_LANG = 1100 };
    static const char* scale_labels[SCALE_COUNT] = {
        "Sharp (crisp pixels, smooth edges)", "Smooth", "CRT", "Nearest", "Integer (pixels doubled up)" };
    capture(0);
    HMENU m = CreatePopupMenu(), sc = CreatePopupMenu(), sp = CreatePopupMenu(), sz = CreatePopupMenu(),
          sv = CreatePopupMenu(), ld = CreatePopupMenu(), rs = CreatePopupMenu(), mu = CreatePopupMenu(),
          ru = CreatePopupMenu(), la = CreatePopupMenu();
    /* The game's languages (LANGn-0.DAT), read at start-up. */
    static const char* langs[5] = { "English", "Deutsch", "Italiano", "Espa\xF1ol", "Fran\xE7" "ais" };
    int lang = GetPrivateProfileIntA("game", "lang", 0, ini_path);
    for (int i = 0; i < 5; i++) AppendMenuA(la, MF_STRING | (lang == i ? MF_CHECKED : 0), ID_LANG + i, langs[i]);
    AppendMenuA(la, MF_SEPARATOR, 0, NULL);
    AppendMenuA(la, MF_STRING | MF_GRAYED, 0, "From the next start");
    /* Modern rules (rules.c): in a network game, the host's, and fixed. */
    for (int i = 0; i < rule_count; i++)
        AppendMenuA(ru, MF_STRING | (tp_rules & rule_list[i].bit ? MF_CHECKED : 0) | (net_n ? MF_GRAYED : 0),
                    ID_RULES + i, rule_list[i].label);
    AppendMenuA(ru, MF_SEPARATOR, 0, NULL);
    AppendMenuA(ru, MF_STRING | MF_GRAYED, 0, net_n ? "The host's rules, for the whole game" :
                "Off: the game as in 1994 (docs\\rules.md)");
    char res_ini[16];
    GetPrivateProfileStringA("game", "res", "640x480", res_ini, sizeof res_ini, ini_path);
    int cur = 640, ticked = 0;
    sscanf(res_ini, "%d", &cur);
    for (int i = 0; i < (int)(sizeof res_list / sizeof res_list[0]); i++) {
        int w = res_list[i] ? res_list[i] : res_fit(), on = !ticked && w == cur;
        char t[64];
        if (res_list[i]) snprintf(t, sizeof t, "%s", res_names[i]);
        else snprintf(t, sizeof t, "%s (%d x 480)", res_names[i], w);
        AppendMenuA(rs, MF_STRING | (on ? MF_CHECKED : 0), ID_RES + i, t);
        ticked |= on;
    }
    static char packs[32][64];
    int npacks = music_packs(packs, 32);
    AppendMenuA(mu, MF_STRING | (!music_pack()[0] ? MF_CHECKED : 0), ID_MUSIC, "The game's own (MIDI)");
    for (int i = 0; i < npacks; i++)
        AppendMenuA(mu, MF_STRING | (!_stricmp(music_pack(), packs[i]) ? MF_CHECKED : 0), ID_MUSIC + 1 + i, packs[i]);
    if (!npacks) AppendMenuA(mu, MF_STRING | MF_GRAYED, 0, "No packs in the music folder (music\\README.md)");
    for (int k = 2; k <= max_window_k(); k += 2) {
        char t[48];
        snprintf(t, sizeof t, "%dx\t%d x %d", k / 2, step_w() * k, 240 * k);
        AppendMenuA(sz, MF_STRING | (!g_present.fullscreen && cw == step_w() * k && ch == 240 * k ? MF_CHECKED : 0), ID_SIZE + k, t);
    }
    for (int slot = 1; slot <= 9; slot++) {
        char t[64];
        int used = slot_label(slot, t, sizeof t);
        AppendMenuA(sv, MF_STRING, ID_SAVE + slot, t);
        AppendMenuA(ld, MF_STRING | (used && !net_n ? 0 : MF_GRAYED), ID_LOAD + slot, t);   /* not in a network game: it would leave the others */
    }
    for (int i = 0; i < SCALE_COUNT; i++)
        AppendMenuA(sc, MF_STRING | (g_present.scale == i ? MF_CHECKED : 0), ID_SCALE + i, scale_labels[i]);
    for (int k = 0; k < (int)(sizeof speeds / sizeof speeds[0]); k++) {
        char t[48];
        snprintf(t, sizeof t, "%d fps%s", speeds[k], speeds[k] == 25 ? " (original)" : "");
        AppendMenuA(sp, MF_STRING | ((int)game_fps == speeds[k] ? MF_CHECKED : 0), ID_SPEED + k, t);
    }
    char ini_hires[8];
    GetPrivateProfileStringA("game", "hires", "1", ini_hires, sizeof ini_hires, ini_path);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sv, "Save state");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ld, "Load state");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)network_menu(), "Network game");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sz, "Window size");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sc, "Scaling\tF12");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sp, "Speed\tCtrl+PgUp/PgDn");
    AppendMenuA(m, MF_STRING | (g_present.look ? MF_CHECKED : 0), ID_VIVID, "Vivid colour\tF9");
    AppendMenuA(m, MF_STRING | (g_present.fullscreen ? MF_CHECKED : 0), ID_FULL, "Fullscreen\tF11");
    AppendMenuA(m, MF_STRING | (muted ? MF_CHECKED : 0), ID_MUTE, "Mute");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)mu, "Music");
    AppendMenuA(m, MF_STRING | (ini_hires[0] != '0' ? MF_CHECKED : 0), ID_HIRES,
                "Open parks in 640x480 (R switches in a park)");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)rs, "Park resolution");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)la, "Language");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ru, "Modern rules");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, wnd, NULL);
    DestroyMenu(m);
    if (cmd >= ID_NET && cmd < ID_NET_P + 80) { network_command(cmd); return; }
    if (cmd > ID_SAVE && cmd <= ID_SAVE + 9) { state_request = cmd - ID_SAVE; return; }
    if (cmd > ID_LOAD && cmd <= ID_LOAD + 9) { state_request = -(cmd - ID_LOAD); return; }
    if (cmd >= ID_SIZE && cmd < ID_SAVE) {
        char v[8];
        size_window(cmd - ID_SIZE);
        snprintf(v, sizeof v, "%d", cmd - ID_SIZE);
        WritePrivateProfileStringA("display", "window", v, ini_path);
    }
    else if (cmd >= ID_SCALE && cmd < ID_SCALE + SCALE_COUNT) { g_present.scale = cmd - ID_SCALE; layout(); }
    else if (cmd >= ID_SPEED && cmd < ID_VIVID) set_speed(speeds[cmd - ID_SPEED]);
    else if (cmd == ID_VIVID) g_present.look ^= 1;
    else if (cmd == ID_FULL) apply_fullscreen(!g_present.fullscreen);
    else if (cmd == ID_MUTE) { muted ^= 1; sound_set_mute(muted); WritePrivateProfileStringA("sound", "mute", muted ? "1" : "0", ini_path); }
    else if (cmd == ID_HIRES) WritePrivateProfileStringA("game", "hires", ini_hires[0] != '0' ? "0" : "1", ini_path);
    else if (cmd >= ID_RES && cmd < ID_RES + (int)(sizeof res_list / sizeof res_list[0])) {
        /* Takes effect when the game next sets 640x480 up: a park opening, or R. */
        int w = res_list[cmd - ID_RES] ? res_list[cmd - ID_RES] : res_fit();
        char v[16];
        snprintf(v, sizeof v, "%dx480", w);
        WritePrivateProfileStringA("game", "res", v, ini_path);
        res_w = w > 640 ? w : 0;
        snprintf(state_note, 96, "park resolution %d x 480: at the next park, or R", w);
        state_note_until = GetTickCount() + 4000;
        if (!g_present.fullscreen && ch >= 480) size_window(ch / 240);   /* the same scale, the new shape */
    }
    else if (cmd >= ID_LANG && cmd < ID_LANG + 5) {
        char v[4] = { (char)('0' + cmd - ID_LANG), 0 };
        WritePrivateProfileStringA("game", "lang", v, ini_path);
        snprintf(state_note, 96, "language: from the next start");
        state_note_until = GetTickCount() + 4000;
    }
    else if (cmd >= ID_RULES && cmd < ID_RULES + rule_count && !net_n) {
        tp_rules ^= rule_list[cmd - ID_RULES].bit;
        rules_save(ini_path);
    }
    else if (cmd >= ID_MUSIC && cmd <= ID_MUSIC + npacks) {
        const char* p = cmd == ID_MUSIC ? "" : packs[cmd - ID_MUSIC - 1];
        sound_pick_music(p);
        WritePrivateProfileStringA("sound", "music", p, ini_path);
    }
    if (cmd) { save_settings(); title_until = GetTickCount() + 2000; }
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: cw = LOWORD(lp); ch = HIWORD(lp); layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ValidateRect(h, NULL); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { apply_fullscreen(!g_present.fullscreen); save_settings(); return 0; }
        if (wp == VK_F10) {               /* F10 is ours: the settings menu, not the system menu */
            POINT p = { (dst.left + dst.right) / 2, (dst.top + dst.bottom) / 2 };
            ClientToScreen(h, &p);
            settings_menu(p.x, p.y);
            return 0;
        }
        /* fall through: Alt is a game key */
    case WM_KEYDOWN:
        if (wp == VK_F11) { apply_fullscreen(!g_present.fullscreen); save_settings(); return 0; }
        if (wp == VK_F12) {
            g_present.scale = (g_present.scale + 1) % SCALE_COUNT;
            layout(); save_settings(); title_until = GetTickCount() + 2000; return 0;
        }
        if (wp == VK_F9) { g_present.look ^= 1; save_settings(); title_until = GetTickCount() + 2000; return 0; }
        if ((wp == VK_PRIOR || wp == VK_NEXT) && GetKeyState(VK_CONTROL) < 0) {   /* speed */
            speed_step(wp == VK_PRIOR);
            return 0;
        }
        key(lp, 1);   /* repeats too: a held key types again, as on a PC keyboard */
        return 0;
    case WM_SYSKEYUP: case WM_KEYUP:
        if (wp == VK_F9 || wp == VK_F11 || wp == VK_F12) return 0;
        key(lp, 0);
        return 0;
    case WM_MOUSEMOVE:
        if (captured) pointer_moved(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) capture(0);
        break;
    case WM_MOVE: if (captured) clip(); return 0;
    case WM_RBUTTONDOWN: {               /* on the bars: the settings menu */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!captured && !PtInRect(&dst, p)) { ClientToScreen(h, &p); settings_menu(p.x, p.y); return 0; }
        } /* fall through */
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!captured) {                 /* the click that takes the mouse is not the game's */
            if (m == WM_LBUTTONDOWN && PtInRect(&dst, p)) capture(1);
            return 0;
        }
        buttons = (wp & MK_LBUTTON ? 1 : 0) | (wp & MK_RBUTTON ? 2 : 0) | (wp & MK_MBUTTON ? 4 : 0);
        dos32_mouse_move(0, 0, buttons);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }   /* the game draws its own */
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, wp, lp);
}

void present_run(void) {
    SetProcessDPIAware();
    WNDCLASSA wc = { CS_HREDRAW | CS_VREDRAW, proc, 0, 0, GetModuleHandle(NULL), NULL,
                     LoadCursor(NULL, IDC_ARROW), NULL, NULL, "themepark" };
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    /* The size chosen in the menu, else the largest whole multiple of
     * 640x480 that fits the work area (2x, 1280x960, on a 1080p screen). */
    int k = GetPrivateProfileIntA("display", "window", 0, ini_path), most = max_window_k();
    if (k < 1 || k > most) k = most >= 2 ? most & ~1 : most;
    wnd = CreateWindowA("themepark", "Theme Park", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        640, 480, NULL, NULL, wc.hInstance, NULL);
    int fs = g_present.fullscreen;
    size_window(k);                  /* (which leaves fullscreen: back to it below) */
    ShowWindow(wnd, SW_SHOW);
    if (fs) apply_fullscreen(1);
    if ((muted = GetPrivateProfileIntA("sound", "mute", 0, ini_path))) sound_set_mute(1);
    for (;;) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (IsIconic(wnd)) { Sleep(50); continue; }
        render();
    }
}

void present_close(void) { if (wnd) PostMessage(wnd, WM_CLOSE, 0, 0); }

/* --present-test DIR: one frame through each scaling mode into a hidden
 * 1440x1080 window, each read back to DIR/<mode>.bmp. Returns 0 when Direct3D
 * drew all of them. */
extern const char* snapshot_path;
int present_test(const char* dir) {
    WNDCLASSA wc = { 0, DefWindowProcA, 0, 0, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "tp_test" };
    RegisterClassA(&wc);
    wnd = CreateWindowA("tp_test", "", WS_POPUP, 0, 0, 1440, 1080, NULL, NULL, wc.hInstance, NULL);
    cw = 1440; ch = 1080;
    int w, h, bad = 0;
    if (dos32_frame(frame, 1920, 1080, &w, &h)) { fw = w; fh = h; lights_apply(frame, w, h); }
    for (int m = 0; m < SCALE_COUNT; m++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s.bmp", dir, scale_names[m]);
        g_present.scale = m;
        layout();
        snapshot_path = path;
        if (!d3d_present(wnd, frame, fw, fh, &dst, cw, ch, m, 0, 0)) bad++;
        else fprintf(stderr, "present-test: %s\n", path);
    }
    DestroyWindow(wnd);
    return bad;
}
