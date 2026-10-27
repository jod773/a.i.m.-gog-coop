// Developer test harness (enabled with [Dev] Harness=1 in the mod ini).
//
// Lets an external script drive the game without touching the real mouse,
// keyboard or window focus:
//   * commands are read from <root>rr_cmd.txt (the file is deleted after
//     reading); results are appended to the log
//   * "shot <file.bmp>"     - save the next rendered frame (back buffer)
//   * "click <x> <y>"       - virtual left click at client coordinates
//   * "rclick <x> <y>"
//   * "move <x> <y>"        - virtual cursor position
//   * "key <dik>"           - tap a DirectInput scan code
//   * "hold <dik> <ms>"
//   * "virtual on|off"      - while on, real input is ignored
// The virtual cursor is reported through GetCursorPos; buttons and keys
// are merged into the DirectInput device state/data.
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ---- minimal DirectInput 8 / D3D8 definitions ----------------------------------
static const GUID GUID_SysMouse_ = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID GUID_SysKeyboard_ = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; UINT_PTR uAppData; } DIDOD;
#define DIMOFS_BUTTON0 12
#define DIMOFS_BUTTON1 13

typedef HRESULT (WINAPI *CreateDevice8_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, DIDOD *, DWORD *, DWORD);
typedef HRESULT (WINAPI *Acquire_t)(void *);
typedef HRESULT (WINAPI *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);

static CreateDevice8_t o_DICreateDevice;
int textinput_feed_state(const uint8_t *state);
int textinput_feed_event(int sc, int down);
static GetDeviceState_t o_GetDeviceState;
static GetDeviceData_t o_GetDeviceData;
static Acquire_t o_Acquire;
static Present_t o_Present;
static void *g_mouse, *g_kbd;

static CRITICAL_SECTION g_cs;
static int g_enabled;
static volatile int g_virtual;          // ignore real input
static volatile LONG g_vx = 100, g_vy = 100;
static volatile int g_btn[2];           // virtual mouse buttons (frames remaining / state)
static uint8_t g_keys[256];             // virtual keys held
static char g_shot_path[MAX_PATH];
static void (*g_game_cmd)(const char *line);
static volatile int g_shot_req;
static HWND g_wnd;

// pending buffered events for GetDeviceData
static DIDOD g_mq[64]; static int g_mqn;
static DIDOD g_kq[64]; static int g_kqn;
static DWORD g_seq = 1;

extern char g_ini[MAX_PATH];
// the co-op chat key (DirectInput scan code, [Coop] ChatKey, default T)
static int chat_key(void)
{
    static int k = -1;
    if (k < 0) k = GetPrivateProfileIntA("Coop", "ChatKey", 0x14, g_ini) & 255;
    return k;
}
static void chat_key_seen(int down)
{
    static int was;
    void coop_chat_key(void);
    if (down && !was) coop_chat_key();
    was = down != 0;
}

static void push_ev(DIDOD *q, int *n, DWORD ofs, DWORD data)
{
    if (*n >= 64) return;
    DIDOD e = {ofs, data, GetTickCount(), g_seq++, 0};
    q[(*n)++] = e;
}

// ---- DirectInput wrappers ---------------------------------------------------------
// The mouse and keyboard are always wrapped: a device that lost its
// acquisition (focus change, alt-tab) is reacquired and read again. The
// virtual-input parts only run with the harness enabled.
#define DIERR_INPUTLOST_   ((HRESULT)0x8007001E)
#define DIERR_NOTACQUIRED_ ((HRESULT)0x8007000C)
#define LOST(hr) ((hr) == DIERR_INPUTLOST_ || (hr) == DIERR_NOTACQUIRED_)

static HRESULT WINAPI h_Acquire(void *dev)
{
    HRESULT hr = o_Acquire(dev);
    return g_virtual ? 0 : hr;
}

// called when the game window becomes active again
void input_reacquire(void)
{
    if (!o_Acquire) return;
    if (g_kbd) o_Acquire(g_kbd);
    if (g_mouse) o_Acquire(g_mouse);
}

// ---- gamepad as mouse -----------------------------------------------------------------
// The game reads the mouse either as a state (GetDeviceState) or as buffered
// events (GetDeviceData); the movement goes to whichever it uses.
static int g_pad_events;               // the game reads buffered mouse events
static BOOL (WINAPI *o_GetCursorPos)(POINT *);
static int g_pad_msgs;                 // A.I.M.: menus read clicks from window messages

