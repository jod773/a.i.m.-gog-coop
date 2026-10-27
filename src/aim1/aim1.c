// A.I.M. (1) co-op: game hooks.
#include "../common/util.h"
#include <string.h>
#include <stdio.h>

// --- start-up diagnostics (logged once each) ---
static HWND (WINAPI *o_FindWindowA)(LPCSTR, LPCSTR);
static LONG (WINAPI *o_RegOpenKeyA)(HKEY, LPCSTR, PHKEY);
static LONG (WINAPI *o_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static BOOL (WINAPI *o_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static VOID (WINAPI *o_ExitProcess)(UINT);
static VOID (WINAPI *o_OutputDebugStringA)(LPCSTR);

static HWND WINAPI h_FindWindowA(LPCSTR cls, LPCSTR title)
{
    // single-instance check: allow several instances (co-op on one PC, and AIM 2 running alongside)
    if (((uintptr_t)cls >> 16) && !strcmp(cls, "SRSWC0")) return NULL;
    return o_FindWindowA(cls, title);
}

static LONG WINAPI h_RegOpenKeyA(HKEY k, LPCSTR sub, PHKEY out)
{
    LONG r = o_RegOpenKeyA(k, sub, out);
    mlog("aim1: RegOpenKeyA(%p, %s) = %ld", k, sub ? sub : "", r);
    return r;
}

static LONG WINAPI h_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD len)
{
    LONG r = o_RegQueryValueExA(k, name, res, type, data, len);
    mlog("aim1: RegQueryValueExA(%s) = %ld", name ? name : "", r);
    return r;
}

static BOOL WINAPI h_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh,
                                    DWORD fl, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    BOOL r = o_CreateProcessA(app, cmd, pa, ta, inh, fl, env, dir, si, pi);
    mlog("aim1: CreateProcessA(%s, %s) = %d", app ? app : "", cmd ? cmd : "", r);
    return r;
}

static VOID WINAPI h_ExitProcess(UINT code)
{
    void *bt[8] = {0};
    int n = CaptureStackBackTrace(1, 8, bt, NULL);
    mlog("aim1: ExitProcess(%u) from %p %p %p %p %p %p", code, n > 0 ? bt[0] : 0, n > 1 ? bt[1] : 0,
         n > 2 ? bt[2] : 0, n > 3 ? bt[3] : 0, n > 4 ? bt[4] : 0, n > 5 ? bt[5] : 0);
    o_ExitProcess(code);
}

static VOID WINAPI h_OutputDebugStringA(LPCSTR s)
{
    if (s) mlog("aim1: dbg: %s", s);
    o_OutputDebugStringA(s);
}

// --- asset overrides: AIMCoop\Data\... shadows Data\... (the game's files stay untouched) ---
static HANDLE (WINAPI *o_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *o_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// Override path for a game-relative "Data\..." path (or absolute path inside the game folder), or 0.
static int override_path(const char *path, char *out, size_t n)
{
    size_t rl = strlen(g_root);
    const char *rel = path;
    if (!_strnicmp(path, g_root, rl)) rel = path + rl;
    while (*rel == '.' && (rel[1] == '\\' || rel[1] == '/')) rel += 2;
    if (_strnicmp(rel, "data\\", 5) && _strnicmp(rel, "data/", 5)) return 0;
    snprintf(out, n, "%sAIMCoop\\%s", g_root, rel);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tpl)
{
    char alt[MAX_PATH];
    if (name && !(acc & GENERIC_WRITE) && disp == OPEN_EXISTING && override_path(name, alt, sizeof alt)) {
        HANDLE h = o_CreateFileA(alt, acc, share, sa, disp, fl, tpl);
        if (h != INVALID_HANDLE_VALUE) {
            mlog("aim1: override %s", alt + strlen(g_root));
            return h;
        }
    }
    return o_CreateFileA(name, acc, share, sa, disp, fl, tpl);
}

static HANDLE WINAPI h_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tpl)
{
    char a[MAX_PATH], alt[MAX_PATH];
    if (name && !(acc & GENERIC_WRITE) && disp == OPEN_EXISTING &&
        WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, NULL, NULL) && override_path(a, alt, sizeof alt)) {
        HANDLE h = o_CreateFileA(alt, acc, share, sa, disp, fl, tpl);
        if (h != INVALID_HANDLE_VALUE) {
            mlog("aim1: override %s", alt + strlen(g_root));
            return h;
        }
    }
    return o_CreateFileW(name, acc, share, sa, disp, fl, tpl);
}

// Videos: CBinkPlayer::Play(file, flag) calls. [Video] IntroVideos=0 skips the
// startup logos, [Dev] SkipVideos=1 (test harness) skips every video.
extern char g_ini[MAX_PATH];
static void __attribute__((thiscall)) skip_video(void *pl, const char *file, int flag) { (void)pl; (void)file; (void)flag; }

