// Videos through Direct3D. CBinkPlayer::ShowFrame (0x4a8ac0) locks DirectDraw's primary surface
// and copies each Bink frame onto the screen. With a Direct3D 9 device in fullscreen (and with
// most windowed set-ups on current Windows) that surface is not what is shown, so only the first
// frame (or nothing) appears. Here the frame goes into a texture that the game's own device
// draws and presents, whatever the renderer and display mode.
#include "../common/util.h"
#include "../common/display.h"
#include <string.h>

extern char g_ini[MAX_PATH];
typedef long HR;
#define VT(o) (*(void ***)(o))
#define FN(o, i, t) ((t)VT(o)[i])

// IDirect3DDevice8
enum { D_PRESENT = 15, D_GETBACKBUFFER = 16, D_CREATETEXTURE = 20, D_BEGINSCENE = 34, D_ENDSCENE = 35, D_CLEAR = 36,
       D_SETRS = 50, D_GETRS = 51, D_GETTEXTURE = 60, D_SETTEXTURE = 61, D_GETTSS = 62, D_SETTSS = 63,
       D_DRAWUP = 72, D_SETVS = 76, D_GETVS = 77, D_GETVIEWPORT = 41, D_SETVIEWPORT = 40 };

typedef int (WINAPI *bink_doframe_fn)(void *);
typedef int (WINAPI *bink_copy_fn)(void *, void *, int, unsigned, unsigned, unsigned, unsigned);

static int (THISCALL *o_showframe)(uint8_t *);
static bink_doframe_fn p_doframe;
static bink_copy_fn p_copy;
static void *g_tex, *g_tex_dev;
static unsigned g_tw, g_th;

