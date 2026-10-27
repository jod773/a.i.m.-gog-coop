#include "lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

lang_t g_langs[MAX_LANGS];
int g_nlangs;
int g_lang;
char g_ini[MAX_PATH];

static int g_cp = 1251;
static char g_data_prefix[MAX_PATH];   // "<root>data\" lower-case
static size_t g_data_prefix_len;
static strmap *g_cache;                // lower-case full path -> redirected path ("" = none)
static CRITICAL_SECTION g_cs;

// Lower-case ASCII and cp1251 Cyrillic independently of the Windows code page
// (CharLowerA would use the system ANSI page, which differs between locales).
static void lower_cp(char *s)
{
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
        else if (*p >= 0xC0 && *p <= 0xDF) *p += 32;
        else if (*p == 0xA8) *p = 0xB8;
    }
}

static void read_utf8_ini(const char *file, const char *sec, const char *key, const char *def, char *out, int outsz)
{
    char tmp[256];
    GetPrivateProfileStringA(sec, key, def, tmp, sizeof tmp, file);
    char *c = utf8_to_cp(tmp, g_cp);
    lstrcpynA(out, c, outsz);
    free(c);
}

void lang_init(const char *ini_name, int cp, const char *native_code, const char *native_name)
{
    g_cp = cp;
    InitializeCriticalSection(&g_cs);
    snprintf(g_ini, sizeof g_ini, "%s%s", g_root, ini_name);
    snprintf(g_data_prefix, sizeof g_data_prefix, "%sdata\\", g_root);
    lower_cp(g_data_prefix);
    g_data_prefix_len = strlen(g_data_prefix);
    g_cache = strmap_new();

    // discover languages: Data\Lang\<code>\lang.ini
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%sData\\Lang\\*", g_root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int order[MAX_LANGS];
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            if (g_nlangs >= MAX_LANGS) break;
            char li[MAX_PATH];
            snprintf(li, sizeof li, "%sData\\Lang\\%s\\lang.ini", g_root, fd.cFileName);
            if (!file_exists(li)) continue;
            lang_t *L = &g_langs[g_nlangs];
            lstrcpynA(L->code, fd.cFileName, sizeof L->code);
            CharLowerA(L->code);
            read_utf8_ini(li, "Language", "Name", fd.cFileName, L->name, sizeof L->name);
            order[g_nlangs] = GetPrivateProfileIntA("Language", "Order", 100, li);
            g_nlangs++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // sort by Order
    for (int i = 0; i < g_nlangs; i++)
        for (int j = i + 1; j < g_nlangs; j++)
            if (order[j] < order[i]) {
                lang_t t = g_langs[i]; g_langs[i] = g_langs[j]; g_langs[j] = t;
                int o = order[i]; order[i] = order[j]; order[j] = o;
            }
    if (!g_nlangs) {
        lstrcpynA(g_langs[0].code, native_code, sizeof g_langs[0].code);   // the game's own language
        lstrcpynA(g_langs[0].name, native_name, sizeof g_langs[0].name);
        g_nlangs = 1;
    }

    char code[16];
    GetPrivateProfileStringA("General", "Language", "", code, sizeof code, g_ini);
    g_lang = lang_find(code);
    if (g_lang < 0) {
        g_lang = 0;                     // first run: the first language (by Order)
        WritePrivateProfileStringA("General", "Language", g_langs[g_lang].code, g_ini);
    }
    mlog("languages: %d, current: %s", g_nlangs, g_langs[g_lang].code);
}

const char *lang_code(void) { return g_langs[g_lang].code; }

int lang_find(const char *code)
{
    for (int i = 0; i < g_nlangs; i++)
        if (!_stricmp(g_langs[i].code, code)) return i;
    return -1;
}

void lang_select(int idx)
{
    if (idx < 0 || idx >= g_nlangs) return;
    EnterCriticalSection(&g_cs);
    g_lang = idx;
    // drop the redirect cache
    g_cache = strmap_new();     // old one is leaked on purpose: lookups may be in flight
    LeaveCriticalSection(&g_cs);
    WritePrivateProfileStringA("General", "Language", g_langs[idx].code, g_ini);
    mlog("language switched to %s", g_langs[idx].code);
}