static void pad_cursor(int dx, int dy)
{
    if (!dx && !dy) return;
    if (g_virtual) {
        g_vx += dx; g_vy += dy;
        if (g_pad_msgs && g_wnd) PostMessageA(g_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(g_vx, g_vy));
        return;
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;          // never move the cursor for other programs
    POINT p;
    if (o_GetCursorPos ? o_GetCursorPos(&p) : GetCursorPos(&p)) SetCursorPos(p.x + dx, p.y + dy);
}

static volatile DWORD g_mouse_read;    // last time the game read the DirectInput mouse
void devtools_set_pad_messages(int on) { g_pad_msgs = on; }

static void pad_click_msg(int b, int down)
{
    HWND w = g_wnd;
    if (!w) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) return;
        w = fg;
    }
    POINT p;
    if (g_virtual) { p.x = g_vx; p.y = g_vy; }
    else { (o_GetCursorPos ? o_GetCursorPos : GetCursorPos)(&p); ScreenToClient(w, &p); }
    UINT msg = b ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
    PostMessageA(w, msg, down ? (b ? MK_RBUTTON : MK_LBUTTON) : 0, MAKELPARAM(p.x, p.y));
}

static int g_pad_btn[3];               // mouse button states last handed to the game
static uint8_t g_pad_keys[256];        // keys last handed to the game
static DIDOD g_pq[64]; static int g_pqn;   // the pad's key events (buffered keyboard readers)

static void pad_buttons(const int *btn, DIDOD *q, int *qn)
{
    for (int i = 0; i < 3; i++) {
        if (btn[i] == g_pad_btn[i]) continue;
        if (g_pad_msgs && i < 2) pad_click_msg(i, btn[i]);
        if (q) push_ev(q, qn, DIMOFS_BUTTON0 + i, btn[i] ? 0x80 : 0);
        g_pad_btn[i] = btn[i];
    }
}

// the pad's keys: changes become keyboard events; Esc also as a window message (menus)
static void pad_keys(void)
{
    uint8_t k[256];
    gamepad_keys(k);
    for (int i = 1; i < 256; i++) {
        if (k[i] == g_pad_keys[i]) continue;
        push_ev(g_pq, &g_pqn, i, k[i] ? 0x80 : 0);
        if (i == 0x01 && g_wnd) PostMessageA(g_wnd, k[i] ? WM_KEYDOWN : WM_KEYUP, VK_ESCAPE, k[i] ? 0x00010001 : 0xC0010001);
        g_pad_keys[i] = k[i];
    }
}

static void ensure_cs(void)
{
    static LONG done;
    if (!InterlockedExchange(&done, 1)) InitializeCriticalSection(&g_cs);
}

