// "4. Debug" in the in-flight command menu (tilde).
//
// The command menu is drawn by 0x408420 from its state at CMMApp + 0xb8
// (-1 closed, 0 root, 1 group, 2 general, 3 target commands); the game's
// input code only acts on states 0..3 and toggles the menu with tilde for
// any state. We add a line to the root menu and a state of our own.
#include "engine.h"
#include "coop.h"
#include "../common/lang.h"
#include <stdio.h>
#include <string.h>

#define CALL(addr, T) ((T)(addr))
#define MENU_STATE   0xb8
#define ST_DEBUG     100
#define TYPE_BUILDING 0x0b
#define OBJ_TYPE     0x54
#define OBJ_NAME     0x14

typedef int (THISCALL *height_fn)(void *, float);
typedef void (THISCALL *text_fn)(void *, float, float, gstr *, uint32_t, int);
typedef void (THISCALL *ctor_cstr_fn)(void *, const char *);
typedef void (THISCALL *dtor_fn)(void *);
typedef void (THISCALL *setpos_fn)(void *, float, float, float);
typedef void (THISCALL *draw_fn)(void *);

static draw_fn o_draw;
static int g_on, g_sel = -1;
static uint8_t g_prev[256];

static uint8_t *app(void) { return *(uint8_t **)A_RENDER_APP_PTR; }

int devtools_key_down(int dik);

// a key's state: the harness' virtual keyboard or the real one (game window active)
static int key_down(int dik)
{
    if (devtools_key_down(dik)) return 1;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    UINT vk = MapVirtualKeyA(dik, 1 /* MAPVK_VSC_TO_VK */);
    return vk && (GetAsyncKeyState(vk) & 0x8000);
}

static int pressed(int dik)
{
    int d = key_down(dik);
    int p = d && !g_prev[dik];
    g_prev[dik] = (uint8_t)d;
    return p;
}

static void line(int i, const char *s, int enabled)
{
    void *font = A_FONT_PTR ? *(void *volatile *)A_FONT_PTR : NULL;
    if (!font) return;
    gstr g;
    CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&g, s);
    float y = (float)(CALL(A_FONT_HEIGHT, height_fn)(font, 1.0f) * i + 0x9c);
    CALL(A_FONT_TEXT, text_fn)(font, 121.0f, y, &g, 0xff000000, 0xffff);       // shadow
    CALL(A_FONT_TEXT, text_fn)(font, 120.0f, y, &g, enabled ? 0xff00fe00 : 0xff808080, 0xffff);
    CALL(A_GSTR_DTOR, dtor_fn)(&g);
}

// a base's exit doors (B_L5_BASE2_EXIT...) are buildings too, but not enterable: entering one
// opens a broken base screen and leaves it on the map for good
static int is_exit(const uint8_t *o)
{
    char nm[33];
    lstrcpynA(nm, (const char *)o + OBJ_NAME, sizeof nm);
    CharUpperA(nm);
    return strstr(nm, "_EXIT") != NULL;
}

// buildings of the current location, in object list order
static int buildings(uint8_t **out, int max)
{
    int n = 0;
    for (void **nd = *(void ***)A_OBJLIST_HEAD; nd && n < max; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (o && o[OBJ_ALIVE] && *(int *)(o + OBJ_TYPE) == TYPE_BUILDING && o[OBJ_NAME] && !is_exit(o)) out[n++] = o;
    }
    return n;
}

static float dist2(const uint8_t *a, const uint8_t *b)
{
    const float *p = (const float *)(a + OBJ_POS), *q = (const float *)(b + OBJ_POS);
    return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]);
}

static void teleport(int step)
{
    uint8_t *b[128];
    int n = buildings(b, 128);
    if (!n || !coop_in_flight()) return;
    g_sel = ((g_sel + step) % n + n) % n;
    const float *p = (const float *)(b[g_sel] + OBJ_POS);
    CALL(A_OBJ_SETPOS, setpos_fn)(G_PLAYER_GLIDER, p[0] + 150.0f, p[1] + 150.0f, p[2] + 120.0f);
    memset(G_PLAYER_GLIDER + GLIDER_VELOCITY, 0, 12);
    char buf[96];
    snprintf(buf, sizeof buf, aim2_ui("dbg_teleported"), (char *)b[g_sel] + OBJ_NAME);
    coop_notify(buf);
}

static void enter_nearest(void)
{
    uint8_t *b[128];
    int n = buildings(b, 128);
    if (!n || !coop_in_flight() || !A_ENTER_BUILDING) return;
    uint8_t *best = b[0];
    for (int i = 1; i < n; i++)
        if (dist2(b[i], G_PLAYER_GLIDER) < dist2(best, G_PLAYER_GLIDER)) best = b[i];
    typedef void (THISCALL *enter_fn)(void *, name32 *);
    name32 nm;
    memset(&nm, 0, sizeof nm);
    lstrcpynA(nm.s, (char *)best + OBJ_NAME, 32);
    mlog("debug: entering %s", nm.s);
    ((enter_fn)A_ENTER_BUILDING)(NULL, &nm);
}