int lang_redirect(const char *in, char *out, size_t outsz)
{
    if (!in || !*in) return 0;
    char full[MAX_PATH], low[MAX_PATH];
    DWORD n = GetFullPathNameA(in, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) return 0;
    lstrcpyA(low, full);
    lower_cp(low);
    if (strncmp(low, g_data_prefix, g_data_prefix_len) != 0) return 0;
    const char *rest = low + g_data_prefix_len;
    if (!strncmp(rest, "lang\\", 5)) return 0;   // already redirected

    EnterCriticalSection(&g_cs);
    strmap *cache = g_cache;
    const char *hit = strmap_get(cache, low);
    int r = 0;
    if (hit) {
        if (*hit) { lstrcpynA(out, hit, (int)outsz); r = 1; }
    } else {
        char cand[MAX_PATH];
        snprintf(cand, sizeof cand, "%sData\\Lang\\%s\\%s", g_root, lang_code(), full + g_data_prefix_len);
        if (file_exists(cand)) {
            strmap_put(cache, low, cand);
            lstrcpynA(out, cand, (int)outsz);
            r = 1;
        } else {
            strmap_put(cache, low, "");
        }
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

// ---- hooks ------------------------------------------------------------------
static HANDLE (WINAPI *o_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static DWORD (WINAPI *o_GetFileAttributesA)(LPCSTR);
static DWORD (WINAPI *o_GetPrivateProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
static UINT (WINAPI *o_GetPrivateProfileIntA)(LPCSTR, LPCSTR, INT, LPCSTR);
static DWORD (WINAPI *o_GetPrivateProfileSectionA)(LPCSTR, LPSTR, DWORD, LPCSTR);

static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    char red[MAX_PATH];
    if (!(acc & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) && disp == OPEN_EXISTING &&
        lang_redirect(name, red, sizeof red))
        name = red;
    return o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
}

static HANDLE (WINAPI *o_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// The engine's image loader converts its ANSI path to UTF-16 with CP_ACP
// before calling CreateFileW; convert back the same way to look it up.
static HANDLE WINAPI h_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (name && !(acc & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) && disp == OPEN_EXISTING) {
        char a[MAX_PATH], red[MAX_PATH];
        BOOL lossy = FALSE;
        if (WideCharToMultiByte(CP_ACP, 0, name, -1, a, MAX_PATH, NULL, &lossy) && !lossy &&
            lang_redirect(a, red, sizeof red)) {
            WCHAR w[MAX_PATH];
            if (MultiByteToWideChar(CP_ACP, 0, red, -1, w, MAX_PATH))
                return o_CreateFileW(w, acc, share, sa, disp, flags, tmpl);
        }
    }
    return o_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

static DWORD WINAPI h_GetFileAttributesA(LPCSTR name)
{
    char red[MAX_PATH];
    if (lang_redirect(name, red, sizeof red)) name = red;
    return o_GetFileAttributesA(name);
}

static DWORD WINAPI h_GetPrivateProfileStringA(LPCSTR s, LPCSTR k, LPCSTR d, LPSTR out, DWORD n, LPCSTR f)
{
    char red[MAX_PATH];
    if (lang_redirect(f, red, sizeof red)) f = red;
    return o_GetPrivateProfileStringA(s, k, d, out, n, f);
}

static UINT WINAPI h_GetPrivateProfileIntA(LPCSTR s, LPCSTR k, INT d, LPCSTR f)
{
    char red[MAX_PATH];
    if (lang_redirect(f, red, sizeof red)) f = red;
    return o_GetPrivateProfileIntA(s, k, d, f);
}

static DWORD WINAPI h_GetPrivateProfileSectionA(LPCSTR s, LPSTR out, DWORD n, LPCSTR f)
{
    char red[MAX_PATH];
    if (lang_redirect(f, red, sizeof red)) f = red;
    return o_GetPrivateProfileSectionA(s, out, n, f);
}

void lang_install_file_hooks(void)
{
    int n = 0;
    n += iat_hook_all("kernel32.dll", "CreateFileA", h_CreateFileA, (void **)&o_CreateFileA);
    n += iat_hook_all("kernel32.dll", "CreateFileW", h_CreateFileW, (void **)&o_CreateFileW);
    n += iat_hook_all("kernel32.dll", "GetFileAttributesA", h_GetFileAttributesA, (void **)&o_GetFileAttributesA);
    n += iat_hook_all("kernel32.dll", "GetPrivateProfileStringA", h_GetPrivateProfileStringA, (void **)&o_GetPrivateProfileStringA);
    n += iat_hook_all("kernel32.dll", "GetPrivateProfileIntA", h_GetPrivateProfileIntA, (void **)&o_GetPrivateProfileIntA);
    n += iat_hook_all("kernel32.dll", "GetPrivateProfileSectionA", h_GetPrivateProfileSectionA, (void **)&o_GetPrivateProfileSectionA);
    mlog("file hooks installed in %d import slots", n);
}
