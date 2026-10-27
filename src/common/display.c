// Display modes for Direct3D 8 games: exclusive fullscreen (the game's own
// mode), a normal window, or a borderless window covering the monitor.
//
// Wraps IDirect3D8::CreateDevice / IDirect3DDevice8::Reset to adjust the
// presentation parameters and the window, maps the cursor to back-buffer
// coordinates (the games read the raw screen position), keeps the cursor
// confined to the game while it is active, and reacquires input when the
// game gets the focus back.
#include "display.h"
#include <string.h>

typedef struct {
    UINT BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount;
    UINT MultiSampleType, SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
} PP8;

typedef struct { UINT Width, Height, RefreshRate, Format; } MODE8;

typedef HRESULT (WINAPI *CreateDevice_t)(void *, UINT, UINT, HWND, DWORD, PP8 *, void **);
typedef HRESULT (WINAPI *Reset_t)(void *, PP8 *);
typedef HRESULT (WINAPI *GetAdapterDisplayMode_t)(void *, UINT, MODE8 *);
typedef void *(WINAPI *Direct3DCreate8_t)(UINT);

static CreateDevice_t o_CreateDevice;
static Reset_t o_Reset;
static Direct3DCreate8_t o_Direct3DCreate8;
static GetAdapterDisplayMode_t o_GetMode;
static BOOL (WINAPI *o_GetCursorPos)(POINT *);
static BOOL (WINAPI *o_ClipCursor)(const RECT *);
static LONG (WINAPI *o_ChangeDisplaySettingsA)(void *, DWORD);
static WNDPROC o_wndproc;

static void *g_d3d;
static HWND g_wnd;
static UINT g_adapter, g_devtype = 1;    // of the game's device
static int g_msaa;               // anti-aliasing samples (0 = the game's setting)
static int g_vsync = -1;         // fullscreen vertical sync: -1 the game's setting, 0 off, 1 on
static int g_aniso;              // forced anisotropic filtering level (0 = the game's filtering)
static int g_d3d9;               // running on the Direct3D 9 layer (d3d9wrap.c)
int display_is_d3d9(void) { return g_d3d9; }
extern char g_ini[MAX_PATH];
static int g_mode;              // DISPLAY_*
static int g_test;              // test harness: fixed window position, never touch the real cursor
static int g_tx, g_ty;
static int g_bbw, g_bbh;        // current back buffer size
static int g_clip_wanted;       // the game asked to confine the cursor
static int g_started;           // the device exists (startup mode already applied)
static void *g_dev;             // the game's IDirect3DDevice8
void *display_device(void) { return g_dev; }
static struct { UINT format, refresh, interval; int saved; } g_fs;   // the game's fullscreen settings
static void (*g_on_activate)(void);
void devtools_wrap_device(void *dev, HWND wnd);

int display_mode(void) { return g_mode; }
HWND display_window(void) { return g_wnd; }
void display_on_activate(void (*cb)(void)) { g_on_activate = cb; }