static int g_player_sel;

static void teleport_player(void)
{
    const char *name = NULL;
    uint8_t *g = coop_remote_by_index(g_player_sel++, &name);
    if (!g) { g_player_sel = 0; g = coop_remote_by_index(g_player_sel++, &name); }
    if (!g || !coop_in_flight()) return;
    const float *p = (const float *)(g + OBJ_POS), *m = (const float *)(g + OBJ_MATRIX);
    // keep our own orientation and step back along our view line (forward is -row 2):
    // the other player ends up in the middle of the screen
    (void)m;
    const float *me = (const float *)(G_PLAYER_GLIDER + OBJ_MATRIX);
    CALL(A_OBJ_SETPOS, setpos_fn)(G_PLAYER_GLIDER, p[0] + me[8] * 90.0f, p[1] + me[9] * 90.0f, p[2] + me[10] * 90.0f);
    memset(G_PLAYER_GLIDER + GLIDER_VELOCITY, 0, 12);
    char buf[96];
    snprintf(buf, sizeof buf, aim2_ui("dbg_teleported"), name);
    coop_notify(buf);
}

// passage tunnels: records of 0x128 bytes, +0x100 keys left, +0x104 unlimited
static void unlock_passages(void)
{
    if (!A_PASSAGES) return;
    // every sector with a full map (the game adds a record when you get its key)
    static const char *const locs[] = {"location1", "location2", "location3", "location5", "location6",
                                       "location9", "location10", "location11"};
    typedef uint8_t *(THISCALL *locinfo_fn)(void *, const char *);
    for (size_t i = 0; A_GET_LOCINFO && i < sizeof locs / sizeof locs[0]; i++) {
        uint8_t *r = ((locinfo_fn)A_GET_LOCINFO)((void *)A_GAMEDATA, locs[i]);
        if (r) r[0x104] = 1;
    }
    uint8_t *recs = *(uint8_t **)A_PASSAGES;
    int n = *(int *)(A_PASSAGES + 4);
    for (int i = 0; recs && i < n && i < 64; i++) {
        recs[i * 0x128 + 0x104] = 1;
        mlog("debug: passage %d: %.32s / %.64s", i, (char *)recs + i * 0x128 + 0x108, (char *)recs + i * 0x128);
    }
    char buf[96];
    snprintf(buf, sizeof buf, aim2_ui("dbg_unlocked"), n);
    coop_notify(buf);
    mlog("debug: %d passages unlocked", n);
}

static void show_position(void)
{
    if (!coop_in_flight()) return;
    const float *p = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    char buf[96];
    snprintf(buf, sizeof buf, "%.0f %.0f %.0f", p[0], p[1], p[2]);
    coop_notify(buf);
    mlog("debug: position %s", buf);
}

// ---- player names above their gliders ----------------------------------------------------
// The scene's view and projection come from the Direct3D device (GetTransform).
static int g_names = 1;
int names_enabled(void) { return g_names; }
void names_set_enabled(int on)
{
    g_names = on ? 1 : 0;
    WritePrivateProfileStringA("Coop", "ShowNames", g_names ? "1" : "0", g_ini);
}

typedef struct { float m[16]; } mat4;
typedef struct { DWORD x, y, w, h; float zmin, zmax; } viewport8;
uint8_t *coop_remote_by_index(int n, const char **name);

static void draw_names(void)
{
    void *dev = A_DEVICE_PTR ? *(void *volatile *)A_DEVICE_PTR : NULL;
    void *font = A_FONT_PTR ? *(void *volatile *)A_FONT_PTR : NULL;
    if (!g_names || !dev || !font || !coop_in_flight()) return;
    typedef HRESULT (WINAPI *gett_fn)(void *, DWORD, mat4 *);
    typedef HRESULT (WINAPI *getvp_fn)(void *, viewport8 *);
    mat4 v, pr;
    viewport8 vp;
    void **vt = *(void ***)dev;
    if (((gett_fn)vt[38])(dev, 2, &v) < 0 || ((gett_fn)vt[38])(dev, 3, &pr) < 0 || ((getvp_fn)vt[41])(dev, &vp) < 0) return;
    const char *name;
    uint8_t *g;
    for (int i = 0; (g = coop_remote_by_index(i, &name)) != NULL; i++) {
        const float *p = (const float *)(g + OBJ_POS);
        float w[4] = {p[0], p[1], p[2] + 8.0f, 1.0f}, e[4], c[4];   // a little above the glider
        for (int k = 0; k < 4; k++) e[k] = w[0] * v.m[k] + w[1] * v.m[4 + k] + w[2] * v.m[8 + k] + w[3] * v.m[12 + k];
        for (int k = 0; k < 4; k++) c[k] = e[0] * pr.m[k] + e[1] * pr.m[4 + k] + e[2] * pr.m[8 + k] + e[3] * pr.m[12 + k];
        if (c[3] <= 0.1f) continue;                                   // behind the camera
        float sx = vp.x + (c[0] / c[3] + 1.0f) * 0.5f * vp.w, sy = vp.y + (1.0f - c[1] / c[3]) * 0.5f * vp.h;
        if (sx < 0 || sy < 0 || sx > vp.x + vp.w || sy > vp.y + vp.h) continue;
        const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        float d2 = (p[0] - pp[0]) * (p[0] - pp[0]) + (p[1] - pp[1]) * (p[1] - pp[1]) + (p[2] - pp[2]) * (p[2] - pp[2]);
        if (d2 > 2500.0f * 2500.0f) continue;                        // too far to read
        gstr gs;
        CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&gs, name);
        float x = sx - (float)lstrlenA(name) * 4.0f, y = sy - 28.0f;
        CALL(A_FONT_TEXT, text_fn)(font, x + 1, y + 1, &gs, 0xff000000, 0xffff);
        uint32_t coop_player_color(void);
        CALL(A_FONT_TEXT, text_fn)(font, x, y, &gs, coop_player_color(), 0xffff);
        CALL(A_GSTR_DTOR, dtor_fn)(&gs);
    }
}

