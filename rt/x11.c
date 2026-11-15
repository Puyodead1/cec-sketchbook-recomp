/* The subset of Xlib the game uses, implemented on a Win32 window.
 * Structures handed to the guest use the i386 Xlib layouts (Display, Screen, Visual, XImage, XEvent...).
 * The X server is modelled as one 16bpp TrueColor (RGB565) screen whose root window is our Win32 window. */
#include "rt.h"
#include <ctype.h>

#define WIN_ROOT 0x100u
#define WIN_TOP 0x200001u

static int SW = 640, SH = 480;
static u16 *fb;                     /* SW*SH RGB565 */
static CRITICAL_SECTION xcs;
static HWND hwnd;
static HANDLE ui_ready;
static volatile int cursor_hidden;

static u8 *dpy, *scr, *vis;
static u32 default_gc;

/* ---------------- events */
typedef struct { int type; u32 keycode; u32 state; u32 x, y; } XEv;
static XEv evq[256];
static int evh, evt;
static u32 kc_sym[256], kc_sym_shift[256];
static char kc_chr[256][4];
static u32 ev_serial;
static volatile u32 win_mask = 5;

static void ev_push(int type, u32 keycode, u32 state, u32 x, u32 y) {
    EnterCriticalSection(&xcs);
    if (((evt + 1) & 255) != evh) {
        evq[evt].type = type; evq[evt].keycode = keycode; evq[evt].state = state; evq[evt].x = x; evq[evt].y = y;
        evt = (evt + 1) & 255;
    }
    LeaveCriticalSection(&xcs);
}

static u32 vk_keysym(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return vk + 32;
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= VK_F1 && vk <= VK_F12) return 0xFFBE + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return 0xFFB0 + (vk - VK_NUMPAD0);
    switch (vk) {
    case VK_BACK: return 0xFF08; case VK_TAB: return 0xFF09; case VK_RETURN: return 0xFF0D;
    case VK_ESCAPE: return 0xFF1B; case VK_DELETE: return 0xFFFF; case VK_HOME: return 0xFF50;
    case VK_LEFT: return 0xFF51; case VK_UP: return 0xFF52; case VK_RIGHT: return 0xFF53;
    case VK_DOWN: return 0xFF54; case VK_PRIOR: return 0xFF55; case VK_NEXT: return 0xFF56;
    case VK_END: return 0xFF57; case VK_INSERT: return 0xFF63; case VK_SPACE: return 0x20;
    case VK_SHIFT: case VK_LSHIFT: return 0xFFE1; case VK_RSHIFT: return 0xFFE2;
    case VK_CONTROL: case VK_LCONTROL: return 0xFFE3; case VK_RCONTROL: return 0xFFE4;
    case VK_MENU: return 0xFFE9; case VK_CAPITAL: return 0xFFE5;
    case VK_MULTIPLY: return 0xFFAA; case VK_ADD: return 0xFFAB; case VK_SUBTRACT: return 0xFFAD;
    case VK_DECIMAL: return 0xFFAE; case VK_DIVIDE: return 0xFFAF;
    }
    {
        UINT ch = MapVirtualKeyA(vk, 2) & 0x7fff;   /* MAPVK_VK_TO_CHAR */
        if (ch >= 0x20 && ch < 0x7f) return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
    }
    return 0;
}

static u32 cur_state(void) {
    u32 s = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) s |= 1;
    if (GetKeyState(VK_CAPITAL) & 1) s |= 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) s |= 4;
    if (GetKeyState(VK_MENU) & 0x8000) s |= 8;
    return s;
}

static void key_event(int down, WPARAM vk, LPARAM lp) {
    u32 scan = (lp >> 16) & 0xff, kc = (scan + 8) & 0xff, st = cur_state();
    BYTE ks[256];
    WORD out[2] = { 0, 0 };
    int n;
    if (lp & 0x01000000) kc = (kc | 0x80) & 0xff;   /* extended keys get their own keycodes */
    /* UIB box: number row 1-8 = buttons, C = coin (hold Ctrl to type them into the game instead) */
    if (down && !(st & 4)) {
        if (vk >= '1' && vk <= '8') { uib_press((int)(vk - '0')); return; }
        if (vk == 'C') { uib_coin(1); return; }
    }
    kc_sym[kc] = vk_keysym((UINT)vk);
    GetKeyboardState(ks);
    ks[VK_CONTROL] = ks[VK_LCONTROL] = ks[VK_RCONTROL] = 0;
    n = ToAscii((UINT)vk, scan, ks, out, 0);
    kc_chr[kc][0] = n == 1 ? (char)out[0] : 0;
    if ((st & 4) && n == 1 && isalpha((u8)out[0])) kc_chr[kc][0] = (char)(toupper((u8)out[0]) - '@');
    if (vk == VK_RETURN) kc_chr[kc][0] = '\r';
    ev_push(down ? 2 : 3, kc, st, 0, 0);
}

