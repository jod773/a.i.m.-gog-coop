// A.I.M. 2: Clan Wars loader: dinput8.dll proxy for the GOG executable (addresses in addr_cw.h).
#include "aim2.h"
#include "../common/lang.h"
#include "../common/display.h"
#include "coop.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void devtools_install(void);
void devtools_wrap_dinput(void *di);
void input_reacquire(void);


static HMODULE g_real_dinput;

// ---- dinput8 proxy -------------------------------------------------------------
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    if (!g_real_dinput) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\dinput8.dll");
        g_real_dinput = LoadLibraryA(p);
    }
    DI8Create_t f = g_real_dinput ? (DI8Create_t)GetProcAddress(g_real_dinput, "DirectInput8Create") : NULL;
    HRESULT hr = f ? f(inst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out) devtools_wrap_dinput(*out);
    return hr;
}

// ---- UI strings defined by the language pack (lang.ini [UI]) ----------------
const char *aim2_ui(const char *key)
{
    static char buf[8][256];
    static int slot;
    char li[MAX_PATH], tmp[256];
    snprintf(li, sizeof li, "%sData\\Lang\\%s\\lang.ini", g_root, lang_code());
    GetPrivateProfileStringA("UI", key, key, tmp, sizeof tmp, li);
    char *c = utf8_to_cp(tmp, 1251);
    char *out = buf[slot++ & 7];
    lstrcpynA(out, c, 256);
    free(c);
    return out;
}

void aim2_apply_language(void)
{
    int *tdb = A_TEXTDB_PTR ? *(int **)A_TEXTDB_PTR : NULL;
    if (tdb) {
        typedef char (THISCALL *load_fn)(void *);
        char ok = VFN(tdb, 0x0c, load_fn)(tdb);
        mlog("text database re-read: %d", ok);
    }
}

// ---- deferred initialisation ---------------------------------------------------
// DllMain runs under the loader lock, so the real work is done on the first
// call the C runtime startup makes to GetStartupInfoA/W (all DLLs are loaded,
// game code hasn't run yet).
static VOID (WINAPI *o_GetStartupInfo)(void *);

// ---- how the game ends -----------------------------------------------------------
// A session that just stops (no CRASH line, no Windows error) ended through one of these: the
// callers are logged ("exe+XXXX" = address - 0x400000 - ASLR delta, as in the disassembly).
static VOID (WINAPI *o_ExitProcess)(UINT);
static VOID (WINAPI *o_PostQuitMessage)(int);
static BOOL (WINAPI *o_DestroyWindow)(HWND);

static void log_callers(const char *what, unsigned arg)
{
    void *bt[6] = {0};
    int n = CaptureStackBackTrace(2, 6, bt, NULL);
    char b[200];
    int k = 0;
    for (int i = 0; i < n && k < (int)sizeof b - 24; i++) {
        uintptr_t a = (uintptr_t)bt[i], ex = (uintptr_t)GetModuleHandleA(NULL);
        if (a >= ex && a < ex + 0x400000) k += snprintf(b + k, sizeof b - k, " exe+%x", (unsigned)(a - ex));
        else k += snprintf(b + k, sizeof b - k, " %p", bt[i]);
    }
    b[k] = 0;
    mlog("%s(%u) from%s", what, arg, b);
}
static VOID WINAPI h_ExitProcess(UINT c) { log_callers("ExitProcess", c); o_ExitProcess(c); }
static VOID WINAPI h_PostQuitMessage(int c) { log_callers("PostQuitMessage", (unsigned)c); o_PostQuitMessage(c); }
static BOOL WINAPI h_DestroyWindow(HWND w)
{
    if (w && !GetParent(w)) log_callers("DestroyWindow (top level)", 0);
    return o_DestroyWindow(w);
}

// the Bink player (story videos): how it is set up and played (#17: frozen after a skipped video)
typedef int (THISCALL *bink_init_fn)(void *, HWND, int, int, int);
typedef void (THISCALL *bink_play_fn)(void *, const char *, int);
typedef int (THISCALL *bink_done_fn)(void *);
static bink_init_fn o_bink_initwin, o_bink_initfull;
static bink_play_fn o_bink_play;
static bink_done_fn o_bink_done;
static int THISCALL bink_initwin(void *p, HWND w, int x, int y, int z)
{
    int r = o_bink_initwin(p, w, x, y, z);
    mlog("video: InitWindow(%d, %d, %d) = %08x", x, y, z, r);
    return r;
}
static int THISCALL bink_initfull(void *p, HWND w, int x, int y, int z)
{
    log_callers("video: InitFullscreen", (unsigned)x);
    int r = o_bink_initfull(p, w, x, y, z);
    mlog("video: InitFullscreen(%d, %d, %d) = %08x", x, y, z, r);
    return r;
}
static void THISCALL bink_play(void *p, const char *f, int skippable)
{
    mlog("video: PlayBink(%s, %d)", f ? f : "", skippable);
    o_bink_play(p, f, skippable);
    mlog("video: PlayBink done");
}
static int THISCALL bink_done(void *p)
{
    int r = o_bink_done(p);
    mlog("video: Done");
    return r;
}
static void bink_log_install(void)
{
    if (!VT_BINK_PLAYER) return;
    void **vt = (void **)VT_BINK_PLAYER;
    void *f;
    uint32_t cur;
#define HOOK_VF(off, fn, orig) cur = (uint32_t)(uintptr_t)vt[(off) / 4]; *(void **)&orig = (void *)(uintptr_t)cur; \
    f = (void *)fn; patch_bytes((uintptr_t)&vt[(off) / 4], &cur, &f, 4)
    HOOK_VF(0x10, bink_initwin, o_bink_initwin);
    HOOK_VF(0x14, bink_initfull, o_bink_initfull);
    HOOK_VF(0x1c, bink_play, o_bink_play);
    HOOK_VF(0x28, bink_done, o_bink_done);
#undef HOOK_VF
}