static void monitor_rect(HWND w, RECT *r, int work)
{
    MONITORINFO mi = {sizeof mi};
    if (GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi))
        *r = work ? mi.rcWork : mi.rcMonitor;
    else
        SetRect(r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

// confine the cursor to the visible game area while the game is active
static void apply_clip(void)
{
    static int clipped;
    if (g_test || !o_ClipCursor) return;
    if (!g_clip_wanted || !g_wnd || GetForegroundWindow() != g_wnd || IsIconic(g_wnd)) {
        if (clipped) o_ClipCursor(NULL);    // only release our own clip
        clipped = 0;
        return;
    }
    RECT r;
    if (g_mode == DISPLAY_FULLSCREEN) {
        GetWindowRect(g_wnd, &r);
    } else {
        GetClientRect(g_wnd, &r);
        MapWindowPoints(g_wnd, NULL, (POINT *)&r, 2);
    }
    o_ClipCursor(&r);
    clipped = 1;
}

static void style_window(HWND w, int bbw, int bbh)
{
    if (!w) return;
    g_wnd = w;
    LONG st = GetWindowLongA(w, GWL_STYLE) & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                                               WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE | WS_BORDER | WS_DLGFRAME);
    LONG ex = GetWindowLongA(w, GWL_EXSTYLE) & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    RECT r;
    HWND after = HWND_NOTOPMOST;
    if (g_mode == DISPLAY_WINDOWED) {
        st |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        SetRect(&r, 0, 0, bbw, bbh);
        AdjustWindowRectEx(&r, st, FALSE, ex);
        int ww = r.right - r.left, wh = r.bottom - r.top, x, y;
        if (g_test) {
            x = g_tx; y = g_ty;
        } else {
            RECT m;
            monitor_rect(w, &m, 1);
            x = m.left + ((m.right - m.left) - ww) / 2;
            y = m.top + ((m.bottom - m.top) - wh) / 2;
            if (x < m.left) x = m.left;
            if (y < m.top) y = m.top;
        }
        SetRect(&r, x, y, x + ww, y + wh);
    } else if (g_mode == DISPLAY_BORDERLESS) {
        st |= WS_POPUP | WS_VISIBLE;
        monitor_rect(w, &r, 0);
        after = HWND_TOP;
    } else {
        st |= WS_POPUP | WS_VISIBLE;
        SetRect(&r, 0, 0, bbw, bbh);
        after = HWND_TOP;
    }
    SetWindowLongA(w, GWL_STYLE, st);
    SetWindowLongA(w, GWL_EXSTYLE, ex);
    SetWindowPos(w, after, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 (g_test ? SWP_NOACTIVATE : 0) | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void fix_pp(PP8 *pp, HWND wnd)
{
    if (!g_fs.saved) {
        // the game always fills these for its fullscreen mode
        g_fs.format = pp->BackBufferFormat;
        g_fs.refresh = pp->FullScreen_RefreshRateInHz;
        g_fs.interval = pp->FullScreen_PresentationInterval;
        g_fs.saved = 1;
    }
    g_bbw = pp->BackBufferWidth;
    g_bbh = pp->BackBufferHeight;
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : wnd;
    if (w) g_wnd = w;
    if (g_mode == DISPLAY_FULLSCREEN) {
        pp->Windowed = FALSE;
        pp->BackBufferFormat = g_fs.format;
        pp->FullScreen_RefreshRateInHz = g_fs.refresh;
        pp->FullScreen_PresentationInterval = g_fs.interval;
        // at startup the game's own window handling is left untouched
        if (g_started) style_window(w, pp->BackBufferWidth, pp->BackBufferHeight);
    } else {
        MODE8 m = {0};
        if (g_d3d && o_GetMode) o_GetMode(g_d3d, 0, &m);
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        pp->FullScreen_PresentationInterval = 0;
        if (m.Format) pp->BackBufferFormat = m.Format;
        if (pp->SwapEffect == 0 || pp->SwapEffect > 3) pp->SwapEffect = 1;   // DISCARD
        style_window(w, pp->BackBufferWidth, pp->BackBufferHeight);
    }
    // anti-aliasing: needs device support for the back buffer and depth formats, a DISCARD
    // swap chain and a back buffer that is not lockable
    if (g_msaa > 1 && g_d3d) {
        typedef HRESULT (WINAPI *ms_fn)(void *, UINT, UINT, UINT, BOOL, UINT);
        ms_fn chk = (ms_fn)(*(void ***)g_d3d)[11];       // IDirect3D8::CheckDeviceMultiSampleType
        UINT n = g_msaa;
        while (n > 1 && (chk(g_d3d, g_adapter, g_devtype, pp->BackBufferFormat, pp->Windowed, n) < 0 ||
                         (pp->EnableAutoDepthStencil && chk(g_d3d, g_adapter, g_devtype, pp->AutoDepthStencilFormat, pp->Windowed, n) < 0)))
            n /= 2;
        if (n > 1) {
            pp->MultiSampleType = n;
            pp->SwapEffect = 1;
            pp->Flags &= ~1u;                               // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
        }
        mlog("display: anti-aliasing %ux (asked %dx)", n > 1 ? n : 0, g_msaa);
    }
    if (!pp->Windowed && g_vsync >= 0)                     // windows are synchronised by the desktop compositor
        pp->FullScreen_PresentationInterval = g_vsync ? 1 : 0x80000000;
    mlog("display: %s %ux%u fmt=%u", g_mode == DISPLAY_WINDOWED ? "windowed" : g_mode == DISPLAY_BORDERLESS ? "borderless" : "fullscreen",
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat);
}

int textinput_active(void);
void textinput_wm_key(int sc);

static LRESULT CALLBACK h_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && textinput_active()) textinput_wm_key(((lp >> 16) & 0x7f) | ((lp >> 24) & 1 ? 0x80 : 0));
    LRESULT r = CallWindowProcA(o_wndproc, w, msg, wp, lp);
    if (msg == WM_ACTIVATEAPP) {
        // the game only reads posted messages, so it never sees this one
        extern volatile int g_d9_in_reset;
        if (wp && g_mode == DISPLAY_FULLSCREEN && IsIconic(w) && !g_d9_in_reset) ShowWindow(w, SW_RESTORE);
        if (wp && g_on_activate) g_on_activate();
        apply_clip();
    } else if (msg == WM_ACTIVATE || (msg == WM_SIZE && g_mode != DISPLAY_FULLSCREEN) || msg == WM_MOVE) {
        apply_clip();
    }
    return r;
}