/* ---------------- window */
/* Drawing calls only mark the screen dirty; the UI thread repaints at most once per timer tick (~60 Hz).
   Invalidating on every call (the game pushes the screen one row per XPutImage, ~200 per pen step) made the
   UI thread repaint continuously and hold xcs, stalling the game thread. */
static volatile LONG dirty;
static void present(void) { dirty = 1; }
#define PRESENT_TIMER 2

/* ---------------- camera overlay (see cam.c)
   The bttv card DMAs video straight into the framebuffer inside the video window, except where the UI is
   drawn on top. We keep the video in its own buffer plus a per-pixel mask saying "this pixel shows video":
   going live marks the black pixels of the window as video; while live, anything the game draws inside the
   window takes the pixel back (drawing black hands it to the video again); outside of live, drawing just
   takes the pixel. Freeze/stop simply stop updating the video buffer, so the last frame stays until drawn over. */
static u16 *cam;                       /* SW*SH RGB565, valid inside the video window */
static u8 *vmask;                      /* SW*SH: 1 = show cam[] instead of fb[] */
static int cam_live, cx, cy, cw, ch;
static u16 vpix_i(int i) { return vmask && vmask[i] ? cam[i] : fb[i]; }
static u16 vpix(int x, int y) { return vpix_i(y * SW + x); }
#define IN_CAM(x, y) ((x) >= cx && (y) >= cy && (x) < cx + cw && (y) < cy + ch)
static void fb_put(int x, int y, u16 p) {   /* every framebuffer write goes through here */
    int i = y * SW + x;
    fb[i] = p;
    if (vmask) vmask[i] = (u8)(cam_live && !p && IN_CAM(x, y));
}

void x11_screen_size(int *w, int *h) { *w = SW; *h = SH; }

void x11_cam_rect(int x, int y, int w, int h) {
    if (!fb) return;
    EnterCriticalSection(&xcs);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    cx = x; cy = y; cw = w > 0 ? w : 0; ch = h > 0 ? h : 0;
    LeaveCriticalSection(&xcs);
}

void x11_cam_mode(int live) {
    int x, y;
    if (!fb) return;
    EnterCriticalSection(&xcs);
    if (!cam) { cam = (u16 *)calloc(SW * SH, 2); vmask = (u8 *)calloc(SW * SH, 1); }
    if (live && !cam_live)
        for (y = 0; y < SH; y++)
            for (x = 0; x < SW; x++) {
                int i = y * SW + x;
                if (!IN_CAM(x, y)) vmask[i] = 0;
                else if (!fb[i]) vmask[i] = 1;   /* pixels still showing the old frame stay video too */
            }
    cam_live = live;
    LeaveCriticalSection(&xcs);
    present();
}

