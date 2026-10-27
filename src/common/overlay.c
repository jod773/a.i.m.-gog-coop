// Text drawn over the game's picture (the chat line being typed), through the game's own
// Direct3D 8 device just before it presents a frame. The font is drawn once by GDI into a
// texture (ASCII 32-126 and cp1251 192-255, 16 x 14 cells).
#include "util.h"
#include <string.h>

typedef long HR;
#define VT(o) (*(void ***)(o))
#define FN(o, i, t) ((t)VT(o)[i])

enum { D_GETBACKBUFFER = 16, D_CREATETEXTURE = 20, D_BEGINSCENE = 34, D_ENDSCENE = 35, D_SETVIEWPORT = 40,
       D_GETVIEWPORT = 41, D_SETRS = 50, D_GETRS = 51, D_GETTEXTURE = 60, D_SETTEXTURE = 61, D_GETTSS = 62,
       D_SETTSS = 63, D_DRAWUP = 72, D_SETVS = 76, D_GETVS = 77 };

#define CW 16
#define CH 28
#define TEXW 256
#define TEXH 512                      // 16 x 18 cells
static void *g_font, *g_font_dev;
static unsigned char g_adv[256];
static char g_line[160];
static volatile int g_show;
static CRITICAL_SECTION g_cs;
static int g_cs_ok;

static void lock(void) { if (!g_cs_ok) { InitializeCriticalSection(&g_cs); g_cs_ok = 1; } EnterCriticalSection(&g_cs); }

void overlay_set_line(const char *text)
{
    lock();
    if (text) { lstrcpynA(g_line, text, sizeof g_line); g_show = 1; }
    else g_show = 0;
    LeaveCriticalSection(&g_cs);
}

static int cell_of(unsigned char c)
{
    if (c >= 32 && c < 127) return c - 32;            // 0..94
    if (c >= 192) return 95 + (c - 192);               // 95..158
    return -1;
}

static int make_font(void *dev)
{
    if (g_font && g_font_dev == dev) return 1;
    g_font = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), TEXW, -TEXH, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    uint32_t *px = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bm) { DeleteDC(dc); return 0; }
    HGDIOBJ ob = SelectObject(dc, bm);
    HFONT f = CreateFontA(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, RUSSIAN_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    memset(px, 0, TEXW * TEXH * 4);
    for (int c = 32; c < 256; c++) {
        int k = cell_of((unsigned char)c);
        if (k < 0) continue;
        char ch = (char)c;
        SIZE sz;
        GetTextExtentPoint32A(dc, &ch, 1, &sz);
        g_adv[c] = (unsigned char)(sz.cx > CW ? CW : sz.cx);
        TextOutA(dc, (k % 16) * CW, (k / 16) * CH + 2, &ch, 1);
    }
    GdiFlush();
    SelectObject(dc, of);
    DeleteObject(f);
    void *t = NULL;
    // A8R8G8B8 (21), managed
    if (FN(dev, D_CREATETEXTURE, HR (WINAPI *)(void *, UINT, UINT, UINT, DWORD, DWORD, DWORD, void **))(dev, TEXW, TEXH, 1, 0, 21, 1, &t) >= 0 && t) {
        struct { int pitch; uint8_t *bits; } lr;
        if (FN(t, 16, HR (WINAPI *)(void *, UINT, void *, const RECT *, DWORD))(t, 0, &lr, NULL, 0) >= 0) {
            for (int y = 0; y < TEXH; y++) {
                uint32_t *d = (uint32_t *)(lr.bits + y * lr.pitch);
                for (int x = 0; x < TEXW; x++) {
                    uint32_t a = px[y * TEXW + x] & 0xff;      // white text: any channel is the coverage
                    d[x] = (a << 24) | 0x00ffffff;
                }
            }
            FN(t, 17, HR (WINAPI *)(void *, UINT))(t, 0);
            g_font = t;
            g_font_dev = dev;
        } else {
            FN(t, 2, ULONG (WINAPI *)(void *))(t);
        }
    }
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
    return g_font != NULL;
}

typedef struct { float x, y, z, rhw; DWORD c; float u, v; } vtx;

static int quad(vtx *v, float x0, float y0, float x1, float y1, DWORD c, float u0, float v0, float u1, float v1)
{
    vtx q[6] = {{x0, y0, 0, 1, c, u0, v0}, {x1, y0, 0, 1, c, u1, v0}, {x0, y1, 0, 1, c, u0, v1},
                {x1, y0, 0, 1, c, u1, v0}, {x1, y1, 0, 1, c, u1, v1}, {x0, y1, 0, 1, c, u0, v1}};
    memcpy(v, q, sizeof q);
    return 6;
}

// a small line of text in a corner of the screen, no box (the mod's version in the main menu)
static char g_corner[96];
static volatile int g_corner_show;

void overlay_set_corner(const char *text)
{
    lock();
    if (text) { lstrcpynA(g_corner, text, sizeof g_corner); g_corner_show = 1; }
    else g_corner_show = 0;
    LeaveCriticalSection(&g_cs);
}

static float text_width(const char *s, float sc)
{
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) w += (g_adv[*p] ? g_adv[*p] : 8) * sc;
    return w;
}