static void subclass(HWND w)
{
    if (!w || o_wndproc) return;
    o_wndproc = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)h_wndproc);
}

static HRESULT WINAPI h_Reset(void *dev, PP8 *pp)
{
    fix_pp(pp, g_wnd);
    HRESULT hr = o_Reset(dev, pp);
    mlog("display: Reset hr=%08lx", hr);
    if (hr >= 0) apply_clip();
    return hr;
}

// Gamma ramps (the menus' fades) change the whole monitor's brightness when the game is in a
// window: only exclusive fullscreen gets them.
typedef void (WINAPI *SetGammaRamp_t)(void *, DWORD, const void *);
static SetGammaRamp_t o_SetGammaRamp;
static void WINAPI h_SetGammaRamp(void *dev, DWORD flags, const void *ramp)
{
    static int n;
    if (!n++) mlog("display: the game sets gamma ramps (Direct3D)");
    if (g_mode == DISPLAY_FULLSCREEN && !g_test) o_SetGammaRamp(dev, flags, ramp);
}

static BOOL (WINAPI *o_SetDeviceGammaRamp)(HDC, void *);
static BOOL WINAPI h_SetDeviceGammaRamp(HDC dc, void *ramp)
{
    static int n;
    if (!n++) mlog("display: the game sets gamma ramps (GDI)");
    if (g_mode == DISPLAY_FULLSCREEN && !g_test) return o_SetDeviceGammaRamp(dc, ramp);
    return TRUE;
}

// Gamma set by any other module (loaded later, or calling through GetProcAddress) goes
// through gdi32 itself: patched at its entry.
static BOOL (WINAPI *o_gdi_gamma)(HDC, void *);
static BOOL WINAPI h_gdi_gamma(HDC dc, void *ramp)
{
    static int n;
    if (n < 5) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        void *ret = __builtin_return_address(0);
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m))
            GetModuleFileNameA(m, name, MAX_PATH);
        mlog("display: SetDeviceGammaRamp from %p (%s)%s", ret, name, g_mode == DISPLAY_FULLSCREEN && !g_test ? "" : " - ignored");
        n++;
    }
    if (g_mode == DISPLAY_FULLSCREEN && !g_test) return o_gdi_gamma(dc, ramp);
    return TRUE;
}

