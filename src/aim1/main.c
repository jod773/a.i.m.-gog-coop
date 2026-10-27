// A.I.M. co-op loader: dinput8.dll proxy for the GOG AIM.exe.
#include "../common/util.h"
#include "../common/display.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void devtools_install(void);
void devtools_set_click_messages(int on);
void devtools_wrap_dinput(void *di);
void input_reacquire(void);
void devtools_set_pad_messages(int on);
void aim1_install(void);
void aim1_coop_install(void);
void aim1_lobby_install(void);
void aim1_debug_install(void);

#define EXE_TIMESTAMP 0x43e99b7cu   // 2006-02-08, GOG AIM.exe

char g_ini[MAX_PATH];
static HMODULE g_real_dinput;

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

static int display_mode_ini(void)
{
    char v[32];
    GetPrivateProfileStringA("Video", "DisplayMode", "", v, sizeof v, g_ini);
    if (!_stricmp(v, "windowed")) return DISPLAY_WINDOWED;
    if (!_stricmp(v, "borderless")) return DISPLAY_BORDERLESS;
    return GetPrivateProfileIntA("Dev", "Windowed", 0, g_ini) ? DISPLAY_WINDOWED : DISPLAY_FULLSCREEN;
}

static VOID (WINAPI *o_GetStartupInfoA)(LPSTARTUPINFOA);

static void init_mod(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != EXE_TIMESTAMP) {
        mlog("unsupported executable (timestamp %08lx); mod disabled", nt->FileHeader.TimeDateStamp);
        return;
    }
    crashlog_watch_main_thread();          // (GetStartupInfoA runs on the main thread)
    int harness = GetPrivateProfileIntA("Dev", "Harness", 0, g_ini);
    if (harness) {
        devtools_install();
        devtools_set_click_messages(1);
    }
    display_install(display_mode_ini(), harness,
                    GetPrivateProfileIntA("Dev", "WindowX", 40, g_ini), GetPrivateProfileIntA("Dev", "WindowY", 40, g_ini));
    display_on_activate(input_reacquire);
    if (GetPrivateProfileIntA("Audio", "GlobalFocus", 1, g_ini)) audio_install();
    gamepad_install();
    devtools_set_pad_messages(1);
    aim1_install();
    void aim1_music_install(void);
    aim1_music_install();
    void aim1_screengrid_install(void);
    aim1_screengrid_install();
    aim1_coop_install();
    aim1_lobby_install();
    aim1_debug_install();
    void aim1_modopts_install(void);
    aim1_modopts_install();
    mlog("AIM co-op initialised");
}

static VOID WINAPI h_GetStartupInfoA(LPSTARTUPINFOA si)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1)) init_mod();
    o_GetStartupInfoA(si);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        {   // other programs of the folder load dinput8.dll too (SetOptions.exe): stay out of them
            uint8_t *b = (uint8_t *)GetModuleHandleA(NULL);
            IMAGE_NT_HEADERS *h = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
            if (h->FileHeader.TimeDateStamp != EXE_TIMESTAMP) return TRUE;
        }
        paths_init();
        snprintf(g_ini, sizeof g_ini, "%sAIMCoop.ini", g_root);
        char lp[MAX_PATH];
        snprintf(lp, sizeof lp, "%sAIMCoop.log", g_root);
        log_open(lp);
        mlog("AIM co-op loader attached, root=%s", g_root);
        crashlog_install();
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
                if (strcmp((char *)ibn->Name, "GetStartupInfoA")) continue;
                o_GetStartupInfoA = (void *)ft->u1.Function;
                DWORD old;
                VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
                ft->u1.Function = (uintptr_t)h_GetStartupInfoA;
                VirtualProtect(&ft->u1.Function, 4, old, &old);
            }
        }
        if (!o_GetStartupInfoA) mlog("GetStartupInfoA import not found; mod disabled");
    }
    return TRUE;
}