// HUD text with a shadow (also used for player names on the full map)
void hud_text(float x, float y, const char *s, uint32_t color)
{
    void *font = A_FONT_PTR ? *(void *volatile *)A_FONT_PTR : NULL;
    if (!font) return;
    gstr gs;
    CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&gs, s);
    CALL(A_FONT_TEXT, text_fn)(font, x + 1, y + 1, &gs, 0xff000000, 0xffff);
    CALL(A_FONT_TEXT, text_fn)(font, x, y, &gs, color, 0xffff);
    CALL(A_GSTR_DTOR, dtor_fn)(&gs);
}

static void THISCALL h_draw(void *self)
{
    o_draw(self);
    draw_names();
    uint8_t *a = app();
    if (!g_on || !a) return;
    int st = *(int *)(a + MENU_STATE);
    (void)0;
    if (st == 0) {
        line(3, aim2_ui("dbg_menu"), 1);
    } else if (st == ST_DEBUG) {
        uint8_t *b[128];
        int n = buildings(b, 128);
        char next[96];
        snprintf(next, sizeof next, aim2_ui("dbg_next"), n ? (char *)b[((g_sel + 1) % n + n) % n] + OBJ_NAME : "-");
        line(0, next, n > 0);
        line(1, aim2_ui("dbg_prev"), n > 0);
        line(2, aim2_ui("dbg_enter"), n > 0 && A_ENTER_BUILDING);
        line(3, aim2_ui("dbg_pos"), 1);
        line(4, aim2_ui("dbg_player"), coop_remote_by_index(0, NULL) != NULL);
        line(5, aim2_ui("dbg_passages"), A_PASSAGES != 0);
    }
}

void coop_say_to(const uint8_t *glider, const char *text);
const char *coop_my_name(void);
int coop_is_remote(const uint8_t *glider);

// target commands (state 3) aimed at another player: tell that player
static void relay_target_command(int k)
{
    if (!coop_in_flight()) return;
    uint8_t *t = *(uint8_t **)(G_PLAYER_GLIDER + GLIDER_TARGET);
    if (!t || !coop_is_remote(t)) return;
    static const char *keys[5] = {"mp_cmd_join", "mp_cmd_convince", "mp_cmd_provoke", "mp_cmd_rob", "mp_cmd_takeout"};
    char buf[160];
    snprintf(buf, sizeof buf, aim2_ui(keys[k]), coop_my_name());
    coop_say_to(t, buf);
}

// every frame (game thread)
void debugmenu_frame(void)
{
    uint8_t *a = app();
    if (!g_on || !a) return;
    int *st = (int *)(a + MENU_STATE);
    static int last_st = -1;
    int was = last_st;
    last_st = *st;
    if (*st == 3 || was == 3)          // the game may already have acted on the key and closed the menu
        for (int k = 0; k < 5; k++)
            if (key_down(0x02 + k) && !g_prev[0x02 + k]) relay_target_command(k);
    int k1 = pressed(0x02), k2 = pressed(0x03), k3 = pressed(0x04), k4 = pressed(0x05), k5 = pressed(0x06), k6 = pressed(0x07);
    if (*st == 0 && k4) {
        *st = ST_DEBUG;
    } else if (*st == ST_DEBUG) {
        if (k1) teleport(1);
        else if (k2) teleport(-1);
        else if (k3) { *st = -1; enter_nearest(); }
        else if (k4) { show_position(); *st = -1; }
        else if (k5) { teleport_player(); *st = -1; }
        else if (k6) { unlock_passages(); *st = -1; }
    }
}

void aim2_debugmenu_install(void)
{
    g_on = GetPrivateProfileIntA("Dev", "DebugMenu", 1, g_ini);
    g_names = GetPrivateProfileIntA("Coop", "ShowNames", 1, g_ini);
    if (!S_CMDMENU_DRAW) return;
    o_draw = (draw_fn)patch_call(S_CMDMENU_DRAW, h_draw);
    mlog("debug menu: %s", o_draw ? "installed" : "FAILED");
}