static int show(uint8_t *pl)
{
    void *dev = display_device();
    unsigned *bink = *(unsigned **)(pl + 4);
    if (!dev || !bink || !p_doframe || !p_copy) return 0;
    unsigned w = bink[0], h = bink[1];
    if (!w || !h || w > 4096 || h > 4096) return 0;
    if (!g_tex || g_tex_dev != dev || g_tw != w || g_th != h) {
        if (g_tex && g_tex_dev == dev) FN(g_tex, 2, ULONG (WINAPI *)(void *))(g_tex);
        g_tex = NULL;
        // X8R8G8B8 (22), managed (1): survives device resets
        HR hr = FN(dev, D_CREATETEXTURE, HR (WINAPI *)(void *, UINT, UINT, UINT, DWORD, DWORD, DWORD, void **))(dev, w, h, 1, 0, 22, 1, &g_tex);
        if (hr < 0 || !g_tex) { mlog("video: no texture (%08lx)", hr); g_tex = NULL; return 0; }
        g_tex_dev = dev; g_tw = w; g_th = h;
        mlog("video: %ux%u frames drawn by Direct3D", w, h);
    }
    p_doframe(bink);
    struct { int pitch; void *bits; } lr;
    if (FN(g_tex, 16, HR (WINAPI *)(void *, UINT, void *, const RECT *, DWORD))(g_tex, 0, &lr, NULL, 0) < 0) return 1;
    p_copy(bink, lr.bits, lr.pitch, h, 0, 0, 3 | 0x80000000);      // BINKSURFACE32, BINKCOPYALL
    FN(g_tex, 17, HR (WINAPI *)(void *, UINT))(g_tex, 0);

    // the whole back buffer; the video centred with its aspect ratio
    struct { DWORD x, y, w, h; float mn, mx; } vp, full;
    FN(dev, D_GETVIEWPORT, HR (WINAPI *)(void *, void *))(dev, &vp);
    void *bb = NULL;
    FN(dev, D_GETBACKBUFFER, HR (WINAPI *)(void *, UINT, DWORD, void **))(dev, 0, 0, &bb);
    unsigned bw = vp.w, bh = vp.h;
    if (bb) {
        DWORD desc[8];
        FN(bb, 8, HR (WINAPI *)(void *, void *))(bb, desc);
        bw = desc[6]; bh = desc[7];
        FN(bb, 2, ULONG (WINAPI *)(void *))(bb);
    }
    full = vp; full.x = full.y = 0; full.w = bw; full.h = bh; full.mn = 0; full.mx = 1;
    float s = (float)bw / w < (float)bh / h ? (float)bw / w : (float)bh / h;
    float x0 = (bw - w * s) / 2 - 0.5f, y0 = (bh - h * s) / 2 - 0.5f, x1 = x0 + w * s, y1 = y0 + h * s;
    struct { float x, y, z, rhw, u, v; } q[4] = {
        {x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};

    static const DWORD rs[] = {7, 14, 15, 22, 27, 28, 137};     // z, z write, alpha test, cull, blend, fog, lighting
    static const DWORD rv[] = {0, 0, 0, 1, 0, 0, 0};
    static const DWORD ts[][2] = {{0, 1}, {0, 2}, {0, 4}, {0, 5}, {0, 16}, {0, 17}, {0, 18}, {1, 1}, {1, 4}};
    static const DWORD tv[] = {2, 2, 2, 2, 2, 2, 0, 1, 1};     // select texture; linear; no mips; stage 1 off
    DWORD old_rs[7], old_ts[9], old_vs = 0;
    void *old_tex = NULL;
    for (int i = 0; i < 7; i++) FN(dev, D_GETRS, HR (WINAPI *)(void *, DWORD, DWORD *))(dev, rs[i], &old_rs[i]);
    for (int i = 0; i < 9; i++) FN(dev, D_GETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD *))(dev, ts[i][0], ts[i][1], &old_ts[i]);
    FN(dev, D_GETVS, HR (WINAPI *)(void *, DWORD *))(dev, &old_vs);
    FN(dev, D_GETTEXTURE, HR (WINAPI *)(void *, DWORD, void **))(dev, 0, &old_tex);

    FN(dev, D_BEGINSCENE, HR (WINAPI *)(void *))(dev);
    FN(dev, D_SETVIEWPORT, HR (WINAPI *)(void *, const void *))(dev, &full);
    FN(dev, D_CLEAR, HR (WINAPI *)(void *, DWORD, const void *, DWORD, DWORD, float, DWORD))(dev, 0, NULL, 1, 0xff000000, 1.0f, 0);
    for (int i = 0; i < 7; i++) FN(dev, D_SETRS, HR (WINAPI *)(void *, DWORD, DWORD))(dev, rs[i], rv[i]);
    for (int i = 0; i < 9; i++) FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, ts[i][0], ts[i][1], tv[i]);
    FN(dev, D_SETVS, HR (WINAPI *)(void *, DWORD))(dev, 0x104);                      // XYZRHW | TEX1
    FN(dev, D_SETTEXTURE, HR (WINAPI *)(void *, DWORD, void *))(dev, 0, g_tex);
    FN(dev, D_DRAWUP, HR (WINAPI *)(void *, DWORD, UINT, const void *, UINT))(dev, 5, 2, q, sizeof q[0]);   // strip
    FN(dev, D_ENDSCENE, HR (WINAPI *)(void *))(dev);

    for (int i = 0; i < 7; i++) FN(dev, D_SETRS, HR (WINAPI *)(void *, DWORD, DWORD))(dev, rs[i], old_rs[i]);
    for (int i = 0; i < 9; i++) FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, ts[i][0], ts[i][1], old_ts[i]);
    FN(dev, D_SETVS, HR (WINAPI *)(void *, DWORD))(dev, old_vs);
    FN(dev, D_SETTEXTURE, HR (WINAPI *)(void *, DWORD, void *))(dev, 0, old_tex);
    if (old_tex) FN(old_tex, 2, ULONG (WINAPI *)(void *))(old_tex);
    FN(dev, D_SETVIEWPORT, HR (WINAPI *)(void *, const void *))(dev, &vp);
    FN(dev, D_PRESENT, HR (WINAPI *)(void *, const RECT *, const RECT *, HWND, const void *))(dev, NULL, NULL, NULL, NULL);
    return 1;
}

static int THISCALL h_showframe(uint8_t *pl)
{
    if (show(pl)) return 0;
    return o_showframe(pl);
}

void aim1_video_install(void)
{
    // off by default until it is verified: players saw only a corner of the picture
    if (!GetPrivateProfileIntA("Video", "VideoD3D", 0, g_ini)) return;
    HMODULE b = GetModuleHandleA("binkw32.dll");
    if (!b) b = LoadLibraryA("binkw32.dll");
    p_doframe = b ? (bink_doframe_fn)GetProcAddress(b, "_BinkDoFrame@4") : NULL;
    p_copy = b ? (bink_copy_fn)GetProcAddress(b, "_BinkCopyToBuffer@28") : NULL;
    static const uint8_t k[] = {0x55, 0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x04};
    if (p_doframe && p_copy) o_showframe = hook_jmp(0x4a8ac0, k, sizeof k, (void *)h_showframe);
    mlog("video: Direct3D frames %s", o_showframe ? "on" : "unavailable");
}