// menus that don't read the DirectInput mouse: move the cursor and click from here
static DWORD WINAPI pad_thread(void *p)
{
    (void)p;
    for (;;) {
        Sleep(16);
        if (!gamepad_enabled() || GetTickCount() - g_mouse_read < 200) continue;
        int dx, dy, btn[3];
        gamepad_poll();
        gamepad_take(&dx, &dy, btn);
        pad_cursor(dx, dy);
        EnterCriticalSection(&g_cs);
        pad_buttons(btn, g_mq, &g_mqn);
        pad_keys();
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

void devtools_start_pad(void)
{
    static int started;
    if (started) return;
    started = 1;
    ensure_cs();
    CreateThread(NULL, 0, pad_thread, NULL, 0, NULL);
}

// ---- mouse sensitivity / smoothing ([Mouse] Sensitivity = percent, Smoothing = ms) -------------
// Some players found the games' lowest mouse sensitivity still too high, and the turning jumpy at
// high frame rates. The real mouse's movement (not the gamepad's, not the harness's) is scaled, and
// smoothed with a time constant: each read hands on part of the movement not yet given to the game
// (nothing is lost, the rest follows within a few frames). Only in flight - menu cursors stay as
// they are.
int cg_in_flight(void);
static int g_msens = -1, g_msmooth;
static float g_mrx, g_mry;                       // movement not yet handed to the game
static LARGE_INTEGER g_mlast;

static void mouse_load(void)
{
    if (g_msens >= 0) return;
    g_msens = GetPrivateProfileIntA("Mouse", "Sensitivity", 100, g_ini);
    if (g_msens < 5 || g_msens > 400) g_msens = 100;
    g_msmooth = GetPrivateProfileIntA("Mouse", "Smoothing", 0, g_ini);
    if (g_msmooth < 0 || g_msmooth > 500) g_msmooth = 0;
}

int mouse_sensitivity(void) { mouse_load(); return g_msens; }
int mouse_smoothing(void) { mouse_load(); return g_msmooth; }
void mouse_set(int sens, int smooth_ms)
{
    char v[16];
    g_msens = sens;
    g_msmooth = smooth_ms;
    g_mrx = g_mry = 0;
    snprintf(v, sizeof v, "%d", sens);
    WritePrivateProfileStringA("Mouse", "Sensitivity", v, g_ini);
    snprintf(v, sizeof v, "%d", smooth_ms);
    WritePrivateProfileStringA("Mouse", "Smoothing", v, g_ini);
}

static LONG mouse_take(float *pending, float part)
{
    float out = *pending * part;
    LONG n = (LONG)out;                          // toward zero
    if (!n && (*pending >= 1.0f || *pending <= -1.0f)) n = *pending > 0 ? 1 : -1;   // always drains
    *pending -= (float)n;
    return n;
}

// the real mouse's movement this read -> what the game gets
static void mouse_filter(LONG *dx, LONG *dy)
{
    mouse_load();
    if ((g_msens == 100 && !g_msmooth) || !cg_in_flight()) { g_mrx = g_mry = 0; return; }
    g_mrx += *dx * (g_msens / 100.0f);
    g_mry += *dy * (g_msens / 100.0f);
    float part = 1.0f;
    if (g_msmooth) {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        float ms = g_mlast.QuadPart ? (float)(now.QuadPart - g_mlast.QuadPart) * 1000.0f / (float)f.QuadPart : 16.0f;
        g_mlast = now;
        if (ms > 100.0f) ms = 100.0f;
        part = 1.0f - expf(-ms / (float)g_msmooth);
    }
    *dx = mouse_take(&g_mrx, part);
    *dy = mouse_take(&g_mry, part);
}

// buffered mouse events (DIMOFS_X 0 / DIMOFS_Y 4): the movement of these events, filtered, goes into
// the first X / Y event (added at the end if there is none and room for one)
static void mouse_filter_events(DIDOD *od, DWORD cb, DWORD *n, DWORD cap)
{
    mouse_load();
    if (g_msens == 100 && !g_msmooth) return;
    LONG sx = 0, sy = 0;
    int fx = -1, fy = -1;
    for (DWORD i = 0; i < *n; i++) {
        DIDOD *e = (DIDOD *)((uint8_t *)od + i * cb);
        if (e->dwOfs == 0) { sx += (LONG)e->dwData; if (fx < 0) fx = (int)i; e->dwData = 0; }
        else if (e->dwOfs == 4) { sy += (LONG)e->dwData; if (fy < 0) fy = (int)i; e->dwData = 0; }
    }
    LONG ox = sx, oy = sy;
    mouse_filter(&ox, &oy);
    for (int axis = 0; axis < 2; axis++) {
        LONG v = axis ? oy : ox;
        int at = axis ? fy : fx;
        if (at < 0) {
            if (!v || *n >= cap) {                   // no room: keep it for the next read
                if (axis) g_mry += (float)v; else g_mrx += (float)v;
                continue;
            }
            DIDOD *e = (DIDOD *)((uint8_t *)od + (*n) * cb);
            memset(e, 0, cb);
            e->dwOfs = axis ? 4 : 0;
            e->dwTimeStamp = GetTickCount();
            e->dwSequence = g_seq++;
            at = (int)(*n)++;
        }
        ((DIDOD *)((uint8_t *)od + at * cb))->dwData = (DWORD)v;
    }
}

static HRESULT gds_inner(void *dev, DWORD cb, void *data)
{
    HRESULT hr = o_GetDeviceState(dev, cb, data);
    if (dev == g_mouse) g_mouse_read = GetTickCount();
    if (LOST(hr) && o_Acquire(dev) >= 0) hr = o_GetDeviceState(dev, cb, data);
    if (!g_enabled) return hr;
    if (g_virtual || hr < 0) {
        if (!g_virtual && hr < 0) return hr;
        memset(data, 0, cb);
        hr = 0;
    }
    EnterCriticalSection(&g_cs);
    if (dev == g_mouse && cb >= 16) {
        uint8_t *b = (uint8_t *)data + 12;
        if (g_btn[0]) b[0] = 0x80;
        if (g_btn[1]) b[1] = 0x80;
    } else if (dev == g_kbd && cb >= 256) {
        uint8_t *k = data;
        for (int i = 0; i < 256; i++) if (g_keys[i]) k[i] = 0x80;
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}

static HRESULT WINAPI h_GetDeviceState(void *dev, DWORD cb, void *data);

// The mouse movement since the last read, for the mod's own use while the game ignores the mouse
// (A.I.M.'s spectator camera: a dead player's game doesn't turn anything with it). Read through
// our hook, so the gamepad's right stick and the test harness count too. 0 = no mouse.
int devtools_read_mouse(int *dx, int *dy, int *dz)
{
    struct { LONG x, y, z; BYTE b[4]; } st;
    *dx = *dy = *dz = 0;
    if (!g_mouse || !o_GetDeviceState || h_GetDeviceState(g_mouse, sizeof st, &st) < 0) return 0;
    *dx = st.x; *dy = st.y; *dz = st.z;
    return 1;
}

static HRESULT WINAPI h_GetDeviceState(void *dev, DWORD cb, void *data)
{
    HRESULT hr = gds_inner(dev, cb, data);
    if (dev == g_mouse && cb >= 8 && hr >= 0 && !g_virtual) mouse_filter((LONG *)data, (LONG *)data + 1);
    // the gamepad on top of the real (or test) input
    if (dev == g_mouse && gamepad_enabled() && cb >= 16) {
        int dx, dy, btn[3];
        gamepad_poll();
        if (hr < 0) { memset(data, 0, cb); hr = 0; }
        if (!g_pad_events) {
            gamepad_take(&dx, &dy, btn);
            ((LONG *)data)[0] += dx;
            ((LONG *)data)[1] += dy;
            pad_cursor(dx, dy);
            pad_buttons(btn, NULL, NULL);
        }
        for (int i = 0; i < 3; i++) if (g_pad_btn[i]) ((uint8_t *)data)[12 + i] = 0x80;
    }
    if (dev == g_kbd && gamepad_enabled() && cb >= 256) {
        uint8_t k[256];
        int any = 0;
        gamepad_poll();
        gamepad_keys(k);
        for (int i = 1; i < 256; i++) any |= k[i];
        if (any && hr < 0) { memset(data, 0, cb); hr = 0; }
        if (hr >= 0) for (int i = 1; i < 256; i++) if (k[i]) ((uint8_t *)data)[i] = 0x80;
    }
    if (dev == g_kbd && cb >= 256 && hr >= 0 && !textinput_active()) chat_key_seen(((uint8_t *)data)[chat_key()] & 0x80);
    if (dev == g_kbd && cb >= 256 && hr >= 0 && textinput_feed_state(data)) memset(data, 0, cb);
    return hr;
}
static HRESULT kbd_text(void *dev, DWORD cb, DIDOD *od, DWORD *inout, DWORD flags, HRESULT hr)
{
    if (dev != g_kbd || !od || (flags & 1) || hr < 0) return hr;
    int eat = 0;
    for (DWORD i = 0; i < *inout; i++) {
        const DWORD *e = (const DWORD *)((uint8_t *)od + i * cb);
        if ((int)e[0] == chat_key() && !textinput_active()) chat_key_seen(e[1] & 0x80);
        eat |= textinput_feed_event((int)e[0], (e[1] & 0x80) != 0);
    }
    if (eat || textinput_feed_event(-1, 0)) *inout = 0;
    return hr;
}

static HRESULT h_GetDeviceData_(void *dev, DWORD cb, DIDOD *od, DWORD *inout, DWORD flags);
static HRESULT WINAPI h_GetDeviceData(void *dev, DWORD cb, DIDOD *od, DWORD *inout, DWORD flags)
{
    return kbd_text(dev, cb, od, inout, flags, h_GetDeviceData_(dev, cb, od, inout, flags));
}

static HRESULT h_GetDeviceData_(void *dev, DWORD cb, DIDOD *od, DWORD *inout, DWORD flags)
{
    DWORD cap = *inout;
    HRESULT hr;
    if (dev == g_mouse) g_mouse_read = GetTickCount();
    if (dev == g_mouse && gamepad_enabled() && od && !(flags & 1)) {
        int dx, dy, btn[3];
        g_pad_events = 1;
        gamepad_poll();
        gamepad_take(&dx, &dy, btn);
        EnterCriticalSection(&g_cs);
        if (dx) push_ev(g_mq, &g_mqn, 0 /*DIMOFS_X*/, (DWORD)dx);
        if (dy) push_ev(g_mq, &g_mqn, 4 /*DIMOFS_Y*/, (DWORD)dy);
        pad_buttons(btn, g_mq, &g_mqn);
        LeaveCriticalSection(&g_cs);
        pad_cursor(dx, dy);
    }
    if (dev == g_kbd && gamepad_enabled() && od && !(flags & 1)) {
        gamepad_poll();
        EnterCriticalSection(&g_cs);
        pad_keys();
        LeaveCriticalSection(&g_cs);
    }
    if (!g_enabled) {
        hr = o_GetDeviceData(dev, cb, od, inout, flags);
        if (LOST(hr) && o_Acquire(dev) >= 0) {
            *inout = cap;
            hr = o_GetDeviceData(dev, cb, od, inout, flags);
        }
        if (dev == g_mouse && hr >= 0 && od && !(flags & 1) && cb >= 8) mouse_filter_events(od, cb, inout, cap);
        DIDOD *pq = dev == g_mouse ? g_mq : dev == g_kbd ? g_pq : NULL;   // gamepad events
        int *pn = dev == g_mouse ? &g_mqn : &g_pqn;
        if (hr >= 0 && pq && *pn && od && !(flags & 1)) {
            EnterCriticalSection(&g_cs);
            int i = 0;
            while (i < *pn && *inout < cap) {
                memcpy((uint8_t *)od + (*inout) * cb, &pq[i], cb < sizeof(DIDOD) ? cb : sizeof(DIDOD));
                (*inout)++;
                i++;
            }
            memmove(pq, pq + i, (*pn - i) * sizeof(DIDOD));
            *pn -= i;
            LeaveCriticalSection(&g_cs);
        }
        return hr;
    }
    if (g_virtual) { *inout = 0; hr = 0; }
    else {
        hr = o_GetDeviceData(dev, cb, od, inout, flags);
        if (hr < 0) { *inout = 0; hr = 0; }
        else if (dev == g_mouse && od && !(flags & 1) && cb >= 8) mouse_filter_events(od, cb, inout, cap);
    }
    EnterCriticalSection(&g_cs);
    DIDOD *q = dev == g_mouse ? g_mq : dev == g_kbd ? g_kq : NULL;
    int *qn = dev == g_mouse ? &g_mqn : &g_kqn;
    if (q && od && !(flags & 1 /*DIGDD_PEEK*/)) {
        int i = 0;
        while (i < *qn && *inout < cap) {
            memcpy((uint8_t *)od + (*inout) * cb, &q[i], cb < sizeof(DIDOD) ? cb : sizeof(DIDOD));
            (*inout)++;
            i++;
        }
        memmove(q, q + i, (*qn - i) * sizeof(DIDOD));
        *qn -= i;
    }
    if (dev == g_kbd && od && !(flags & 1)) {      // the gamepad's key events
        int i = 0;
        while (i < g_pqn && *inout < cap) {
            memcpy((uint8_t *)od + (*inout) * cb, &g_pq[i], cb < sizeof(DIDOD) ? cb : sizeof(DIDOD));
            (*inout)++;
            i++;
        }
        memmove(g_pq, g_pq + i, (g_pqn - i) * sizeof(DIDOD));
        g_pqn -= i;
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}

static void hook_vt(void *obj, int idx, void *fn, void **orig)
{
    void **vt = *(void ***)obj;
    if (vt[idx] == fn) return;
    DWORD old;
    VirtualProtect(&vt[idx], 4, PAGE_EXECUTE_READWRITE, &old);
    if (!*orig) *orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], 4, old, &old);
}

static HRESULT WINAPI h_DICreateDevice(void *di, REFGUID g, void **out, void *outer)
{
    HRESULT hr = o_DICreateDevice(di, g, out, outer);
    if (hr >= 0 && out && *out) {
        if (IsEqualGUID(g, &GUID_SysMouse_)) g_mouse = *out;
        else if (IsEqualGUID(g, &GUID_SysKeyboard_)) g_kbd = *out;
        else return hr;
        hook_vt(*out, 7, h_Acquire, (void **)&o_Acquire);
        hook_vt(*out, 9, h_GetDeviceState, (void **)&o_GetDeviceState);
        hook_vt(*out, 10, h_GetDeviceData, (void **)&o_GetDeviceData);
    }
    return hr;
}

// called by the dinput8 proxy with the real IDirectInput8 object
void devtools_wrap_dinput(void *di)
{
    if (!di) return;
    ensure_cs();
    hook_vt(di, 3, h_DICreateDevice, (void **)&o_DICreateDevice);
}

// ---- cursor -------------------------------------------------------------------------
static BOOL WINAPI h_GetCursorPos(POINT *p)
{
    if (!g_virtual) return o_GetCursorPos(p);
    p->x = g_vx; p->y = g_vy;
    if (g_wnd) ClientToScreen(g_wnd, p);
    return TRUE;
}
static BOOL (WINAPI *o_SetForegroundWindow)(HWND);
static BOOL WINAPI h_SetForegroundWindow(HWND h) { (void)h; return TRUE; }

// ---- screenshots ---------------------------------------------------------------------
typedef struct { UINT Format, Type; DWORD Usage; UINT Pool, Size, MultiSampleType, Width, Height; } SDESC8;
typedef struct { INT Pitch; void *pBits; } LRECT8;

static void save_bmp(const char *path, int w, int h, int fmt, const uint8_t *bits, int pitch)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int rowsz = w * 3, pad = (4 - rowsz % 4) % 4;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + (rowsz + pad) * h), 0, 0, 54};
    BITMAPINFOHEADER ih = {40, w, h, 1, 24, 0, (DWORD)((rowsz + pad) * h), 2835, 2835, 0, 0};
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    uint8_t *row = malloc(rowsz + pad);
    memset(row, 0, rowsz + pad);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *s = bits + y * pitch;
        for (int x = 0; x < w; x++) {
            if (fmt == 23) {           // R5G6B5
                uint16_t v = ((const uint16_t *)s)[x];
                row[x * 3 + 0] = (uint8_t)((v & 31) << 3);
                row[x * 3 + 1] = (uint8_t)(((v >> 5) & 63) << 2);
                row[x * 3 + 2] = (uint8_t)(((v >> 11) & 31) << 3);
            } else {                   // X8R8G8B8 / A8R8G8B8
                row[x * 3 + 0] = s[x * 4 + 0];
                row[x * 3 + 1] = s[x * 4 + 1];
                row[x * 3 + 2] = s[x * 4 + 2];
            }
        }
        fwrite(row, rowsz + pad, 1, f);
    }
    free(row);
    fclose(f);
}