/* new frame: 32-bit xRGB, w*h, stride in pixels; centre-cropped to the window's aspect and scaled into it */
void x11_cam_frame(const u32 *px, int w, int h, int stride, int mirror, int vflip) {
    int x, y, sx0, sy0, sw, sh;
    static int *xmap, xmap_n;
    int rx, ry, rw, rh;
    if (!fb || !cam || !cam_live || !cw || !ch || w <= 0 || h <= 0) return;
    EnterCriticalSection(&xcs);
    rx = cx; ry = cy; rw = cw; rh = ch;
    LeaveCriticalSection(&xcs);
    /* scale into a private row first (no lock held), then copy rows in under the lock */
    if ((long long)w * rh > (long long)h * rw) { sh = h; sw = (int)((long long)h * rw / rh); }
    else { sw = w; sh = (int)((long long)w * rh / rw); }
    sx0 = (w - sw) / 2; sy0 = (h - sh) / 2;
    if (xmap_n < rw) { free(xmap); xmap = (int *)malloc(rw * sizeof(int)); xmap_n = rw; }
    for (x = 0; x < rw; x++) { int sx = sx0 + x * sw / rw; xmap[x] = mirror ? w - 1 - sx : sx; }
    EnterCriticalSection(&xcs);
    if (rx == cx && ry == cy && rw == cw && rh == ch)
        for (y = 0; y < rh; y++) {
            int sy = sy0 + y * sh / rh;
            const u32 *row = px + (long)(vflip ? h - 1 - sy : sy) * stride;
            u16 *d = cam + (ry + y) * SW + rx;
            for (x = 0; x < rw; x++) {
                u32 p = row[xmap[x]];
                d[x] = (u16)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x1F));
            }
        }
    LeaveCriticalSection(&xcs);
    if (hwnd) {   /* repaint just the video window */
        RECT rc, r;
        GetClientRect(hwnd, &rc);
        r.left = rx * rc.right / SW; r.top = ry * rc.bottom / SH;
        r.right = (rx + rw) * rc.right / SW + 1; r.bottom = (ry + rh) * rc.bottom / SH + 1;
        InvalidateRect(hwnd, &r, FALSE);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        BITMAPINFO bi;
        int i;
        static u16 *snap;
        static u32 *px32, lut[65536];
        static int cap;
        int sw, sh;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        if (!lut[65535]) {
            for (i = 0; i < 65536; i++) {
                u32 r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
                lut[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
        EnterCriticalSection(&xcs);   /* only a quick copy under the lock (the video mode can change) */
        sw = SW; sh = SH;
        if (cap < sw * sh) {
            free(snap); free(px32);
            snap = (u16 *)malloc((size_t)sw * sh * 2); px32 = (u32 *)malloc((size_t)sw * sh * 4); cap = sw * sh;
        }
        if (vmask) for (i = 0; i < sw * sh; i++) snap[i] = vmask[i] ? cam[i] : fb[i];
        else memcpy(snap, fb, (size_t)sw * sh * 2);
        LeaveCriticalSection(&xcs);
        for (i = 0; i < sw * sh; i++) px32[i] = lut[snap[i]];
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = sw;
        bi.bmiHeader.biHeight = -sh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, sw, sh, px32, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DROPFILES: {   /* a picture dropped on the window becomes the camera image */
        HMODULE sh = GetModuleHandleA("shell32.dll");
        UINT (WINAPI *query)(HANDLE, UINT, LPSTR, UINT) = sh ? (void *)GetProcAddress(sh, "DragQueryFileA") : 0;
        void (WINAPI *finish)(HANDLE) = sh ? (void *)GetProcAddress(sh, "DragFinish") : 0;
        char path[MAX_PATH];
        extern int rt_cam_use_image(const char *path);
        if (query && query((HANDLE)w, 0, path, sizeof path)) rt_cam_use_image(path);
        if (finish) finish((HANDLE)w);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && cursor_hidden) { SetCursor(0); return TRUE; }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) break;
        key_event(1, w, l);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        key_event(0, w, l);
        return 0;
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
        RECT rc;
        u32 x, y, b = m == WM_LBUTTONDOWN ? 1 : m == WM_MBUTTONDOWN ? 2 : 3;
        GetClientRect(h, &rc);
        x = (u32)(LOWORD(l) * SW / (rc.right ? rc.right : 1));
        y = (u32)(HIWORD(l) * SH / (rc.bottom ? rc.bottom : 1));
        ev_push(4, b, cur_state(), x, y);
        return 0;
    }
    case WM_CLOSE:
        rt_logf("window closed\n");
        rt_raise(GSIG_TERM);
        SetTimer(h, 1, 3000, 0);
        return 0;
    case WM_TIMER:
        if (w == PRESENT_TIMER) {
            if (InterlockedExchange(&dirty, 0)) InvalidateRect(h, 0, FALSE);
            return 0;
        }
        ExitProcess(0);
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI ui_thread(void *arg) {
    WNDCLASSA wc;
    RECT r;
    MSG msg;
    int scale = getenv("VEND_SCALE") ? atoi(getenv("VEND_SCALE")) : 1;
    (void)arg;
    if (scale < 1) scale = 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, IDC_ARROW);
    wc.lpszClassName = "SketchBook";
    RegisterClassA(&wc);
    r.left = 0; r.top = 0;   /* sized for the largest video mode (800x600); smaller modes are scaled up */
    r.right = (SW < 800 ? 800 : SW) * scale; r.bottom = (SH < 600 ? 600 : SH) * scale;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("SketchBook", "Sketch Book  [C: coin, 1-8: buttons, drop a picture here to sketch it]",
                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    SetTimer(hwnd, PRESENT_TIMER, 15, 0);
    {   /* drag and drop of pictures (shell32 loaded on demand: nothing else needs it) */
        HMODULE sh = LoadLibraryA("shell32.dll");
        void (WINAPI *accept)(HWND, BOOL) = sh ? (void *)GetProcAddress(sh, "DragAcceptFiles") : 0;
        if (accept) accept(hwnd, TRUE);
    }
    SetEvent(ui_ready);
    while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

static void ensure_window(void) {
    static int done;
    if (done) return;
    done = 1;
    ui_ready = CreateEventA(0, TRUE, FALSE, 0);
    CreateThread(0, 0, ui_thread, 0, 0, 0);
    WaitForSingleObject(ui_ready, INFINITE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}

/* ---------------- GC */
typedef struct { u32 ext_data, gid; u32 fg, bg, lw; } GCRec;
static u32 new_gc(void) {
    GCRec *g = (GCRec *)calloc(1, sizeof(GCRec));
    g->gid = 0x300000 + (u32)(uintptr_t)g % 0xfffff;
    g->bg = 0xffff;
    g->lw = 1;
    return (u32)(uintptr_t)g;
}

/* ---------------- display */
DECL_IMP(XOpenDisplay) {
    if (!dpy) {
        static char name[] = ":0";
        static char vendor[] = "Sketch Win32 GDI";
        if (getenv("VEND_W")) SW = atoi(getenv("VEND_W"));
        if (getenv("VEND_H")) SH = atoi(getenv("VEND_H"));
        InitializeCriticalSection(&xcs);
        fb = (u16 *)calloc(SW * SH, 2);
        dpy = (u8 *)calloc(1, 4096);
        scr = (u8 *)calloc(1, 256);
        vis = (u8 *)calloc(1, 64);
        default_gc = new_gc();
        M32(vis + 4) = 0x21; M32(vis + 8) = 4;
        M32(vis + 12) = 0xF800; M32(vis + 16) = 0x07E0; M32(vis + 20) = 0x001F;
        M32(vis + 24) = 6; M32(vis + 28) = 64;
        M32(scr + 4) = (u32)(uintptr_t)dpy; M32(scr + 8) = WIN_ROOT;
        M32(scr + 12) = SW; M32(scr + 16) = SH; M32(scr + 20) = SW * 254 / 960; M32(scr + 24) = SH * 254 / 960;
        M32(scr + 28) = 1; M32(scr + 36) = 16; M32(scr + 40) = (u32)(uintptr_t)vis;
        M32(scr + 44) = default_gc; M32(scr + 48) = 0x20; M32(scr + 52) = 0xffff; M32(scr + 56) = 0;
        M32(scr + 60) = 1; M32(scr + 64) = 1; M32(scr + 68) = 0;
        M32(dpy + 8) = 100;   /* ConnectionNumber */
        M32(dpy + 16) = 11; M32(dpy + 24) = (u32)(uintptr_t)vendor;
        M32(dpy + 48) = 0; M32(dpy + 52) = 32; M32(dpy + 56) = 32; M32(dpy + 60) = 0;
        M32(dpy + 76) = 60802000;
        M32(dpy + 116) = 65535;
        M32(dpy + 128) = (u32)(uintptr_t)name;
        M32(dpy + 132) = 0; M32(dpy + 136) = 1; M32(dpy + 140) = (u32)(uintptr_t)scr;
        ensure_window();
        rt_logf("XOpenDisplay -> %p (%dx%d RGB565)\n", dpy, SW, SH);
    }
    RET(dpy);
}
DECL_IMP(XCloseDisplay) { RET(0); }
DECL_IMP(XDefaultScreen) { RET(0); }
DECL_IMP(XRootWindow) { RET(WIN_ROOT); }
DECL_IMP(XDefaultVisual) { RET(vis); }
DECL_IMP(XDefaultDepth) { RET(16); }
DECL_IMP(XDefaultColormap) { RET(0x20); }
DECL_IMP(XFlush) { present(); RET(0); }
DECL_IMP(XSync) { present(); RET(0); }
void rt_gfree(CPU *c, void *p);
DECL_IMP(XFree) { if (ARG(0) != (u32)(uintptr_t)vis) rt_gfree(c, ARGP(0)); RET(1); }

/* XMatchVisualInfo(dpy, screen, depth, class, XVisualInfo*) */
DECL_IMP(XMatchVisualInfo) {
    u32 vi = ARG(4);
    if (ARG(2) != 16 || ARG(3) != 4) RET(0);
    memset(ARGP(4), 0, 40);
    M32(vi + 0) = (u32)(uintptr_t)vis; M32(vi + 4) = 0x21; M32(vi + 8) = 0; M32(vi + 12) = 16; M32(vi + 16) = 4;
    M32(vi + 20) = 0xF800; M32(vi + 24) = 0x07E0; M32(vi + 28) = 0x1F; M32(vi + 32) = 64; M32(vi + 36) = 6;
    RET(1);
}

/* XCreateWindow(dpy, parent, x, y, w, h, border, depth, class, visual, valuemask, attrs) */
/* The game switches video modes (640x480 for the camera screens, 800x600 elsewhere: see 0x808CF90) by creating a
   window of the new size; on the machine the X server changed resolution. Here the framebuffer takes the new size and
   the host window keeps its own; the picture is scaled into it (both modes are 4:3). */
static void set_mode(int w, int h) {
    u16 *nfb;
    if ((w == SW && h == SH) || w < 320 || h < 200 || w > 2048 || h > 2048) return;
    if (!(nfb = (u16 *)calloc((size_t)w * h, 2))) return;
    EnterCriticalSection(&xcs);
    free(fb); fb = nfb;
    if (cam) {
        free(cam); free(vmask);
        cam = (u16 *)calloc((size_t)w * h, 2); vmask = (u8 *)calloc((size_t)w * h, 1);
    }
    SW = w; SH = h;
    if (cx + cw > SW) cw = SW > cx ? SW - cx : 0;
    if (cy + ch > SH) ch = SH > cy ? SH - cy : 0;
    if (scr) { M32(scr + 12) = SW; M32(scr + 16) = SH; M32(scr + 20) = SW * 254 / 960; M32(scr + 24) = SH * 254 / 960; }
    LeaveCriticalSection(&xcs);
    rt_logf("video mode %dx%d\n", w, h);
    present();
}
DECL_IMP(XCreateWindow) {
    u32 w = ARG(4), h = ARG(5), mask = ARG(10), at = ARG(11);
    rt_logf("XCreateWindow %ux%u depth %d mask %x\n", w, h, ARG(7), mask);
    if (fb) set_mode((int)w, (int)h);
    /* XSetWindowAttributes: event_mask at offset 40 */
    if (at && (mask & 0x800)) win_mask = M32(at + 40);
    RET(WIN_TOP);
}
DECL_IMP(XMapRaised) { if (hwnd) ShowWindow(hwnd, SW_SHOWNORMAL); present(); RET(0); }
DECL_IMP(XDestroyWindow) { RET(0); }
DECL_IMP(XChangeWindowAttributes) { RET(0); }
/* XWindowAttributes */
DECL_IMP(XGetWindowAttributes) {
    u32 a = ARG(2);
    memset(ARGP(2), 0, 92);
    M32(a + 8) = SW; M32(a + 12) = SH; M32(a + 20) = 16; M32(a + 24) = (u32)(uintptr_t)vis;
    M32(a + 28) = WIN_ROOT; M32(a + 32) = 1; M32(a + 60) = 0x20; M32(a + 64) = 1; M32(a + 68) = 2;
    M32(a + 72) = 5; M32(a + 76) = 5; M32(a + 88) = (u32)(uintptr_t)scr;
    RET(1);
}

/* ---------------- drawing */
DECL_IMP(XCreateGC) { RET(new_gc()); }
DECL_IMP(XFreeGC) { if (ARG(1) != default_gc) free(ARGP(1)); RET(0); }
DECL_IMP(XSetForeground) { ((GCRec *)ARGP(1))->fg = ARG(2); RET(1); }
DECL_IMP(XSetBackground) { ((GCRec *)ARGP(1))->bg = ARG(2); RET(1); }
DECL_IMP(XSetLineAttributes) { ((GCRec *)ARGP(1))->lw = ARG(2) ? ARG(2) : 1; RET(1); }

static void fill(int x, int y, int w, int h, u16 px) {
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) fb_put(x + i, y + j, px);
}
DECL_IMP(XFillRectangle) {
    GCRec *g = (GCRec *)ARGP(2);
    EnterCriticalSection(&xcs);
    fill((s32)ARG(3), (s32)ARG(4), (s32)ARG(5), (s32)ARG(6), (u16)g->fg);
    LeaveCriticalSection(&xcs);
    present();
    RET(0);
}
DECL_IMP(XDrawLine) {
    GCRec *g = (GCRec *)ARGP(2);
    int x0 = (s32)ARG(3), y0 = (s32)ARG(4), x1 = (s32)ARG(5), y1 = (s32)ARG(6);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int lw = (int)g->lw, o = lw / 2;
    EnterCriticalSection(&xcs);
    for (;;) {
        fill(x0 - o, y0 - o, lw, lw, (u16)g->fg);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    LeaveCriticalSection(&xcs);
    present();
    RET(0);
}

/* XImage (i386): width height xoffset format data byte_order bitmap_unit bitmap_bit_order bitmap_pad
 * depth bytes_per_line bits_per_pixel red_mask green_mask blue_mask obdata funcs[6] */
static void destroy_image(CPU *c) {
    u32 im = ARG(0);
    free((void *)(uintptr_t)M32(im + 16));
    free((void *)(uintptr_t)im);
    RET(1);
}
static void put_pixel_cb(CPU *c) { RET(0); }
static u32 get_pixel_cb_dummy;

DECL_IMP(XGetImage) {
    int x = (s32)ARG(2), y = (s32)ARG(3), w = (s32)ARG(4), h = (s32)ARG(5), j, i;
    u8 *im = (u8 *)calloc(1, 88);
    u16 *d = (u16 *)calloc((size_t)(w * h ? w * h : 1), 2);
    u32 a = (u32)(uintptr_t)im;
    (void)get_pixel_cb_dummy;
    EnterCriticalSection(&xcs);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            d[j * w + i] = (sx >= 0 && sy >= 0 && sx < SW && sy < SH) ? vpix(sx, sy) : 0;
        }
    LeaveCriticalSection(&xcs);
    M32(a + 0) = w; M32(a + 4) = h; M32(a + 8) = 0; M32(a + 12) = 2; M32(a + 16) = (u32)(uintptr_t)d;
    M32(a + 20) = 0; M32(a + 24) = 32; M32(a + 28) = 0; M32(a + 32) = 32; M32(a + 36) = 16;
    M32(a + 40) = w * 2; M32(a + 44) = 16; M32(a + 48) = 0xF800; M32(a + 52) = 0x07E0; M32(a + 56) = 0x1F;
    M32(a + 64) = 0;
    M32(a + 68) = rt_host_callback(destroy_image);
    M32(a + 72) = 0;
    M32(a + 76) = rt_host_callback(put_pixel_cb);
    RET(a);
}

/* XPutImage(dpy, d, gc, image, src_x, src_y, dest_x, dest_y, w, h) */
DECL_IMP(XPutImage) {
    u32 im = ARG(3);
    int sx = (s32)ARG(4), sy = (s32)ARG(5), dx = (s32)ARG(6), dy = (s32)ARG(7), w = (s32)ARG(8), h = (s32)ARG(9);
    int bpl = (s32)M32(im + 40), bpp = (s32)M32(im + 44), j, i;
    const u8 *data = (const u8 *)(uintptr_t)M32(im + 16);
    if (!data) RET(0);
    EnterCriticalSection(&xcs);
    for (j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= SH) continue;
        if (bpp == 16 && !cam_live) {   /* fast path: clip and copy the row */
            int x0 = dx < 0 ? -dx : 0, x1 = dx + w > SW ? SW - dx : w;
            if (x1 > x0) {
                memcpy(fb + ty * SW + dx + x0, data + (sy + j) * bpl + (sx + x0) * 2, (size_t)(x1 - x0) * 2);
                if (vmask) memset(vmask + ty * SW + dx + x0, 0, (size_t)(x1 - x0));
            }
            continue;
        }
        for (i = 0; i < w; i++) {
            int tx = dx + i;
            const u8 *p;
            if (tx < 0 || tx >= SW) continue;
            p = data + (sy + j) * bpl + (sx + i) * (bpp / 8);
            if (bpp == 16) fb_put(tx, ty, *(const u16 *)p);
            else if (bpp == 32 || bpp == 24) fb_put(tx, ty, (u16)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3)));
        }
    }
    LeaveCriticalSection(&xcs);
    present();
    RET(0);
}