static void gamma_entry_hook(void)
{
    uint8_t *f = (uint8_t *)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SetDeviceGammaRamp");
    for (int i = 0; f && i < 3 && f[0] == 0xff && f[1] == 0x25; i++) f = **(uint8_t ***)(f + 2);    // jmp [import]
    static const uint8_t prolog[5] = {0x8b, 0xff, 0x55, 0x8b, 0xec};
    uint8_t first[5];
    if (f) memcpy(first, f, 5);
    // "mov edi, edi; push ebp; mov ebp, esp", or a system call stub starting with "mov eax, imm32"
    if (f && (!memcmp(f, prolog, 5) || f[0] == 0xb8)) o_gdi_gamma = hook_jmp((uintptr_t)f, first, 5, (void *)h_gdi_gamma);
    if (!o_gdi_gamma) mlog("display: gdi32 gamma entry not hooked (%02x %02x %02x)", f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0);
}

// Forced anisotropic filtering: linear minification becomes anisotropic.
typedef HRESULT (WINAPI *SetTSS_t)(void *, DWORD, DWORD, DWORD);
static SetTSS_t o_SetTSS;
static HRESULT WINAPI h_SetTSS(void *dev, DWORD stage, DWORD type, DWORD v)
{
    if (g_aniso > 1) {
        if (type == 17 /*D3DTSS_MINFILTER*/ && v == 2 /*LINEAR*/) {
            o_SetTSS(dev, stage, 21 /*D3DTSS_MAXANISOTROPY*/, (DWORD)g_aniso);
            v = 3;                                          // D3DTEXF_ANISOTROPIC
        } else if (type == 21) {
            v = (DWORD)g_aniso;
        }
    }
    return o_SetTSS(dev, stage, type, v);
}

// [Dev] D3DTrace=1: which Direct3D 8 features the game uses (planning the Direct3D 9 port)
typedef HRESULT (WINAPI *vfn5_t)(void *, DWORD, DWORD, DWORD, DWORD, DWORD);
static vfn5_t o_tr[100];
static volatile LONG g_trn[100];
#define TRACE_FN(i) static HRESULT WINAPI tr##i(void *d, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f) \
    { LONG n = InterlockedIncrement(&g_trn[i]); if (n <= 3) mlog("d3dtrace: slot %d (%lx %lx %lx %lx %lx)", i, a, b, c, e, f); return o_tr[i](d, a, b, c, e, f); }
TRACE_FN(20) TRACE_FN(25) TRACE_FN(26) TRACE_FN(31) TRACE_FN(75) TRACE_FN(87) TRACE_FN(48) TRACE_FN(44) TRACE_FN(46) TRACE_FN(21) TRACE_FN(22) TRACE_FN(94)
TRACE_FN(70) TRACE_FN(71) TRACE_FN(72) TRACE_FN(73) TRACE_FN(64) TRACE_FN(57) TRACE_FN(54) TRACE_FN(52)
static void trace_install(void **vt)
{
    static const int slots[] = {20, 25, 26, 31, 75, 87, 48, 44, 46, 21, 22, 94, 70, 71, 72, 73, 64, 57, 54, 52};
    void *fns[] = {tr20, tr25, tr26, tr31, tr75, tr87, tr48, tr44, tr46, tr21, tr22, tr94, tr70, tr71, tr72, tr73, tr64, tr57, tr54, tr52};
    for (int i = 0; i < 20; i++) {
        DWORD old;
        VirtualProtect(&vt[slots[i]], 4, PAGE_EXECUTE_READWRITE, &old);
        o_tr[slots[i]] = (vfn5_t)vt[slots[i]];
        vt[slots[i]] = fns[i];
        VirtualProtect(&vt[slots[i]], 4, old, &old);
    }
}

