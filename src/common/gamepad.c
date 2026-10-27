// Gamepad (XInput), both games.
//
// In flight the pad plays the game through its own key bindings (Data\CONFIG\cfg.ini [INPUT]):
// the left stick accelerates / brakes and strafes, the right stick turns and aims (as the mouse),
// the buttons press the bound keys or mouse buttons. Which button does what is set in the mod's
// ini, [Gamepad] (button = game action, e.g. RB=FIRE). In menus and bases the pad is a mouse:
// left stick moves the cursor, right stick faster, A = left click, B = right click,
// START = Esc.
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char g_ini[MAX_PATH];
int cg_in_flight(void);

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xpad;
typedef struct { DWORD packet; xpad pad; } xstate;
typedef DWORD (WINAPI *getstate_fn)(DWORD, xstate *);

static getstate_fn get_state;
static volatile int g_on;
static float g_fx, g_fy;           // fractional movement
static DWORD g_last;
static int g_dx, g_dy;             // movement not yet handed to the game
static int g_btn[3];               // mouse buttons
static uint8_t g_keys[256];        // keys held by the pad
static CRITICAL_SECTION g_cs;
static float g_slow = 700.0f, g_fast = 1800.0f, g_aim = 900.0f;   // pixels per second at full deflection
static int g_invert;

// ---- buttons and their actions ----------------------------------------------------------------
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_START, B_BACK, B_LS, B_RS, B_LB, B_RB, B_A, B_B, B_X, B_Y, B_LT, B_RT, NB };
static const char *const k_bname[NB] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS", "LB", "RB", "A", "B", "X", "Y", "LT", "RT"};
static const char *const k_bdef[NB] = {"CHANGEAMMO", "NIGHTLIGHTTOGGLE", "SHOWJOURNAL", "SHOWHOLD", "ESC", "SHOWMAP", "ALWAYSRUN", "SELECT",
                                       "FIREALT", "FIRE", "JUMP", "BRAKE", "ROCKET", "SELECT", "BRAKE", "RUN"};
static const WORD k_xmask[14] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000};
static int g_bcode[NB];            // key code: DIK (1-255), 0x101-0x103 mouse buttons, 0 none
static int g_move[4];              // FORWARD, BACKWARD, STRAFELEFT, STRAFERIGHT

// key names: the games' Data\CONFIG\DICODE.ini (NAME = third column), plus A.I.M.'s spellings
static struct { char name[24]; int code; } g_names[320];
static int g_nnames;

static void add_name(const char *n, int code)
{
    if (g_nnames < 320 && n[0]) { lstrcpynA(g_names[g_nnames].name, n, 24); g_names[g_nnames++].code = code; }
}

static void load_names(const char *dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%sData\\CONFIG\\DICODE.ini", dir);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[160], a[64], c[64];
        unsigned code;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%63s %x %63s", a, &code, c) == 3) add_name(c, (int)code);
        fclose(f);
    }
    static const struct { const char *n; int c; } extra[] = {
        {"LCtrl", 0x1d}, {"RCtrl", 0x9d}, {"LShift", 0x2a}, {"RShift", 0x36}, {"LAlt", 0x38}, {"RAlt", 0xb8},
        {"Space", 0x39}, {"Enter", 0x1c}, {"Return", 0x1c}, {"Tab", 0x0f}, {"CapsLck", 0x3a}, {"CapsLock", 0x3a},
        {"Esc", 0x01}, {"Escape", 0x01}, {"Up", 0xc8}, {"Down", 0xd0}, {"Left", 0xcb}, {"Right", 0xcd},
        {"Mouse1", 0x101}, {"Mouse2", 0x102}, {"Mouse3", 0x103}, {"Backspace", 0x0e}};
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) add_name(extra[i].n, extra[i].c);
    for (int c = 'A'; c <= 'Z'; c++) {
        static const BYTE dik[26] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                     0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
        char n[2] = {(char)c, 0};
        add_name(n, dik[c - 'A']);
    }
    for (int d = 1; d <= 10; d++) { char n[3]; snprintf(n, sizeof n, "%d", d % 10); add_name(n, 1 + d); }
}

