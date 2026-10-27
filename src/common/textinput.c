// A line of text typed by the player (co-op name entry in the lobbies). The
// keys come from the game's DirectInput keyboard (devtools.c hands every
// keyboard read to us); while a line is being typed the game sees no keys.
#include "util.h"
#include <string.h>

static volatile int g_active;
static volatile int g_result;          // 0 typing, 1 confirmed, -1 cancelled
static volatile DWORD g_drain_until;   // after Enter/Esc: the game still doesn't see the key
static char g_text[128];
static int g_free;                     // chat: any printable character
static int g_max = 24;
static uint8_t g_prev[256];
static CRITICAL_SECTION g_cs;
static int g_cs_ok;
// A press can come from DirectInput (state reads, buffered events) and from WM_KEYDOWN (display.c).
// Both are used: some setups deliver only one of them, or only some keys through WM_KEYDOWN. The
// same key from the other source within a moment is the same press.
static DWORD g_last_t[256];
static uint8_t g_last_src[256];
enum { SRC_DI = 1, SRC_WM = 2 };
static uint8_t g_held[256];            // held when typing started
static DWORD g_start_t;

static void lock(void)
{
    if (!g_cs_ok) { InitializeCriticalSection(&g_cs); g_cs_ok = 1; }
    EnterCriticalSection(&g_cs);
}

void textinput_start(const char *initial, int max)
{
    lock();
    lstrcpynA(g_text, initial ? initial : "", sizeof g_text);
    g_max = max > 0 && max < (int)sizeof g_text ? max : (int)sizeof g_text - 1;
    g_text[g_max] = 0;
    g_result = 0;
    g_free = 0;
    // Keys held when typing starts (the chat key itself) don't count. Marking every key as held
    // swallowed the first press of each key with buffered input (no release came first): the chat
    // "didn't register" letters until they had been pressed once.
    memset(g_prev, 0, sizeof g_prev);
    for (int sc = 1; sc < 0x80; sc++) {
        UINT vk = MapVirtualKeyA(sc, 1 /*MAPVK_VSC_TO_VK*/);
        if (vk && (GetAsyncKeyState(vk) & 0x8000)) g_prev[sc] = 0x80;
    }
    memset(g_last_t, 0, sizeof g_last_t);
    memcpy(g_held, g_prev, sizeof g_held);
    g_start_t = GetTickCount();
    g_active = 1;
    LeaveCriticalSection(&g_cs);
}

int textinput_active(void) { return g_active; }

void textinput_start_free(const char *initial, int max)    // a chat line: any text
{
    textinput_start(initial, max);
    g_free = 1;
}

void textinput_stop(void) { g_active = 0; }

// 0 while typing, 1 confirmed (Enter), -1 cancelled (Esc); text = the line so far
int textinput_poll(char *text, int n)
{
    lock();
    if (text) lstrcpynA(text, g_text, n);
    int r = g_result;
    LeaveCriticalSection(&g_cs);
    return r;
}

static void key(int sc, const uint8_t *state, int src)
{
    if (sc < 0 || sc > 255) return;
    DWORD now = GetTickCount();
    if (g_last_t[sc] && g_last_src[sc] != src && now - g_last_t[sc] < 200) return;   // the other source's copy
    g_last_t[sc] = now ? now : 1;
    g_last_src[sc] = (uint8_t)src;
    if (sc == 0x1c || sc == 0x9c) { if (g_text[0]) { g_result = 1; g_active = 0; g_drain_until = GetTickCount() + 400; } return; }   // Enter
    if (sc == 0x01) { g_result = -1; g_active = 0; g_drain_until = GetTickCount() + 400; return; }   // Esc
    int len = (int)strlen(g_text);
    if (sc == 0x0e) { if (len) g_text[len - 1] = 0; return; }                                 // Backspace
    UINT vk = MapVirtualKeyA(sc, 1 /*MAPVK_VSC_TO_VK*/);
    if (!vk) return;
    BYTE ks[256] = {0};
    if ((state && (state[0x2a] | state[0x36]) & 0x80) || (GetKeyState(VK_SHIFT) & 0x8000)) ks[VK_SHIFT] = 0x80;
    WORD ch[2] = {0};
    if (ToAscii(vk, sc, ks, ch, 0) != 1) return;
    char c = (char)ch[0];
    // names travel as plain text and end up in file names: keep them simple
    if (g_free ? (unsigned char)c < 32 :
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.'))
        return;
    if (len < g_max) { g_text[len] = c; g_text[len + 1] = 0; }
}

// keyboard state read (256 bytes): returns 1 if the game must not see it
static int draining(void) { return (int)(g_drain_until - GetTickCount()) > 0; }

int textinput_feed_state(const uint8_t *state)
{
    if (!g_active) return draining();
    lock();
    for (int i = 0; i < 256 && g_active; i++) {
        if ((state[i] & 0x80) && !(g_prev[i] & 0x80)) key(i, state, SRC_DI);
        g_prev[i] = state[i];
    }
    LeaveCriticalSection(&g_cs);
    return 1;
}

// buffered keyboard event (scan code, pressed): returns 1 if swallowed
int textinput_feed_event(int sc, int down)
{
    if (!g_active) return draining();
    lock();
    if (sc >= 0 && sc < 256) {
        if (down && !(g_prev[sc] & 0x80)) key(sc, g_prev, SRC_DI);
        g_prev[sc] = down ? 0x80 : 0;
    }
    LeaveCriticalSection(&g_cs);
    return 1;
}

// WM_KEYDOWN from the game window (display.c): every press, with key repeat
void textinput_wm_key(int sc)
{
    if (!g_active) return;
    lock();
    // the chat key's own message can come in after typing started
    if (!(sc >= 0 && sc < 256 && (g_held[sc] & 0x80) && GetTickCount() - g_start_t < 400)) key(sc, NULL, SRC_WM);
    LeaveCriticalSection(&g_cs);
}