static void take_shot(void *dev)
{
    void **vt = *(void ***)dev;
    void *bb = NULL, *img = NULL;
    typedef HRESULT (WINAPI *GetBB_t)(void *, UINT, UINT, void **);
    typedef HRESULT (WINAPI *CreateImg_t)(void *, UINT, UINT, UINT, void **);
    typedef HRESULT (WINAPI *CopyRects_t)(void *, void *, const RECT *, UINT, void *, const POINT *);
    typedef HRESULT (WINAPI *GetDesc_t)(void *, SDESC8 *);
    typedef HRESULT (WINAPI *Lock_t)(void *, LRECT8 *, const RECT *, DWORD);
    typedef HRESULT (WINAPI *Unlock_t)(void *);
    typedef ULONG (WINAPI *Release_t)(void *);
    if (((GetBB_t)vt[16])(dev, 0, 0, &bb) < 0) { mlog("shot: GetBackBuffer failed"); return; }
    void **svt = *(void ***)bb;
    SDESC8 d;
    ((GetDesc_t)svt[8])(bb, &d);
    if (((CreateImg_t)vt[27])(dev, d.Width, d.Height, d.Format, &img) >= 0 &&
        ((CopyRects_t)vt[28])(dev, bb, NULL, 0, img, NULL) >= 0) {
        void **ivt = *(void ***)img;
        LRECT8 lr;
        if (((Lock_t)ivt[9])(img, &lr, NULL, 0x10 /*READONLY*/) >= 0) {
            save_bmp(g_shot_path, d.Width, d.Height, d.Format, lr.pBits, lr.Pitch);
            ((Unlock_t)ivt[10])(img);
            mlog("shot saved: %s (%ux%u fmt %u)", g_shot_path, d.Width, d.Height, d.Format);
        }
    } else {
        mlog("shot: copy failed (fmt %u, ms %u)", d.Format, d.MultiSampleType);
    }
    if (img) ((Release_t)(*(void ***)img)[2])(img);
    ((Release_t)svt[2])(bb);
}

