/*
 * The window: dos32's screen through our own presenter, and the player's keys
 * and mouse handed back to the game.
 *
 *   F12              scaling: sharp-bilinear (default), smooth, CRT, nearest, integer
 *   F9               colour: natural / vivid
 *   Ctrl+PgUp/PgDn   speed: the game's frames per second, 12 to 120 (default 25)
 *   F11, Alt+Enter   borderless fullscreen
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

present_settings g_present = { SCALE_SHARP, 0, 0, 1, 0 };

static const char* scale_names[SCALE_COUNT] = { "sharp", "smooth", "crt", "nearest", "integer" };
static char ini_path[MAX_PATH];
static HWND wnd;
static uint32_t frame[1024 * 768];
static int fw = 320, fh = 200;
static RECT dst;                 /* where the picture lands in the client area */
static int cw, ch;
static WINDOWPLACEMENT saved_place = { sizeof saved_place };
static DWORD title_until;        /* show the scaling mode in the title for a moment */
extern double game_fps;          /* game.c: the speed control */
static const int speeds[] = { 12, 15, 20, 25, 30, 35, 40, 50, 60, 90, 120 };

int present_scale_from_name(const char* name) {
    for (int i = 0; i < SCALE_COUNT; i++) if (!_stricmp(name, scale_names[i])) return i;
    return -1;
}

void present_load_settings(void) {
    char buf[64];
    GetModuleFileNameA(NULL, ini_path, MAX_PATH);
    strcpy(strrchr(ini_path, '\\') + 1, "themepark.ini");
    GetPrivateProfileStringA("display", "scale", "sharp", buf, sizeof buf, ini_path);
    int s = present_scale_from_name(buf);
    g_present.scale = s < 0 ? SCALE_SHARP : s;
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
    } else {
        /* 4:3 whatever the mode's pixel aspect: 320x200 and 640x400 were shown
         * on 4:3 monitors with tall pixels. */
        w = cw; h = cw * 3 / 4;
        if (h > ch) { h = ch; w = ch * 4 / 3; }
    }
    dst.left = (cw - w) / 2; dst.top = (ch - h) / 2;
    dst.right = dst.left + w; dst.bottom = dst.top + h;
}

static void set_title(void) {
    char t[128];
    if (GetTickCount() < title_until)
        snprintf(t, sizeof t, "Theme Park - %s%s - speed %.0f fps", scale_names[g_present.scale],
                 g_present.look ? ", vivid" : "", game_fps);
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

static void mouse_to_game(LPARAM lp, WPARAM wp) {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return;
    int gx = (x - dst.left) * fw / dw, gy = (y - dst.top) * fh / dh;
    if (gx < 0) gx = 0; if (gx >= fw) gx = fw - 1;
    if (gy < 0) gy = 0; if (gy >= fh) gy = fh - 1;
    dos32_mouse(gx, gy, (wp & MK_LBUTTON ? 1 : 0) | (wp & MK_RBUTTON ? 2 : 0) | (wp & MK_MBUTTON ? 4 : 0));
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

static void render(void) {
    int w, h;
    if (dos32_frame(frame, 1024, 768, &w, &h) && (w != fw || h != fh)) { fw = w; fh = h; layout(); }
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

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: cw = LOWORD(lp); ch = HIWORD(lp); layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ValidateRect(h, NULL); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { apply_fullscreen(!g_present.fullscreen); save_settings(); return 0; }
        if (wp == VK_F10) return 0;       /* F10 would open the system menu */
        /* fall through: Alt is a game key */
    case WM_KEYDOWN:
        if (wp == VK_F11) { apply_fullscreen(!g_present.fullscreen); save_settings(); return 0; }
        if (wp == VK_F12) {
            g_present.scale = (g_present.scale + 1) % SCALE_COUNT;
            layout(); save_settings(); title_until = GetTickCount() + 2000; return 0;
        }
        if (wp == VK_F9) { g_present.look ^= 1; save_settings(); title_until = GetTickCount() + 2000; return 0; }
        if ((wp == VK_PRIOR || wp == VK_NEXT) && GetKeyState(VK_CONTROL) < 0) {   /* speed */
            int n = sizeof speeds / sizeof speeds[0], k = 0;
            while (k < n - 1 && speeds[k] < game_fps) k++;
            k = wp == VK_PRIOR ? (k + 1 < n ? k + 1 : k) : (k > 0 ? k - 1 : 0);
            game_fps = speeds[k];
            char v[16]; snprintf(v, sizeof v, "%d", speeds[k]);
            WritePrivateProfileStringA("game", "fps", v, ini_path);
            title_until = GetTickCount() + 2000;
            return 0;
        }
        if (!(lp & (1 << 30))) key(lp, 1);
        return 0;
    case WM_SYSKEYUP: case WM_KEYUP:
        if (wp == VK_F9 || wp == VK_F11 || wp == VK_F12) return 0;
        key(lp, 0);
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) SetCapture(h);
        if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) ReleaseCapture();
        mouse_to_game(lp, wp);
        return 0;
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
    /* The largest 4:3 window that fits the work area at a whole multiple of 320x240. */
    RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    int k = 4;
    while (k > 1 && (320 * k > wa.right - wa.left - 40 || 240 * k > wa.bottom - wa.top - 80)) k--;
    RECT r = { 0, 0, 320 * k, 240 * k };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("themepark", "Theme Park", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(wnd, SW_SHOW);
    if (g_present.fullscreen) apply_fullscreen(1);
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
    if (dos32_frame(frame, 1024, 768, &w, &h)) { fw = w; fh = h; }
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