static void exitlog_install(void)
{
    bink_log_install();
    iat_hook_all("kernel32.dll", "ExitProcess", h_ExitProcess, (void **)&o_ExitProcess);
    iat_hook_all("user32.dll", "PostQuitMessage", h_PostQuitMessage, (void **)&o_PostQuitMessage);
    iat_hook_all("user32.dll", "DestroyWindow", h_DestroyWindow, (void **)&o_DestroyWindow);
}

static void init_mod(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    g_exe_delta = (uintptr_t)exe - 0x400000;
    if (g_exe_delta) mlog("executable relocated by %+ld bytes (ASLR)", (long)g_exe_delta);
    crashlog_install();
    if (nt->FileHeader.TimeDateStamp != EXE_TIMESTAMP) {
        mlog("unsupported executable (timestamp %08lx); mod disabled", nt->FileHeader.TimeDateStamp);
        return;
    }
    lang_init(MOD_INI, 1251, MOD_NATIVE_LANG, MOD_NATIVE_LANG_NAME);
    if (GetPrivateProfileIntA("Audio", "GlobalFocus", 1, g_ini)) audio_install();
    gamepad_install();
    lang_install_file_hooks();
    if (FEAT_OPTIONS) aim2_options_install();
    if (FEAT_VFS) aim2_vfs_install();
    if (FEAT_SAVES) aim2_saves_install();
    if (FEAT_INTRO) aim2_intro_install();
    crashlog_watch_main_thread();          // (the startup call runs on the main thread)
    exitlog_install();
    int harness = GetPrivateProfileIntA("Dev", "Harness", 0, g_ini);
    if (harness) devtools_install();
    display_install(aim2_display_mode_ini(), harness,
                    GetPrivateProfileIntA("Dev", "WindowX", 40, g_ini), GetPrivateProfileIntA("Dev", "WindowY", 40, g_ini));
    display_on_activate(input_reacquire);
    if (FEAT_DISPLAY_APPLY) aim2_display_install();
    if (FEAT_COOP) aim2_coop_install();
    if (FEAT_LOBBY) aim2_lobby_install();
    if (FEAT_ECONOMY) aim2_economy_install();
    if (FEAT_COOP) aim2_debugmenu_install();
    aim2_modopts_install();
    aim2_shadows_install();
    mlog(MOD_NAME " initialised");
}

static VOID WINAPI h_GetStartupInfo(void *si)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1)) init_mod();
    o_GetStartupInfo(si);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        paths_init();
        char lp[MAX_PATH];
        snprintf(lp, sizeof lp, "%s" MOD_LOG, g_root);
        log_open(lp);
        mlog(MOD_NAME " loader attached, root=%s", g_root);
        // hook only the executable's import of GetStartupInfoA (MSVC 7) or ...W (MSVC 10)
        HMODULE exe = GetModuleHandleA(NULL);
        uint8_t *base = (uint8_t *)exe;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (IMAGE_IMPORT_DESCRIPTOR *imp = (void *)(base + dd.VirtualAddress); imp->Name; imp++) {
            if (_stricmp((char *)(base + imp->Name), "kernel32.dll")) continue;
            IMAGE_THUNK_DATA *ft = (void *)(base + imp->FirstThunk);
            IMAGE_THUNK_DATA *ot = (void *)(base + imp->OriginalFirstThunk);
            for (; ft->u1.Function; ft++, ot++) {
                if (ot->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                IMAGE_IMPORT_BY_NAME *ibn = (void *)(base + ot->u1.AddressOfData);
                if (strcmp((char *)ibn->Name, "GetStartupInfoA") && strcmp((char *)ibn->Name, "GetStartupInfoW")) continue;
                o_GetStartupInfo = (void *)ft->u1.Function;
                DWORD old;
                VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
                ft->u1.Function = (uintptr_t)h_GetStartupInfo;
                VirtualProtect(&ft->u1.Function, 4, old, &old);
            }
        }
        if (!o_GetStartupInfo) mlog("GetStartupInfo import not found; mod disabled");
    }
    return TRUE;
}