void display_trace_report(void)
{
    for (int i = 0; i < 100; i++) if (g_trn[i]) mlog("d3dtrace: slot %d called %ld times", i, g_trn[i]);
}

// ---- frame pacing ------------------------------------------------------------------------------
// [Video] FrameLimit=<fps>: evenly spaced frames (uneven frame times make variable refresh
// rate monitors - G-SYNC / FreeSync - flicker in brightness). [Dev] FrameStats=1 logs frame times.
typedef HRESULT (WINAPI *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);
static Present_t o_dPresent;
static int g_fps_limit, g_frame_stats, g_refresh = 60;
static LARGE_INTEGER g_qpf, g_last_present;

static void frame_stats(double ms)
{
    static double sum, mn = 1e9, mx, sq;
    static int n;
    static DWORD t0;
    sum += ms; sq += ms * ms; n++;
    if (ms < mn) mn = ms;
    if (ms > mx) mx = ms;
    if (!t0) t0 = GetTickCount();
    if (GetTickCount() - t0 < 5000) return;
    double avg = sum / n, var = sq / n - avg * avg;
    int sd = 0;
    while ((double)(sd + 1) * (sd + 1) <= var) sd++;
    mlog("frames: %d in 5 s, avg %.1f ms, min %.1f, max %.1f, sd ~%d", n, avg, mn, mx, sd);
    sum = sq = mx = 0; mn = 1e9; n = 0; t0 = GetTickCount();
}

