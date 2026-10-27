// Debug tools: harness commands (buildings, tp, enter, pos) and a tilde
// debug menu shown as HUD notices (A.I.M. has no command menu of its own):
// tilde opens it, 1-5 pick an action, tilde again or 6 s without a key closes it.
#include "engine.h"
#include "coop.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define VT_BUILDING   0x0052d6e8   // CBuildingObject
// CBuildingObject::Run, when the player flies in: flag, building, its two names, state
#define A_ENTER_FLAG  0x00677120   // byte
#define A_ENTER_STATE 0x00677124   // int = 1
#define A_ENTER_NAME  0x00677128   // name32 (object name, +0x14)
#define A_ENTER_NAME2 0x00677148   // name32 (+0x34)
#define A_ENTER_OBJ   0x0067726c   // building object

typedef void (THISCALL *setpos_fn)(void *, float, float, float);
extern char g_ini[MAX_PATH];

static uint8_t *coop_player_glider(void) { return coop_in_flight() ? G_PLAYER_GLIDER : NULL; }
uint8_t *coop_remote_by_index(int n, const char **name);
int devtools_key_down(int dik);

// a base's exit doors (..._EXIT) are buildings too, but not places to enter
static int is_exit(const uint8_t *o)
{
    char nm[33];
    lstrcpynA(nm, (const char *)o + OBJ_NAME, sizeof nm);
    CharUpperA(nm);
    return strstr(nm, "_EXIT") != NULL;
}

static int is_building(const uint8_t *o) { return o && *(const uint32_t *)o == VT_BUILDING && o[OBJ_ALIVE] && o[OBJ_NAME] && !is_exit(o); }

static int buildings(uint8_t **out, int max)
{
    int n = 0;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd && n < max; nd = (void **)nd[1])
        if (is_building((uint8_t *)nd[0])) out[n++] = (uint8_t *)nd[0];
    return n;
}

static uint8_t *find_building(const char *name)
{
    uint8_t *b[256];
    int n = buildings(b, 256);
    for (int i = 0; i < n; i++)
        if (!_stricmp((char *)b[i] + OBJ_NAME, name)) return b[i];
    return NULL;
}

static void move_player(float x, float y, float z)
{
    uint8_t *g = coop_player_glider();
    if (!g) return;
    VFN(g, VF_OBJ_SETPOS, setpos_fn)(g, x, y, z);
}

static void teleport_to(uint8_t *b)
{
    const float *p = (const float *)(b + OBJ_POS);
    move_player(p[0] + 150.0f, p[1] + 150.0f, p[2] + 120.0f);
    char buf[96];
    snprintf(buf, sizeof buf, aim1_ui("dbg_teleported"), (char *)b + OBJ_NAME);
    coop_notify(buf);
}

static void enter(uint8_t *b)
{
    memcpy((void *)A_ENTER_NAME, b + OBJ_NAME, 32);
    memcpy((void *)A_ENTER_NAME2, b + 0x34, 32);
    *(uint8_t **)A_ENTER_OBJ = b;
    *(int *)A_ENTER_STATE = 1;
    *(uint8_t *)A_ENTER_FLAG = 1;
    mlog("debug: entering %s", (char *)b + OBJ_NAME);
}

// fly into a building as the player would (respawn.c: back in the base our mechmind was brought to)
int aim1_enter_building(const char *name)
{
    uint8_t *b = coop_player_glider() ? find_building(name) : NULL;
    if (b) enter(b);
    return b != NULL;
}

static float dist2(const uint8_t *a, const uint8_t *b)
{
    const float *p = (const float *)(a + OBJ_POS), *q = (const float *)(b + OBJ_POS);
    return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]);
}

// passage tunnels need a rank (computed from the rating) and crystals: raise both
#define A_PLAYER_STATS 0x005b7b38   // *: +0x20 rating (float), +0x24 rank, +0x34 crystals (float)
static void unlock_passages(void)
{
    uint8_t *s = *(uint8_t **)A_PLAYER_STATS;
    if (!s) return;
    if (*(float *)(s + 0x20) < 5000.0f) *(float *)(s + 0x20) = 5000.0f;
    *(float *)(s + 0x34) += 1000.0f;
    coop_notify(aim1_ui("dbg_unlocked"));
    mlog("debug: rating %.0f, crystals %.0f", *(float *)(s + 0x20), *(float *)(s + 0x34));
}