static void videos_install(void)
{
    static const uintptr_t startup[] = {0x41a11a, 0x41a129, 0x41a138, 0x41a147};
    int all = GetPrivateProfileIntA("Dev", "SkipVideos", 0, g_ini);
    if (all || !GetPrivateProfileIntA("Video", "IntroVideos", 1, g_ini)) {
        for (int i = 0; i < 4; i++) patch_call(startup[i], skip_video);
        mlog("intro videos skipped");
    }
    if (all) {                         // the game's own intro (new game) and any other video
        uint8_t *jmp = (uint8_t *)0x4a8e20;
        uint8_t stub[3] = {0xc2, 0x08, 0x00};                         // ret 8
        uint8_t expect[1] = {jmp[0]};
        patch_bytes((uintptr_t)jmp, expect, stub, 1);
        patch_bytes((uintptr_t)jmp, NULL, stub, 3);
        mlog("all videos skipped (test harness)");
    }
}

// Gamma keys (F11 darker, F12 brighter): the game steps the gamma every time it finds the key
// down, so at today's frame rates one tap runs to the end of the range. Its three reads of the
// key states (mov al, [0x541303] F11 / [0x541304] F12, twice) become calls that answer "pressed
// since the last check" (then a slow repeat while held). The F11 read comes first in each check
// and takes the new state of both keys; the two F12 reads answer from it.
static uint8_t g_gk_t[2], g_gk_held[2];
static DWORD g_gk_next[2];

unsigned __cdecl gk_check(unsigned eax)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < 2; i++) {
        uint8_t k = *(volatile uint8_t *)(0x541303 + i) & 0x80;
        uint8_t fire = 0;
        if (k && !g_gk_held[i]) { fire = 0x80; g_gk_next[i] = now + 450; }
        else if (k && (int)(now - g_gk_next[i]) >= 0) { fire = 0x80; g_gk_next[i] = now + 200; }
        g_gk_held[i] = k;
        g_gk_t[i] = fire;
    }
    return (eax & ~0xffu) | g_gk_t[0];
}
unsigned __cdecl gk_f12(unsigned eax) { return (eax & ~0xffu) | g_gk_t[1]; }

// keep ecx / edx (the game's mov al left them alone) and the top of eax
__asm__(".globl _gk_stub11\n_gk_stub11:\n push %ecx\n push %edx\n push %eax\n call _gk_check\n add $4, %esp\n pop %edx\n pop %ecx\n ret\n"
        ".globl _gk_stub12\n_gk_stub12:\n push %ecx\n push %edx\n push %eax\n call _gk_f12\n add $4, %esp\n pop %edx\n pop %ecx\n ret\n");
void gk_stub11(void);
void gk_stub12(void);

static void gamma_keys_install(void)
{
    static const struct { uintptr_t at; uint8_t was[5]; void *to; } r[] = {
        {0x420731, {0xa0, 0x03, 0x13, 0x54, 0x00}, (void *)gk_stub11},
        {0x42073a, {0xa0, 0x04, 0x13, 0x54, 0x00}, (void *)gk_stub12},
        {0x420747, {0xa0, 0x04, 0x13, 0x54, 0x00}, (void *)gk_stub12}};
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        uint8_t call[5] = {0xe8};
        *(int32_t *)(call + 1) = (int32_t)((uintptr_t)r[i].to - (r[i].at + 5));
        ok &= patch_bytes(r[i].at, r[i].was, call, 5) != 0;
    }
    mlog("gamma keys: %s", ok ? "one step per press" : "FAILED");
}
void aim1_install(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    gamma_keys_install();
    videos_install();
    void aim1_video_install(void);
    aim1_video_install();
    iat_hook_one(exe, "kernel32.dll", "CreateFileA", h_CreateFileA, (void **)&o_CreateFileA);
    iat_hook_one(exe, "kernel32.dll", "CreateFileW", h_CreateFileW, (void **)&o_CreateFileW);
    iat_hook_one(exe, "user32.dll", "FindWindowA", h_FindWindowA, (void **)&o_FindWindowA);
    iat_hook_one(exe, "advapi32.dll", "RegOpenKeyA", h_RegOpenKeyA, (void **)&o_RegOpenKeyA);
    iat_hook_one(exe, "advapi32.dll", "RegQueryValueExA", h_RegQueryValueExA, (void **)&o_RegQueryValueExA);
    iat_hook_one(exe, "kernel32.dll", "CreateProcessA", h_CreateProcessA, (void **)&o_CreateProcessA);
    iat_hook_one(exe, "kernel32.dll", "OutputDebugStringA", h_OutputDebugStringA, (void **)&o_OutputDebugStringA);
    iat_hook_all("kernel32.dll", "ExitProcess", h_ExitProcess, (void **)&o_ExitProcess);
    // Lost device (alt-tab, screen lock): when IDirect3DDevice8::Reset fails the
    // game logs "can't restore to fullscreen mode" and quits. Retry on the next
    // frame instead.
    static const uint8_t fail[] = {0x68, 0x74, 0xa6, 0x52, 0x00}, retry[] = {0xeb, 0xf8, 0x90, 0x90, 0x90};
    patch_bytes(0x00497a4e, fail, retry, sizeof fail);
    mlog("aim1: installed");
}
