/*
 * The window: dos32's screen, letterboxed and scaled with GDI, and the
 * player's keys and mouse handed back to the game.
 *
 *   F11, Alt+Enter   borderless fullscreen
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>

#include "dos32.h"
#include "present.h"

static HWND wnd;
static uint32_t frame[1024 * 768];
static int fw = 320, fh = 200;
static RECT dst;                 /* where the picture lands in the client area */
static int fullscreen;
static WINDOWPLACEMENT saved_place = { sizeof saved_place };

static void layout(int cw, int ch) {
    /* Keep 4:3 whatever the mode's pixel aspect: 320x200 and 640x400 were
     * shown on 4:3 monitors with tall pixels. */
    int w = cw, h = cw * 3 / 4;
    if (h > ch) { h = ch; w = ch * 4 / 3; }
    dst.left = (cw - w) / 2; dst.top = (ch - h) / 2;
    dst.right = dst.left + w; dst.bottom = dst.top + h;
}

static void toggle_fullscreen(void) {
    DWORD st = GetWindowLong(wnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(wnd, &saved_place);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &saved_place);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = !fullscreen;
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

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: layout(LOWORD(lp), HIWORD(lp)); InvalidateRect(h, NULL, TRUE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        int w, ht;
        if (dos32_frame(frame, 1024, 768, &w, &ht)) { fw = w; fh = ht; }
        InvalidateRect(h, NULL, FALSE);
        return 0; }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), fw, -fh, 1, 32, BI_RGB } };
        HBRUSH black = GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { 0, 0, rc.right, dst.top }, { 0, dst.bottom, rc.right, rc.bottom },
                         { 0, dst.top, dst.left, dst.bottom }, { dst.right, dst.top, rc.right, dst.bottom } };
        for (int i = 0; i < 4; i++) FillRect(dc, &bars[i], black);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                      0, 0, fw, fh, frame, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0; }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(); return 0; }
        /* fall through: Alt is a game key */
    case WM_KEYDOWN:
        if (wp == VK_F11) { toggle_fullscreen(); return 0; }
        if (!(lp & (1 << 30))) dos32_key(((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0xE000 : 0), 1);
        return 0;
    case WM_SYSKEYUP: case WM_KEYUP:
        dos32_key(((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0xE000 : 0), 0);
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
    RegisterClassA(&wc);
    RECT r = { 0, 0, 1280, 960 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("themepark", "Theme Park", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SetTimer(wnd, 1, 16, NULL);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
}

void present_close(void) { if (wnd) PostMessage(wnd, WM_CLOSE, 0, 0); }