// harness commands; 1 if handled
int aim1_debug_cmd(const char *cmd, const char *arg)
{
    uint8_t *g = coop_player_glider();
    if (!strcmp(cmd, "buildings")) {
        uint8_t *b[256];
        int n = buildings(b, 256);
        for (int i = 0; i < n; i++) {
            const float *p = (const float *)(b[i] + OBJ_POS);
            mlog("buildings: %-24.32s %-24.32s %9.0f %9.0f %7.0f", (char *)b[i] + OBJ_NAME, (char *)b[i] + 0x34, p[0], p[1], p[2]);
        }
        return 1;
    }
    if (!strcmp(cmd, "tp") && g) {
        uint8_t *b = find_building(arg);
        if (b) teleport_to(b);
        else mlog("debug: no building %s", arg);
        return 1;
    }
    if (!strcmp(cmd, "enter") && g) {
        uint8_t *b = find_building(arg);
        if (b) enter(b);
        else mlog("debug: no building %s", arg);
        return 1;
    }
    if (!strcmp(cmd, "findval")) {         // findval <number>: int or float fields of the player's pilot
        double v = atof(arg);
        struct { const char *what; uint8_t *p; int n; } objs[] = {{"mechanoid", G_PLAYER_MECHANOID, 0x4000}, {"mech", G_PLAYER_MECH, 0x8c}};
        for (int k = 0; k < 2; k++)
            for (int i = 0; i + 4 <= objs[k].n; i += 2) {
                int32_t iv; float fv;
                memcpy(&iv, objs[k].p + i, 4);
                memcpy(&fv, objs[k].p + i, 4);
                if (iv == (int32_t)v || fv == (float)v) mlog("findval: %s+0x%x = %d / %g", objs[k].what, i, iv, fv);
            }
        return 1;
    }
    if (!strcmp(cmd, "unlock")) { unlock_passages(); return 1; }
    if (!strcmp(cmd, "pos") && g) {
        const float *p = (const float *)(g + OBJ_POS), *m = (const float *)(g + OBJ_MATRIX);
        mlog("debug: position %.1f %.1f %.1f | r0 %.2f %.2f %.2f r1 %.2f %.2f %.2f r2 %.2f %.2f %.2f",
             p[0], p[1], p[2], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
        return 1;
    }
    return 0;
}

// ---- tilde menu ------------------------------------------------------------------------
static int g_on, g_open, g_sel = -1, g_psel;
static DWORD g_opened;
static uint8_t g_prev[256];

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
    int d = key_down(dik), p = d && !g_prev[dik];
    g_prev[dik] = (uint8_t)d;
    return p;
}

static void show_menu(void)
{
    uint8_t *b[256];
    int n = buildings(b, 256);
    char buf[128];
    coop_notify(aim1_ui("dbg_title"));
    snprintf(buf, sizeof buf, aim1_ui("dbg_next"), n ? (char *)b[((g_sel + 1) % n + n) % n] + OBJ_NAME : "-");
    coop_notify(buf);
    coop_notify(aim1_ui("dbg_prev"));
    coop_notify(aim1_ui("dbg_enter"));
    coop_notify(aim1_ui("dbg_pos"));
    coop_notify(aim1_ui("dbg_player"));
    coop_notify(aim1_ui("dbg_passages"));
}