static int key_code(const char *name)
{
    if (!name[0]) return 0;
    for (int i = 0; i < g_nnames; i++) if (!_stricmp(g_names[i].name, name)) return g_names[i].code;
    return 0;
}

// a game action (cfg.ini [INPUT] NAME, NAME2, NAME3) -> the first binding we can press
static int action_code(const char *cfg, const char *action)
{
    if (!_stricmp(action, "ESC")) return 0x01;
    if (!_strnicmp(action, "KEY:", 4)) return key_code(action + 4);
    // a mouse button binding first (A.I.M. fires only with those), else the first key
    int first = 0;
    for (int k = 0; k < 3; k++) {
        char key[40], v[40];
        snprintf(key, sizeof key, k ? "%s%d" : "%s", action, k + 1);
        GetPrivateProfileStringA("INPUT", key, "", v, sizeof v, cfg);
        int c = key_code(v);
        if (c >= 0x101 && c <= 0x103) return c;
        if (c && c < 0x100 && !first) first = c;        // (not the joystick ones)
    }
    return first;
}

static void load_bindings(void)
{
    char dir[MAX_PATH], cfg[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *s = strrchr(dir, '\\');
    if (s) s[1] = 0;
    snprintf(cfg, sizeof cfg, "%sData\\CONFIG\\cfg.ini", dir);
    load_names(dir);
    static const char *const mv[4] = {"FORWARD", "BACKWARD", "STRAFELEFT", "STRAFERIGHT"};
    for (int i = 0; i < 4; i++) g_move[i] = action_code(cfg, mv[i]);
    char line[512] = "";
    for (int i = 0; i < NB; i++) {
        char a[40];
        GetPrivateProfileStringA("Gamepad", k_bname[i], k_bdef[i], a, sizeof a, g_ini);
        g_bcode[i] = action_code(cfg, a);
        snprintf(line + strlen(line), sizeof line - strlen(line), " %s=%s(%x)", k_bname[i], a, g_bcode[i]);
    }
    mlog("gamepad: move %x %x %x %x;%s", g_move[0], g_move[1], g_move[2], g_move[3], line);
}

// ---- polling ----------------------------------------------------------------------------------
static float axis(SHORT v)
{
    float f = v / 32767.0f;
    float dz = 0.22f;
    if (f > -dz && f < dz) return 0;
    f = f > 0 ? (f - dz) / (1 - dz) : (f + dz) / (1 - dz);
    return f * fabsf(f);           // finer control near the centre
}

int gamepad_enabled(void) { return g_on; }

void gamepad_set_enabled(int on)
{
    g_on = on && get_state;
    WritePrivateProfileStringA("Input", "Gamepad", on ? "1" : "0", g_ini);
}

// test harness: a fake pad state for ms milliseconds
static xstate g_fake;
static volatile DWORD g_fake_until;
void gamepad_fake(int lx, int ly, int rx, int ry, int buttons, int ms)
{
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.pad.lx = (SHORT)lx; g_fake.pad.ly = (SHORT)ly; g_fake.pad.rx = (SHORT)rx; g_fake.pad.ry = (SHORT)ry;
    g_fake.pad.buttons = (WORD)buttons;
    g_fake.pad.lt = (buttons & 0x10000) ? 255 : 0;
    g_fake.pad.rt = (buttons & 0x20000) ? 255 : 0;
    g_fake_until = GetTickCount() + ms;
}

// an analog amount as a key held part of the time (100 ms cycles): half deflection, half thrust
static int pulse(float amount, DWORD now)
{
    if (amount <= 0.05f) return 0;
    if (amount >= 0.9f) return 1;
    return (float)(now % 100) < amount * 100.0f;
}

// polls the pad; accumulates movement since the last poll
void gamepad_poll(void)
{
    if (!g_on) return;
    xstate s;
    DWORD now = GetTickCount();
    float dt = g_last ? (now - g_last) / 1000.0f : 0;
    if (dt > 0.1f) dt = 0.1f;
    g_last = now;
    int found = 0;
    if ((int)(g_fake_until - now) > 0) { s = g_fake; found = 1; }
    for (DWORD i = 0; i < 4 && !found; i++)
        if (get_state(i, &s) == 0) found = 1;
    EnterCriticalSection(&g_cs);
    uint8_t keys[256];
    int btn[3] = {0, 0, 0};
    memset(keys, 0, sizeof keys);
    if (found) {
        int flight = cg_in_flight();
        int held[NB];
        for (int i = 0; i < 14; i++) held[i] = (s.pad.buttons & k_xmask[i]) != 0;
        held[B_LT] = s.pad.lt > 100;
        held[B_RT] = s.pad.rt > 100;
        float vx, vy;
        if (flight) {
            // right stick: turn and aim; left stick: thrust and strafe
            vx = axis(s.pad.rx) * g_aim;
            vy = -axis(s.pad.ry) * g_aim * (g_invert ? -1 : 1);
            float fwd = axis(s.pad.ly), side = axis(s.pad.lx);
            if (g_move[0] && pulse(fwd, now)) keys[g_move[0]] = 1;
            if (g_move[1] && pulse(-fwd, now)) keys[g_move[1]] = 1;
            if (g_move[2] && pulse(-side, now)) keys[g_move[2]] = 1;
            if (g_move[3] && pulse(side, now)) keys[g_move[3]] = 1;
            for (int i = 0; i < NB; i++) {
                int c = g_bcode[i];
                if (!held[i] || !c) continue;
                if (c >= 0x101 && c <= 0x103) btn[c - 0x101] = 1;
                else if (c < 256) keys[c] = 1;
            }
        } else {
            vx = axis(s.pad.lx) * g_slow + axis(s.pad.rx) * g_fast;
            vy = -(axis(s.pad.ly) * g_slow + axis(s.pad.ry) * g_fast);
            btn[0] = held[B_A];
            btn[1] = held[B_B];
            if (held[B_START]) keys[0x01] = 1;
        }
        g_fx += vx * dt;
        g_fy += vy * dt;
        int ix = (int)g_fx, iy = (int)g_fy;
        g_fx -= ix;
        g_fy -= iy;
        g_dx += ix;
        g_dy += iy;
    }
    memcpy(g_btn, btn, sizeof btn);
    memcpy(g_keys, keys, sizeof keys);
    LeaveCriticalSection(&g_cs);
}

// takes the accumulated movement and the mouse button states
void gamepad_take(int *dx, int *dy, int btn[3])
{
    EnterCriticalSection(&g_cs);
    *dx = g_dx; *dy = g_dy;
    g_dx = g_dy = 0;
    memcpy(btn, g_btn, sizeof g_btn);
    LeaveCriticalSection(&g_cs);
}

// the keys the pad holds (1 = down)
void gamepad_keys(uint8_t out[256])
{
    EnterCriticalSection(&g_cs);
    memcpy(out, g_keys, 256);
    LeaveCriticalSection(&g_cs);
}

void gamepad_install(void)
{
    InitializeCriticalSection(&g_cs);
    static const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !get_state; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) get_state = (getstate_fn)GetProcAddress(m, "XInputGetState");
    }
    char b[32];
    GetPrivateProfileStringA("Input", "GamepadSpeed", "700", b, sizeof b, g_ini);
    g_slow = (float)atof(b);
    GetPrivateProfileStringA("Input", "GamepadFastSpeed", "1800", b, sizeof b, g_ini);
    g_fast = (float)atof(b);
    GetPrivateProfileStringA("Input", "GamepadAimSpeed", "900", b, sizeof b, g_ini);
    g_aim = (float)atof(b);
    g_invert = GetPrivateProfileIntA("Input", "GamepadInvertY", 0, g_ini);
    g_on = get_state && GetPrivateProfileIntA("Input", "Gamepad", 0, g_ini);
    mlog("gamepad: %s%s", get_state ? "XInput found" : "no XInput", g_on ? ", on" : "");
    load_bindings();
    void devtools_start_pad(void);
    devtools_start_pad();
}