static HRESULT WINAPI h_dPresent(void *dev, const RECT *a, const RECT *b, HWND w, const void *r)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // -1 = the monitor's refresh rate; VSync in a window (where the swap chain can't wait for
    // the display) paces to the refresh rate too
    int fps = g_fps_limit > 0 ? g_fps_limit : g_fps_limit < 0 || (g_vsync == 1 && g_mode != DISPLAY_FULLSCREEN) ? g_refresh : 0;
    if (fps > 0 && g_last_present.QuadPart) {
        LONGLONG next = g_last_present.QuadPart + g_qpf.QuadPart / fps;
        while (now.QuadPart < next) {
            LONGLONG left_ms = (next - now.QuadPart) * 1000 / g_qpf.QuadPart;
            if (left_ms > 2) Sleep((DWORD)(left_ms - 1)); else YieldProcessor();
            QueryPerformanceCounter(&now);
        }
        if (now.QuadPart - next < g_qpf.QuadPart / fps) now.QuadPart = next;           // keep the rhythm
    }
    if (g_frame_stats && g_last_present.QuadPart) frame_stats((double)(now.QuadPart - g_last_present.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
    g_last_present = now;
    void overlay_draw(void *dev);
    overlay_draw(dev);
    HRESULT hr = o_dPresent(dev, a, b, w, r);
    static HRESULT last;
    if (hr != last) { mlog("display: Present hr=%08lx (was %08lx)", hr, last); last = hr; }   // device lost / back
    return hr;
}

// a lost device the game never gets back from looks like a frozen game: log the state changes
typedef HRESULT (WINAPI *TestCoop_t)(void *);
static TestCoop_t o_TestCoop;
static HRESULT WINAPI h_TestCoop(void *dev)
{
    HRESULT hr = o_TestCoop(dev);
    static HRESULT last;
    if (hr != last) { mlog("display: TestCooperativeLevel hr=%08lx (was %08lx)", hr, last); last = hr; }
    return hr;
}

int display_fps_limit(void) { return g_fps_limit; }
void display_set_fps_limit(int fps)
{
    char b[16];
    g_fps_limit = fps;
    wsprintfA(b, "%d", g_fps_limit);
    WritePrivateProfileStringA("Video", "FrameLimit", b, g_ini);
}

int display_msaa(void) { return g_msaa; }
int display_vsync(void) { return g_vsync; }

void display_set_msaa(int n)                // applied by the next Reset
{
    char b[16];
    g_msaa = n == 2 || n == 4 || n == 8 ? n : 0;
    wsprintfA(b, "%d", g_msaa);
    WritePrivateProfileStringA("Video", "Antialiasing", b, g_ini);
}

void display_set_vsync(int v)                // -1 the game's setting, 0 off, 1 on
{
    char b[16];
    g_vsync = v < 0 ? -1 : v ? 1 : 0;
    wsprintfA(b, "%d", g_vsync);
    WritePrivateProfileStringA("Video", "VSync", b, g_ini);
}

int display_aniso(void) { return g_aniso; }

void display_set_aniso(int n)               // applies to the textures the game sets from now on
{
    char b[16];
    g_aniso = n > 16 ? 16 : n < 2 ? 0 : n;
    wsprintfA(b, "%d", g_aniso);
    WritePrivateProfileStringA("Video", "Anisotropy", b, g_ini);
}

static HRESULT WINAPI h_CreateDevice(void *d3d, UINT ad, UINT type, HWND focus, DWORD beh, PP8 *pp, void **out)
{
    mlog("display: CreateDevice(adapter %u, type %u, behaviour %lx, %ux%u fmt %u windowed %d)", ad, type, beh,
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed);
    g_adapter = ad;
    g_devtype = type;
    fix_pp(pp, focus);
    HRESULT hr = o_CreateDevice(d3d, ad, type, focus, beh, pp, out);
    if (FAILED(hr) && pp->MultiSampleType) {               // never fail the game's startup over anti-aliasing
        mlog("display: CreateDevice failed with anti-aliasing (%08lx), retrying without", hr);
        pp->MultiSampleType = 0;
        hr = o_CreateDevice(d3d, ad, type, focus, beh, pp, out);
    }
    if (SUCCEEDED(hr) && out && *out) {
        g_dev = *out;
        HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
        if (g_d3d9 && w) {                  // the window's title says which renderer runs
            char t[256];
            GetWindowTextA(w, t, 200);
            if (!strstr(t, "Direct3D 9")) { lstrcatA(t, " - Direct3D 9"); SetWindowTextA(w, t); }
        }
        devtools_wrap_device(*out, w);
        subclass(focus ? focus : w);
        if (!o_Reset) {
            void **vt = *(void ***)*out;
            DWORD old;
            VirtualProtect(&vt[14], 4, PAGE_EXECUTE_READWRITE, &old);
            o_Reset = (Reset_t)vt[14];
            vt[14] = (void *)h_Reset;
            VirtualProtect(&vt[14], 4, old, &old);
            VirtualProtect(&vt[18], 4, PAGE_EXECUTE_READWRITE, &old);
            o_SetGammaRamp = (SetGammaRamp_t)vt[18];
            vt[18] = (void *)h_SetGammaRamp;
            VirtualProtect(&vt[18], 4, old, &old);
            VirtualProtect(&vt[63], 4, PAGE_EXECUTE_READWRITE, &old);
            o_SetTSS = (SetTSS_t)vt[63];
            vt[63] = (void *)h_SetTSS;
            VirtualProtect(&vt[63], 4, old, &old);
            VirtualProtect(&vt[15], 4, PAGE_EXECUTE_READWRITE, &old);
            o_dPresent = (Present_t)vt[15];
            vt[15] = (void *)h_dPresent;
            VirtualProtect(&vt[15], 4, old, &old);
            VirtualProtect(&vt[3], 4, PAGE_EXECUTE_READWRITE, &old);
            o_TestCoop = (TestCoop_t)vt[3];
            vt[3] = (void *)h_TestCoop;
            VirtualProtect(&vt[3], 4, old, &old);
            if (GetPrivateProfileIntA("Dev", "D3DTrace", 0, g_ini)) trace_install(vt);
        }
        g_started = 1;
        apply_clip();
    }
    mlog("display: CreateDevice hr=%08lx", hr);
    return hr;
}

// The GOG release of Clan Wars ships its own d3d8.dll (a Direct3D 9 wrapper with its own
// display settings, dxcfg.ini) that overrides the display modes chosen here. Unless
// [Video] GameWrapper=1, the system's Direct3D 8 is used instead.
static void *system_d3d8(UINT sdk)
{
    HMODULE m = NULL;
    char path[MAX_PATH], dir[MAX_PATH];
    if (GetPrivateProfileIntA("Video", "GameWrapper", 0, g_ini) ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)o_Direct3DCreate8, &m) ||
        !GetModuleFileNameA(m, path, MAX_PATH) || !GetModuleFileNameA(NULL, dir, MAX_PATH))
        return NULL;
    char *s = strrchr(path, '\\'), *t = strrchr(dir, '\\');
    if (!s || !t || s - path != t - dir || _strnicmp(path, dir, s - path)) return NULL;   // not the game's folder
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\d3d8.dll");
    HMODULE sys = LoadLibraryA(path);
    void *(WINAPI *create)(UINT) = sys ? (void *(WINAPI *)(UINT))GetProcAddress(sys, "Direct3DCreate8") : NULL;
    void *d3d = create ? create(sdk) : NULL;
    mlog("display: the game folder's d3d8.dll is a wrapper; using %s (%s)", path, d3d ? "ok" : "failed");
    return d3d;
}