void aim1_debug_frame(void)
{
    if (!g_on) return;
    int t = pressed(0x29);
    int k[6];
    for (int i = 0; i < 6; i++) k[i] = pressed(0x02 + i);
    uint8_t *g = coop_player_glider();
    if (!g) { g_open = 0; return; }
    if (t) {
        g_open = !g_open;
        g_opened = GetTickCount();
        if (g_open) show_menu();
        return;
    }
    if (!g_open) return;
    if (GetTickCount() - g_opened > 6000) { g_open = 0; return; }
    uint8_t *b[256];
    int n = buildings(b, 256);
    if (k[0] && n) { g_sel = (g_sel + 1) % n; teleport_to(b[g_sel]); g_opened = GetTickCount(); }
    else if (k[1] && n) { g_sel = ((g_sel - 1) % n + n) % n; teleport_to(b[g_sel]); g_opened = GetTickCount(); }
    else if (k[2] && n) {
        uint8_t *best = b[0];
        for (int i = 1; i < n; i++) if (dist2(b[i], g) < dist2(best, g)) best = b[i];
        enter(best);
        g_open = 0;
    } else if (k[3]) {
        const float *p = (const float *)(g + OBJ_POS);
        char buf[64];
        snprintf(buf, sizeof buf, "%.0f %.0f %.0f", p[0], p[1], p[2]);
        coop_notify(buf);
        g_open = 0;
    } else if (k[5]) {
        unlock_passages();
        g_open = 0;
    } else if (k[4]) {
        const char *name = NULL;
        uint8_t *r = coop_remote_by_index(g_psel++, &name);
        if (!r) { g_psel = 0; r = coop_remote_by_index(g_psel++, &name); }
        if (r) {
            const float *p = (const float *)(r + OBJ_POS), *m = (const float *)(r + OBJ_MATRIX);
            memcpy(g + OBJ_MATRIX, m, 12 * sizeof(float));
            move_player(p[0] - m[8] * 70.0f, p[1] - m[9] * 70.0f, p[2] - m[10] * 70.0f + 12.0f);   // behind (row 2 is forward)
            char buf[96];
            snprintf(buf, sizeof buf, aim1_ui("dbg_teleported"), name);
            coop_notify(buf);
        }
        g_open = 0;
    }
}

void aim1_names_install(void);
void aim1_debug_install(void)
{
    aim1_names_install();
    g_on = GetPrivateProfileIntA("Dev", "DebugMenu", 1, g_ini);
}

// ---- player names above their gliders (HUD pass: wraps the mission timer drawing) ----
#include "../common/display.h"
#define A_HUD_FONT      0x005b7484   // *: HUD font
#define A_FONT_TEXT     0x00498500   // thiscall(font, x, y, gstr*, colour, flags)
#define S_HUD_TIMERS    0x00445d0f   // call of the mission timer drawing in the HUD
typedef void (THISCALL *text_fn)(void *, float, float, gstr *, uint32_t, int);
typedef void (THISCALL *gstr_cstr_fn)(void *, const char *);
typedef void (THISCALL *gstr_dtor_fn)(void *);
typedef void (THISCALL *hud_fn)(void *);
static hud_fn o_hud_timers;
static int g_names = 1;
uint32_t coop_player_color(void);

typedef struct { float m[16]; } mat4;
typedef struct { DWORD x, y, w, h; float zmin, zmax; } viewport8;