/* ---------------- input */
/* XCheckWindowEvent(dpy, w, mask, XEvent*) */
DECL_IMP(XCheckWindowEvent) {
    u32 mask = ARG(2), e = ARG(3);
    for (;;) {
        XEv ev;
        int have = 0;
        u32 bit;
        EnterCriticalSection(&xcs);
        if (evh != evt) { ev = evq[evh]; evh = (evh + 1) & 255; have = 1; }
        LeaveCriticalSection(&xcs);
        if (!have) RET(0);
        bit = ev.type == 2 ? 1 : ev.type == 3 ? 2 : ev.type == 4 ? 4 : 0;
        if (!(mask & bit)) continue;
        memset(ARGP(3), 0, 96);
        M32(e + 0) = ev.type; M32(e + 4) = ++ev_serial; M32(e + 12) = (u32)(uintptr_t)dpy; M32(e + 16) = ARG(1);
        M32(e + 20) = WIN_ROOT; M32(e + 28) = GetTickCount();
        M32(e + 32) = ev.x; M32(e + 36) = ev.y; M32(e + 40) = ev.x; M32(e + 44) = ev.y;
        M32(e + 48) = ev.state; M32(e + 52) = ev.keycode; M32(e + 56) = 1;
        RET(1);
    }
}
DECL_IMP(XLookupKeysym) {
    u32 kc = M32(ARG(0) + 52) & 0xff;
    (void)kc_sym_shift;
    RET(kc_sym[kc]);
}
/* XLookupString(ev, buf, n, KeySym*, status) */
DECL_IMP(XLookupString) {
    u32 kc = M32(ARG(0) + 52) & 0xff;
    int n = (int)ARG(2), L = kc_chr[kc][0] ? 1 : 0;
    if (ARG(3)) M32(ARG(3)) = kc_sym[kc];
    if (L && n > 0) ARGS(1)[0] = kc_chr[kc][0];
    RET(L);
}
DECL_IMP(XQueryPointer) {
    int i;
    for (i = 1; i <= 7; i++) if (ARG(1 + i)) M32(ARG(1 + i)) = 0;
    if (ARG(2)) M32(ARG(2)) = WIN_ROOT;
    RET(1);
}
DECL_IMP(XWarpPointer) { RET(0); }
DECL_IMP(XGrabKeyboard) { RET(0); }
DECL_IMP(XUngrabKeyboard) { RET(0); }