static HRESULT WINAPI h_Present(void *dev, const RECT *a, const RECT *b, HWND w, const void *r)
{
    if (g_shot_req) { g_shot_req = 0; take_shot(dev); }
    return o_Present(dev, a, b, w, r);
}

void devtools_wrap_device(void *dev, HWND wnd)
{
    if (!g_enabled || !dev) return;
    g_wnd = wnd;
    hook_vt(dev, 15, h_Present, (void **)&o_Present);
}

// ---- command processing -------------------------------------------------------------
static int g_click_msgs;

// A.I.M. reads mouse buttons from window messages: also post them to the
// game's own window.
void devtools_set_click_messages(int on) { g_click_msgs = on; }
int devtools_key_down(int dik) { return g_enabled && g_virtual && g_keys[dik & 255]; }

static void press_button(int b, int down)
{
    EnterCriticalSection(&g_cs);
    g_btn[b] = down;
    push_ev(g_mq, &g_mqn, DIMOFS_BUTTON0 + b, down ? 0x80 : 0);
    LeaveCriticalSection(&g_cs);
    if (g_click_msgs && g_wnd) {
        UINT msg = b ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
        WPARAM wp = down ? (b ? MK_RBUTTON : MK_LBUTTON) : 0;
        PostMessageA(g_wnd, msg, wp, MAKELPARAM(g_vx, g_vy));
    }
}