// glyph quads of s at (x, y); returns the vertex count added (room for max vertices)
static int glyphs(vtx *v, int max, const char *s, float x, float y, float sc, DWORD c)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 6 <= max; p++) {
        int k = cell_of(*p);
        float adv = (g_adv[*p] ? g_adv[*p] : 8) * sc;
        if (k >= 0 && *p != ' ') {
            float u0 = (k % 16) * CW / (float)TEXW, v0 = (k / 16) * CH / (float)TEXH;
            n += quad(v + n, x, y, x + CW * sc, y + CH * sc, c, u0, v0, u0 + CW / (float)TEXW, v0 + CH / (float)TEXH);
        }
        x += adv;
    }
    return n;
}

// called with the game's device right before Present
void overlay_draw(void *dev)
{
    if ((!g_show && !g_corner_show) || !dev) return;
    char line[160], corner[96];
    lock();
    int show = g_show, show_corner = g_corner_show;
    lstrcpynA(line, show ? g_line : "", sizeof line);
    lstrcpynA(corner, show_corner ? g_corner : "", sizeof corner);
    LeaveCriticalSection(&g_cs);
    if (!make_font(dev)) return;

    struct { DWORD x, y, w, h; float mn, mx; } vp;
    FN(dev, D_GETVIEWPORT, HR (WINAPI *)(void *, void *))(dev, &vp);
    float sc = vp.h >= 1000 ? 1.0f : vp.h / 1080.0f + 0.15f;
    float x = vp.x + 40.0f, y = vp.y + vp.h * 0.70f;
    static vtx v[6 * 260];
    int n = 0;
    // the chat line's box (drawn untextured: the font has no solid white area)
    vtx box[6];
    if (show) {
        quad(box, x - 10, y - 6, x + text_width(line, sc) + 24, y + CH * sc + 4, 0xa0000000, 0, 0, 0, 0);
        n += glyphs(v + n, 6 * 160, line, x, y, sc, 0xffffffff);
    }
    if (show_corner) {           // bottom right, small and dim
        float cs = sc * 0.7f;
        float cx = vp.x + vp.w - text_width(corner, cs) - 16.0f, cy = vp.y + vp.h - CH * cs - 10.0f;
        n += glyphs(v + n, 6 * 260 - n, corner, cx, cy, cs, 0xc0d0d0d0);
    }

    static const DWORD rs[] = {7, 14, 15, 22, 27, 28, 137, 19, 20};   // z, zwrite, alphatest, cull, blend, fog, lighting, src, dst
    static const DWORD rv[] = {0, 0, 0, 1, 1, 0, 0, 5, 6};
    static const DWORD ts[][2] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 16}, {0, 17}, {0, 18}, {1, 1}, {1, 4}};
    static const DWORD tv[] = {4, 2, 0, 4, 2, 0, 2, 2, 0, 1, 1};     // modulate texture x diffuse (colour and alpha)
    DWORD ors[9], ots[11], ovs = 0;
    void *otex = NULL;
    for (int i = 0; i < 9; i++) FN(dev, D_GETRS, HR (WINAPI *)(void *, DWORD, DWORD *))(dev, rs[i], &ors[i]);
    for (int i = 0; i < 11; i++) FN(dev, D_GETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD *))(dev, ts[i][0], ts[i][1], &ots[i]);
    FN(dev, D_GETVS, HR (WINAPI *)(void *, DWORD *))(dev, &ovs);
    FN(dev, D_GETTEXTURE, HR (WINAPI *)(void *, DWORD, void **))(dev, 0, &otex);

    FN(dev, D_BEGINSCENE, HR (WINAPI *)(void *))(dev);
    for (int i = 0; i < 9; i++) FN(dev, D_SETRS, HR (WINAPI *)(void *, DWORD, DWORD))(dev, rs[i], rv[i]);
    for (int i = 0; i < 11; i++) FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, ts[i][0], ts[i][1], tv[i]);
    FN(dev, D_SETVS, HR (WINAPI *)(void *, DWORD))(dev, 0x144);                     // XYZRHW | DIFFUSE | TEX1
    // the box: diffuse only
    FN(dev, D_SETTEXTURE, HR (WINAPI *)(void *, DWORD, void *))(dev, 0, NULL);
    FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, 0, 1, 3);     // select diffuse
    FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, 0, 4, 3);
    if (show) FN(dev, D_DRAWUP, HR (WINAPI *)(void *, DWORD, UINT, const void *, UINT))(dev, 4, 2, box, sizeof box[0]);
    FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, 0, 1, 4);
    FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, 0, 4, 4);
    FN(dev, D_SETTEXTURE, HR (WINAPI *)(void *, DWORD, void *))(dev, 0, g_font);
    if (n) FN(dev, D_DRAWUP, HR (WINAPI *)(void *, DWORD, UINT, const void *, UINT))(dev, 4, n / 3, v, sizeof v[0]);
    FN(dev, D_ENDSCENE, HR (WINAPI *)(void *))(dev);

    for (int i = 0; i < 9; i++) FN(dev, D_SETRS, HR (WINAPI *)(void *, DWORD, DWORD))(dev, rs[i], ors[i]);
    for (int i = 0; i < 11; i++) FN(dev, D_SETTSS, HR (WINAPI *)(void *, DWORD, DWORD, DWORD))(dev, ts[i][0], ts[i][1], ots[i]);
    FN(dev, D_SETVS, HR (WINAPI *)(void *, DWORD))(dev, ovs);
    FN(dev, D_SETTEXTURE, HR (WINAPI *)(void *, DWORD, void *))(dev, 0, otex);
    if (otex) FN(otex, 2, ULONG (WINAPI *)(void *))(otex);
}