/* ---------------- cursors / pixmaps / colormaps */
DECL_IMP(XCreatePixmapFromBitmapData) { RET(0x400001); }
DECL_IMP(XCreatePixmapCursor) { RET(0x500001); }
DECL_IMP(XFreePixmap) { RET(0); }
DECL_IMP(XFreeCursor) { RET(0); }
DECL_IMP(XDefineCursor) { cursor_hidden = 1; RET(0); }
DECL_IMP(XUndefineCursor) { cursor_hidden = 0; RET(0); }
DECL_IMP(XCreateColormap) { RET(0x20); }
DECL_IMP(XFreeColormap) { RET(0); }
DECL_IMP(XSetWindowColormap) { RET(0); }
DECL_IMP(XAllocColorCells) { RET(0); }
DECL_IMP(XStoreColor) { RET(0); }
DECL_IMP(XFreeColors) { RET(0); }

/* ---------------- extensions: report none (DGA etc. take their "missing" paths) */
DECL_IMP(XextCreateExtension) { RET(calloc(1, 16)); }
DECL_IMP(XextFindDisplay) { RET(0); }
DECL_IMP(XextAddDisplay) {
    u32 *info = (u32 *)calloc(1, 32);
    info[1] = ARG(1);   /* display */
    info[2] = 0;        /* codes: extension absent */
    RET(info);
}
DECL_IMP(XextRemoveDisplay) { RET(0); }
DECL_IMP(XMissingExtension) { rt_logf("XMissingExtension(%s)\n", ARGS(1) ? ARGS(1) : "?"); RET(0); }
DECL_IMP(XESetEventToWire) { RET(0); }
DECL_IMP(XESetWireToEvent) { RET(0); }
DECL_IMP(_XFlush) { RET_VOID(); }
DECL_IMP(_XReply) { RET(0); }
DECL_IMP(_XRead) { RET_VOID(); }
DECL_IMP(_XReadPad) { RET_VOID(); }
DECL_IMP(_XEatData) { RET_VOID(); }
DECL_IMP(_XSend) { RET_VOID(); }
DECL_IMP(_XSetLastRequestRead) { RET(0); }
