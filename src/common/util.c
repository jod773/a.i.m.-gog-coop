#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <tlhelp32.h>

char g_root[MAX_PATH];
uintptr_t g_exe_delta;
static FILE *g_log;
static CRITICAL_SECTION g_log_cs;

void log_open(const char *path)
{
    InitializeCriticalSection(&g_log_cs);
    // the log keeps every session (crashes of earlier sessions stay readable; delete the file to
    // clear it) - except in test copies ([Dev] Harness=1 in the ini next to it), read from the top
    char ini[MAX_PATH];
    lstrcpynA(ini, path, MAX_PATH);
    char *dot = strrchr(ini, '.');
    if (dot) lstrcpyA(dot, ".ini");
    int harness = GetPrivateProfileIntA("Dev", "Harness", 0, ini);
    g_log = fopen(path, harness ? "w" : "a");
    if (g_log && !harness) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fseek(g_log, 0, SEEK_END);
        if (ftell(g_log) > 0) fputs("\n", g_log);
        fprintf(g_log, "==================== %04d-%02d-%02d %02d:%02d:%02d (mod built %s %s) ====================\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, __DATE__, __TIME__);
        fflush(g_log);
    }
}

void mlog(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

void paths_init(void)
{
    GetModuleFileNameA(NULL, g_root, MAX_PATH);
    char *p = strrchr(g_root, '\\');
    if (p) p[1] = 0;
}

int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

char *utf8_to_cp(const char *s, int cp)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc(n * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    int m = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
    char *r = malloc(m);
    WideCharToMultiByte(cp, 0, w, -1, r, m, NULL, NULL);
    free(w);
    return r;
}

// ---------------------------------------------------------------------------
int patch_bytes(uintptr_t addr, const void *expect, const void *data, size_t len)
{
    if (expect && memcmp((void *)addr, expect, len) != 0) {
        mlog("patch_bytes: unexpected bytes at %08x, not patching", (unsigned)addr);
        return 0;
    }
    DWORD old;
    VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)addr, data, len);
    VirtualProtect((void *)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
    return 1;
}

uintptr_t patch_call(uintptr_t addr, void *target)
{
    uint8_t *p = (uint8_t *)addr;
    if (p[0] != 0xE8) {
        mlog("patch_call: no call at %08x", (unsigned)addr);
        return 0;
    }
    uintptr_t old = addr + 5 + *(int32_t *)(p + 1);
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));
    uint8_t buf[5] = {0xE8};
    memcpy(buf + 1, &rel, 4);
    patch_bytes(addr, NULL, buf, 5);
    return old;
}

void *hook_jmp(uintptr_t addr, const void *expect, size_t len, void *target)
{
    if (len < 5 || memcmp((void *)addr, expect, len) != 0) {
        mlog("hook_jmp: unexpected bytes at %08x, not hooking", (unsigned)addr);
        return NULL;
    }
    uint8_t *tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, (void *)addr, len);
    tr[len] = 0xE9;
    *(int32_t *)(tr + len + 1) = (int32_t)((addr + len) - ((uintptr_t)tr + len + 5));
    uint8_t buf[32];
    memset(buf, 0x90, sizeof buf);
    buf[0] = 0xE9;
    *(int32_t *)(buf + 1) = (int32_t)((uintptr_t)target - (addr + 5));
    patch_bytes(addr, NULL, buf, len);
    return tr;
}

static int iat_hook_module(HMODULE mod, const char *dll, const char *func, void *repl, void *orig_fn)
{
    uint8_t *base = (uint8_t *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (void *)(base + dd.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char *)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *ft = (void *)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA *ot = imp->OriginalFirstThunk ? (void *)(base + imp->OriginalFirstThunk) : ft;
        for (; ft->u1.Function; ft++, ot++) {
            int match = 0;
            if (!(ot->u1.Ordinal & IMAGE_ORDINAL_FLAG) && ot != ft) {
                IMAGE_IMPORT_BY_NAME *ibn = (void *)(base + ot->u1.AddressOfData);
                match = strcmp((char *)ibn->Name, func) == 0;
            } else {
                match = (void *)ft->u1.Function == orig_fn;
            }
            if (match && (void *)ft->u1.Function != repl) {
                DWORD old;
                VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &old);
                ft->u1.Function = (uintptr_t)repl;
                VirtualProtect(&ft->u1.Function, 4, old, &old);
                n++;
            }
        }
    }
    return n;
}