static void *WINAPI h_Direct3DCreate8(UINT sdk)
{
    void *d3d9wrap_create(void);
    void *d3d = GetPrivateProfileIntA("Video", "Renderer", 8, g_ini) == 9 ? d3d9wrap_create() : NULL;
    g_d3d9 = d3d != NULL;
    if (!d3d) d3d = system_d3d8(sdk);
    if (!d3d) d3d = o_Direct3DCreate8(sdk);
    mlog("display: Direct3DCreate8(%u) = %p", sdk, d3d);
    if (d3d && !o_CreateDevice) {
        g_d3d = d3d;
        void **vt = *(void ***)d3d;
        o_GetMode = (GetAdapterDisplayMode_t)vt[8];
        DWORD old;
        VirtualProtect(&vt[15], 4, PAGE_EXECUTE_READWRITE, &old);
        o_CreateDevice = (CreateDevice_t)vt[15];
        vt[15] = (void *)h_CreateDevice;
        VirtualProtect(&vt[15], 4, old, &old);
    }
    return d3d;
}

static LONG WINAPI h_ChangeDisplaySettingsA(void *mode, DWORD flags)
{
    if (g_mode == DISPLAY_FULLSCREEN && !g_test) return o_ChangeDisplaySettingsA(mode, flags);
    return DISP_CHANGE_SUCCESSFUL;
}

// the games use the raw screen position as their (own, drawn) cursor
static BOOL WINAPI h_GetCursorPos(POINT *p)
{
    BOOL ok = o_GetCursorPos(p);
    if (!ok || g_mode == DISPLAY_FULLSCREEN || !g_wnd) return ok;
    ScreenToClient(g_wnd, p);
    RECT c;
    GetClientRect(g_wnd, &c);
    if (c.right > 0 && c.bottom > 0 && g_bbw && g_bbh && (c.right != g_bbw || c.bottom != g_bbh)) {
        p->x = MulDiv(p->x, g_bbw, c.right);
        p->y = MulDiv(p->y, g_bbh, c.bottom);
    }
    return ok;
}

// A.I.M. converts the cursor itself (GetCursorPos + ScreenToClient); the
// position above is already in client space.
static BOOL (WINAPI *o_ScreenToClient)(HWND, POINT *);
static BOOL WINAPI h_ScreenToClient(HWND w, POINT *p)
{
    if (g_mode != DISPLAY_FULLSCREEN && g_wnd && w == g_wnd) return TRUE;
    return o_ScreenToClient(w, p);
}