static void set_key(int k, int down)
{
    EnterCriticalSection(&g_cs);
    g_keys[k & 255] = (uint8_t)down;
    push_ev(g_kq, &g_kqn, k & 255, down ? 0x80 : 0);
    LeaveCriticalSection(&g_cs);
}

static void dump_stack(void);

static void run_cmd(char *line)
{
    char cmd[32] = {0}, a[MAX_PATH] = {0};
    int x = 0, y = 0;
    if (sscanf(line, "%31s", cmd) != 1) return;
    mlog("harness: %s", line);
    if (!strcmp(cmd, "shot") && sscanf(line, "%*s %259s", a) == 1) {
        lstrcpynA(g_shot_path, a, MAX_PATH);
        g_shot_req = 1;
    } else if (!strcmp(cmd, "virtual") && sscanf(line, "%*s %31s", a) == 1) {
        g_virtual = !strcmp(a, "on");
    } else if (!strcmp(cmd, "move") && sscanf(line, "%*s %d %d", &x, &y) == 2) {
        g_vx = x; g_vy = y;
        if (g_click_msgs && g_wnd) PostMessageA(g_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    } else if ((!strcmp(cmd, "click") || !strcmp(cmd, "rclick")) && sscanf(line, "%*s %d %d", &x, &y) == 2) {
        int b = cmd[0] == 'r';
        g_vx = x; g_vy = y;
        if (g_click_msgs && g_wnd) PostMessageA(g_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        Sleep(120);
        press_button(b, 1);
        Sleep(120);
        press_button(b, 0);
    } else if ((!strcmp(cmd, "mdown") || !strcmp(cmd, "mup")) && sscanf(line, "%*s %d", &x) == 1) {   // mdown/mup <button 0/1>
        press_button(x != 0, cmd[1] == 'd');
    } else if (!strcmp(cmd, "mhold") && sscanf(line, "%*s %d %d", &x, &y) == 2) {   // mhold <button 0/1> <ms>
        press_button(x != 0, 1);
        Sleep(y);
        press_button(x != 0, 0);
    } else if (!strcmp(cmd, "key") && sscanf(line, "%*s %i", &x) == 1) {
        set_key(x, 1);
        Sleep(120);
        set_key(x, 0);
    } else if (!strcmp(cmd, "hold") && sscanf(line, "%*s %i %d", &x, &y) == 2) {
        set_key(x, 1);
        Sleep(y);
        set_key(x, 0);
    } else if (!strcmp(cmd, "sleep") && sscanf(line, "%*s %d", &x) == 1) {
        Sleep(x);
    } else if (!strcmp(cmd, "pad")) {           // pad <lx> <ly> <rx> <ry> <buttons> <ms>
        int lx = 0, ly = 0, rx = 0, ry = 0, bt = 0, ms = 500;
        sscanf(line, "%*s %i %i %i %i %i %i", &lx, &ly, &rx, &ry, &bt, &ms);
        void gamepad_fake(int, int, int, int, int, int);
        gamepad_fake(lx, ly, rx, ry, bt, ms);
    } else if (!strcmp(cmd, "chars") && sscanf(line, "%*s %259s", a) == 1) {   // typed text (WM_CHAR) for the game's edit boxes
        HWND display_window(void);
        for (const char *c = a; *c; c++) PostMessageA(display_window(), WM_CHAR, (WPARAM)(*c == '_' ? ' ' : *c), 1);
    } else if (!strcmp(cmd, "memfind") && sscanf(line, "%*s %259s", a) == 1) {   // where a string is in memory
        size_t n = strlen(a);
        int hits = 0;
        MEMORY_BASIC_INFORMATION mi;
        for (uint8_t *p = (uint8_t *)0x10000; hits < 40 && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
            if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
            uint8_t *b = mi.BaseAddress;
            for (size_t i = 0; i + n <= mi.RegionSize && hits < 40; i++)
                if ((i & 0xfff) == 0 && IsBadReadPtr(b + i, 0x1000)) i += 0xfff;
                else if (b[i] == (uint8_t)a[0] && !memcmp(b + i, a, n)) {
                    mlog("memfind: %p (%s, region %p) %.24s", b + i, mi.Type == MEM_IMAGE ? "image" : "heap", mi.AllocationBase, (char *)b + i);
                    hits++;
                }
        }
    } else if (!strcmp(cmd, "ptrfind")) {          // ptrfind <hex address> <hex range>: dwords pointing into [addr - range, addr]
        unsigned long at = 0, range = 0x400;
        sscanf(line, "%*s %lx %lx", &at, &range);
        int hits = 0;
        MEMORY_BASIC_INFORMATION mi;
        for (uint8_t *p = (uint8_t *)0x10000; hits < 40 && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
            if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
            uint32_t *b = mi.BaseAddress;
            for (size_t i = 0; i < mi.RegionSize / 4 && hits < 40; i++)
                if (b[i] <= at && at - b[i] <= range) {
                    mlog("ptrfind: %p (%s) -> %08lx (+%lx)", b + i, mi.Type == MEM_IMAGE ? "image" : "heap", (unsigned long)b[i], at - b[i]);
                    hits++;
                }
        }
    } else if (!strcmp(cmd, "peek")) {             // peek <hex address>: 8 dwords
        unsigned long at = 0;
        sscanf(line, "%*s %lx", &at);
        const uint32_t *p = (const uint32_t *)at;
        if (!IsBadReadPtr(p, 32)) mlog("peek %08lx: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", at, (unsigned long)p[0], (unsigned long)p[1], (unsigned long)p[2], (unsigned long)p[3], (unsigned long)p[4], (unsigned long)p[5], (unsigned long)p[6], (unsigned long)p[7]);
    } else if (!strcmp(cmd, "d3d9frame")) {
        extern volatile int g_d9_logframe;
        g_d9_logframe = 2;
    } else if (!strcmp(cmd, "deactivate") || !strcmp(cmd, "activate")) {   // what Windows sends on alt-tab
        int on = cmd[0] == 'a';
        if (g_wnd) {
            SendMessageA(g_wnd, WM_ACTIVATEAPP, on, 0);
            SendMessageA(g_wnd, WM_ACTIVATE, on ? WA_ACTIVE : WA_INACTIVE, 0);
            SendMessageA(g_wnd, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        }
    } else if (!strcmp(cmd, "devlost")) {          // the Direct3D 9 device acts lost (alt-tab in fullscreen)
        extern volatile int g_d9_fake_lost;
        g_d9_fake_lost = 1;
    } else if (!strcmp(cmd, "lightdbg")) {        // lightdbg <n>: debug view of the light pass (0 = off)
        void d9light_debug(int mode, DWORD fvf, DWORD op);
        int m = 0;
        unsigned fvf = 0, op = 0;
        sscanf(line, "%*s %d %x %u", &m, &fvf, &op);
        d9light_debug(m, fvf, op);
    } else if (!strcmp(cmd, "d3dtrace")) {
        void display_trace_report(void);
        display_trace_report();
    } else if (!strcmp(cmd, "stack")) {
        dump_stack();
    } else if (!strcmp(cmd, "quit")) {
        mlog("harness: quit requested");
        ExitProcess(0);
    } else if (g_game_cmd) {
        g_game_cmd(line);      // game-specific commands (queued for the game thread)
    }
}

void devtools_set_game_cmd(void (*cb)(const char *line)) { g_game_cmd = cb; }

// "stack": log the game thread's registers and the code addresses on its stack
// (as addresses of the unrelocated executable, for the disassembly).
static DWORD g_main_tid;
extern IMAGE_DOS_HEADER __ImageBase;

static void dump_stack(void)
{
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
    if (!t) { mlog("stack: no thread"); return; }
    SuspendThread(t);
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(t, &ctx)) {
        uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL), d = exe - 0x400000;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
        uintptr_t lo = exe + nt->OptionalHeader.BaseOfCode, hi = lo + nt->OptionalHeader.SizeOfCode;
        mlog("stack: eip=%08lx (exe %08lx) esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx", (unsigned long)ctx.Eip,
             (unsigned long)(ctx.Eip - d), (unsigned long)ctx.Esp, (unsigned long)ctx.Ebp, (unsigned long)ctx.Eax, (unsigned long)ctx.Ecx);
        char buf[1024];
        int n = 0, k = 0;
        MEMORY_BASIC_INFORMATION mi;
        for (uintptr_t *p = (uintptr_t *)ctx.Esp; k < 40 && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
             (uint8_t *)p < (uint8_t *)mi.BaseAddress + mi.RegionSize && (uint8_t *)p < (uint8_t *)ctx.Esp + 0x8000; p++) {
            if (*p >= lo && *p < hi) {
                n += snprintf(buf + n, sizeof buf - n, " %08lx", (unsigned long)(*p - d));
                k++;
            } else {
                HMODULE m;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*p, &m) &&
                    m == (HMODULE)&__ImageBase) {
                    n += snprintf(buf + n, sizeof buf - n, " mod+%lx", (unsigned long)(*p - (uintptr_t)m));
                    k++;
                }
            }
            if (n > (int)sizeof buf - 24) break;
        }
        mlog("stack:%s", buf);
    }
    ResumeThread(t);
    CloseHandle(t);
}

static DWORD WINAPI cmd_thread(void *p)
{
    (void)p;
    char path[MAX_PATH], tmp[MAX_PATH];
    snprintf(path, sizeof path, "%srr_cmd.txt", g_root);
    snprintf(tmp, sizeof tmp, "%srr_cmd.run", g_root);
    for (;;) {
        Sleep(100);
        if (!file_exists(path)) continue;
        DeleteFileA(tmp);
        if (!MoveFileA(path, tmp)) continue;
        FILE *f = fopen(tmp, "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line && *line != '#') run_cmd(line);
        }
        fclose(f);
        DeleteFileA(tmp);
    }
    return 0;
}

void devtools_install(void)
{
    g_enabled = 1;
    g_main_tid = GetCurrentThreadId();
    ensure_cs();
    iat_hook_all("user32.dll", "GetCursorPos", h_GetCursorPos, (void **)&o_GetCursorPos);
    iat_hook_all("user32.dll", "SetForegroundWindow", h_SetForegroundWindow, (void **)&o_SetForegroundWindow);
    CreateThread(NULL, 0, cmd_thread, NULL, 0, NULL);
    mlog("devtools: harness enabled");
}