static void draw_names(void)
{
    void *dev = display_device(), *font = *(void **)A_HUD_FONT;
    uint8_t *me = coop_player_glider();
    if (!g_names || !dev || !font || !me) return;
    typedef HRESULT (WINAPI *gett_fn)(void *, DWORD, mat4 *);
    typedef HRESULT (WINAPI *getvp_fn)(void *, viewport8 *);
    mat4 v, pr;
    viewport8 vp;
    void **vt = *(void ***)dev;
    if (((gett_fn)vt[38])(dev, 2, &v) < 0 || ((gett_fn)vt[38])(dev, 3, &pr) < 0 || ((getvp_fn)vt[41])(dev, &vp) < 0) return;
    const char *name;
    uint8_t *g;
    for (int i = 0; (g = coop_remote_by_index(i, &name)) != NULL; i++) {
        const float *p = (const float *)(g + OBJ_POS), *pp = (const float *)(me + OBJ_POS);
        float d2 = (p[0] - pp[0]) * (p[0] - pp[0]) + (p[1] - pp[1]) * (p[1] - pp[1]) + (p[2] - pp[2]) * (p[2] - pp[2]);
        if (d2 > 2500.0f * 2500.0f) continue;
        float w[4] = {p[0], p[1], p[2] + 8.0f, 1.0f}, e[4], c[4];
        for (int k = 0; k < 4; k++) e[k] = w[0] * v.m[k] + w[1] * v.m[4 + k] + w[2] * v.m[8 + k] + w[3] * v.m[12 + k];
        for (int k = 0; k < 4; k++) c[k] = e[0] * pr.m[k] + e[1] * pr.m[4 + k] + e[2] * pr.m[8 + k] + e[3] * pr.m[12 + k];
        if (c[3] <= 0.1f) continue;
        float sx = vp.x + (c[0] / c[3] + 1.0f) * 0.5f * vp.w, sy = vp.y + (1.0f - c[1] / c[3]) * 0.5f * vp.h;
        if (sx < 0 || sy < 0 || sx > vp.x + vp.w || sy > vp.y + vp.h) continue;
        gstr gs;
        ((gstr_cstr_fn)A_GSTR_CTOR_CSTR)(&gs, name);
        float x = sx - (float)lstrlenA(name) * 4.0f, y = sy - 28.0f;
        ((text_fn)A_FONT_TEXT)(font, x + 1, y + 1, &gs, 0xff000000, 0xffff);
        ((text_fn)A_FONT_TEXT)(font, x, y, &gs, coop_player_color(), 0xffff);
        ((gstr_dtor_fn)A_GSTR_DTOR)(&gs);
    }
}

static void THISCALL h_hud_timers(void *self)
{
    o_hud_timers(self);
    draw_names();
}

void aim1_names_install(void)
{
    g_names = GetPrivateProfileIntA("Coop", "ShowNames", 1, g_ini);
    o_hud_timers = (hud_fn)patch_call(S_HUD_TIMERS, h_hud_timers);
    mlog("names: %s", o_hud_timers ? (g_names ? "on" : "off") : "FAILED");
}

// ---- options of this game for the settings menu (common/modopts) -----------------------------------
#include "../common/modopts.h"
static int ini_get(const char *s, const char *k, int def) { return GetPrivateProfileIntA(s, k, def, g_ini) != 0; }
static void ini_set(const char *s, const char *k, int v) { WritePrivateProfileStringA(s, k, v ? "1" : "0", g_ini); }
static int get_mode(void)
{
    char v[32];
    GetPrivateProfileStringA("Video", "DisplayMode", "fullscreen", v, sizeof v, g_ini);
    return !_stricmp(v, "windowed") ? 1 : !_stricmp(v, "borderless") ? 2 : 0;
}
static void set_mode(int v)
{
    static const char *const m[3] = {"fullscreen", "windowed", "borderless"};
    WritePrivateProfileStringA("Video", "DisplayMode", m[v % 3], g_ini);
}
static int get_intro(void) { return ini_get("Video", "IntroVideos", 1); }
static void set_intro(int v) { ini_set("Video", "IntroVideos", v); }
static int get_names(void) { return g_names; }
static void set_names(int v) { g_names = v; ini_set("Coop", "ShowNames", v); }
static int get_debug(void) { return g_on; }
static void set_debug(int v) { g_on = v; ini_set("Dev", "DebugMenu", v); }

void aim1_modopts_install(void)
{
    modopts_set_ui(aim1_ui);
    modopt o[] = {
        {"display", "Display mode", "Fullscreen, a normal window, or a borderless window covering the screen.",
         3, {"Fullscreen", "Windowed", "Borderless"}, get_mode, set_mode, 1, 0},
        {"intro", "Intro videos", "The logo videos played when the game starts.", 2, {"Off", "On"}, get_intro, set_intro, 1, 0},
        {"names", "Show player names", "Other players' names above their gliders in co-op games.", 2, {"Off", "On"}, get_names, set_names, 0, 0},
        {"debug", "Debug menu", "Tilde menu: teleport to buildings and players, enter buildings, unlock passages.", 2, {"Off", "On"}, get_debug, set_debug, 0, 0},
    };
    modopts_add(&o[0]);
    modopts_add_common(2);
    for (int i = 1; i < 4; i++) modopts_add(&o[i]);
}