// A.I.M. makes its window a topmost popup and resizes it after creating the
// device: outside fullscreen the window keeps the frame and size set here.
static BOOL (WINAPI *o_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static LONG (WINAPI *o_SetWindowLongA)(HWND, int, LONG);
static BOOL WINAPI h_SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT fl)
{
    if (g_mode != DISPLAY_FULLSCREEN && g_wnd && w == g_wnd) return TRUE;
    return o_SetWindowPos(w, after, x, y, cx, cy, fl);
}
static LONG WINAPI h_SetWindowLongA(HWND w, int idx, LONG v)
{
    if (g_mode != DISPLAY_FULLSCREEN && g_wnd && w == g_wnd && (idx == GWL_STYLE || idx == GWL_EXSTYLE))
        return GetWindowLongA(w, idx);
    return o_SetWindowLongA(w, idx, v);
}

static BOOL WINAPI h_ClipCursor(const RECT *r)
{
    g_clip_wanted = r != NULL;
    apply_clip();
    return TRUE;
}

int display_select(int mode)
{
    if (mode < DISPLAY_FULLSCREEN || mode > DISPLAY_BORDERLESS) return 0;
    if (g_test && mode != DISPLAY_WINDOWED) {          // test instances never cover the screen
        mlog("display: only windowed mode in the test harness");
        return 0;
    }
    g_mode = mode;
    return 1;
}

void display_install(int mode, int test, int x, int y)
{
    g_test = test;
    g_tx = x; g_ty = y;
    g_mode = test ? DISPLAY_WINDOWED : mode;
    g_msaa = GetPrivateProfileIntA("Video", "Antialiasing", 0, g_ini);
    if (g_msaa != 2 && g_msaa != 4 && g_msaa != 8) g_msaa = 0;
    g_vsync = GetPrivateProfileIntA("Video", "VSync", -1, g_ini);
    g_aniso = GetPrivateProfileIntA("Video", "Anisotropy", 0, g_ini);
    if (g_aniso > 16) g_aniso = 16;
    g_fps_limit = GetPrivateProfileIntA("Video", "FrameLimit", 0, g_ini);
    g_frame_stats = GetPrivateProfileIntA("Dev", "FrameStats", 0, g_ini);
    QueryPerformanceFrequency(&g_qpf);
    DEVMODEA dm = {0};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 20) g_refresh = (int)dm.dmDisplayFrequency;
    {   // 1 ms timer resolution, so the frame limiter's Sleep is accurate
        HMODULE wm = LoadLibraryA("winmm.dll");
        UINT (WINAPI *tbp)(UINT) = wm ? (UINT (WINAPI *)(UINT))GetProcAddress(wm, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);
    }
    HMODULE exe = GetModuleHandleA(NULL);
    iat_hook_all("d3d8.dll", "Direct3DCreate8", h_Direct3DCreate8, (void **)&o_Direct3DCreate8);
    iat_hook_all("gdi32.dll", "SetDeviceGammaRamp", h_SetDeviceGammaRamp, (void **)&o_SetDeviceGammaRamp);
    gamma_entry_hook();
    iat_hook_all("user32.dll", "ChangeDisplaySettingsA", h_ChangeDisplaySettingsA, (void **)&o_ChangeDisplaySettingsA);
    iat_hook_one(exe, "user32.dll", "GetCursorPos", h_GetCursorPos, (void **)&o_GetCursorPos);
    iat_hook_one(exe, "user32.dll", "ClipCursor", h_ClipCursor, (void **)&o_ClipCursor);
    iat_hook_one(exe, "user32.dll", "ScreenToClient", h_ScreenToClient, (void **)&o_ScreenToClient);
    iat_hook_one(exe, "user32.dll", "SetWindowPos", h_SetWindowPos, (void **)&o_SetWindowPos);
    iat_hook_one(exe, "user32.dll", "SetWindowLongA", h_SetWindowLongA, (void **)&o_SetWindowLongA);
    mlog("display: mode %d%s", g_mode, test ? " (test harness)" : "");
}