int iat_hook_one(HMODULE mod, const char *dll, const char *func, void *repl, void **orig)
{
    HMODULE hd = GetModuleHandleA(dll);
    void *real = hd ? (void *)GetProcAddress(hd, func) : NULL;
    if (orig && !*orig) {
        // keep whatever the slot currently points to (possibly another hook)
        *orig = real;
        uint8_t *base = (uint8_t *)mod;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (IMAGE_IMPORT_DESCRIPTOR *imp = (void *)(base + dd.VirtualAddress); dd.VirtualAddress && imp->Name; imp++) {
            if (_stricmp((char *)(base + imp->Name), dll) || !imp->OriginalFirstThunk) continue;
            IMAGE_THUNK_DATA *ft = (void *)(base + imp->FirstThunk), *ot = (void *)(base + imp->OriginalFirstThunk);
            for (; ft->u1.Function; ft++, ot++)
                if (!(ot->u1.Ordinal & IMAGE_ORDINAL_FLAG) &&
                    !strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData))->Name, func))
                    *orig = (void *)ft->u1.Function;
        }
    }
    return iat_hook_module(mod, dll, func, repl, real);
}

int iat_hook_all(const char *dll, const char *func, void *repl, void **orig)
{
    HMODULE hd = GetModuleHandleA(dll);
    void *real = hd ? (void *)GetProcAddress(hd, func) : NULL;
    if (orig && !*orig) *orig = real;
    HMODULE self;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&iat_hook_all, &self);
    char sysdir[MAX_PATH];
    GetSystemWindowsDirectoryA(sysdir, MAX_PATH);
    size_t sl = strlen(sysdir);
    int total = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32 me = {sizeof me};
    if (Module32First(snap, &me)) {
        do {
            if (me.hModule == self) continue;
            if (_strnicmp(me.szExePath, sysdir, sl) == 0) continue;  // leave OS modules alone
            total += iat_hook_module(me.hModule, dll, func, repl, real);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return total;
}

// ---------------------------------------------------------------------------
struct sm_ent { char *k, *v; struct sm_ent *next; };
struct strmap { struct sm_ent *b[4096]; int n; };

static unsigned sm_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

strmap *strmap_new(void) { return calloc(1, sizeof(strmap)); }

void strmap_put(strmap *m, const char *k, const char *v)
{
    unsigned h = sm_hash(k) & 4095;
    for (struct sm_ent *e = m->b[h]; e; e = e->next)
        if (!strcmp(e->k, k)) { free(e->v); e->v = _strdup(v); return; }
    struct sm_ent *e = malloc(sizeof *e);
    e->k = _strdup(k);
    e->v = _strdup(v);
    e->next = m->b[h];
    m->b[h] = e;
    m->n++;
}

const char *strmap_get(strmap *m, const char *k)
{
    if (!m || !k) return NULL;
    for (struct sm_ent *e = m->b[sm_hash(k) & 4095]; e; e = e->next)
        if (!strcmp(e->k, k)) return e->v;
    return NULL;
}

int strmap_count(strmap *m) { return m ? m->n : 0; }

void strmap_each(strmap *m, strmap_cb cb, void *ctx)
{
    if (!m) return;
    for (int i = 0; i < 4096; i++)
        for (struct sm_ent *e = m->b[i]; e; e = e->next) cb(e->k, e->v, ctx);
}

static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *d++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

int strmap_load_tsv(strmap *m, const char *path, int cp)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    char *p = buf;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    int n = 0;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r') p[l - 1] = 0;
        char *tab = strchr(p, '\t');
        if (*p != '#' && tab) {
            *tab = 0;
            unescape(p);
            unescape(tab + 1);
            char *k = utf8_to_cp(p, cp), *v = utf8_to_cp(tab + 1, cp);
            strmap_put(m, k, v);
            free(k);
            free(v);
            n++;
        }
        if (!e) break;
        p = e + 1;
    }
    free(buf);
    return n;
}
