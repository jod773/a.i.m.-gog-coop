// Language overrides for files stored inside .pak archives.
//
// CFileVFS::Open (0x560ab0) looks a name up in every loaded pak and only
// falls back to a loose file when no pak contains it, so packed resources
// can't be replaced by loose files. We check for Data\Lang\<code>\<name>
// first and, if present, open it the same way the original opens loose files.
#include "aim2.h"
#include "../common/lang.h"
#include <stdio.h>

typedef char (THISCALL *vfs_open_fn)(void *self, const char *name);
typedef FILE *(__cdecl *fopen_fn)(const char *, const char *);
static vfs_open_fn o_vfs_open;
static fopen_fn game_fopen;

static int g_trace;

static char THISCALL vfs_open(void *self, const char *name)
{
    char red[MAX_PATH];
    if (g_trace > 0) { g_trace--; mlog("vfs_open: %s", name); }
    if (game_fopen && lang_redirect(name, red, sizeof red)) {
        FILE *f = game_fopen(red, "rb");
        if (f) {
            *(int *)((char *)self + 0x14) = -1;     // "not from a pak"
            *(FILE **)((char *)self + 4) = f;
            return 1;
        }
    }
    return o_vfs_open(self, name);
}

// CPakManager::OpenFile (0x424d00), used by the texture/model loaders:
// fills a file object { +4 pak entry, +8 FILE*, +0xc ok, +0xd from-pak }.
typedef char (THISCALL *pak_open_fn)(void *self, const char *name, void *file);
static pak_open_fn o_pak_open;

static char THISCALL pak_open(void *self, const char *name, void *file)
{
    char red[MAX_PATH];
    if (g_trace > 0) { g_trace--; mlog("pak_open: %s", name); }
    if (game_fopen && lang_redirect(name, red, sizeof red)) {
        FILE *f = game_fopen(red, "rb");
        if (f) {
            *((char *)file + 0xd) = 0;
            *(FILE **)((char *)file + 8) = f;
            *((char *)file + 0xc) = 1;
            return 1;
        }
    }
    return o_pak_open(self, name, file);
}

void aim2_vfs_install(void)
{
    // fopen of the C runtime the game uses, so the FILE* is compatible with
    // its later fread/fclose calls
    g_trace = GetPrivateProfileIntA("Dev", "TraceVfs", 0, g_ini);
    HMODULE crt = GetModuleHandleA(CRT_DLL);
    game_fopen = crt ? (fopen_fn)GetProcAddress(crt, "fopen") : NULL;
    static const uint8_t prologue[] = {VFS_OPEN_PRO};
    o_vfs_open = (vfs_open_fn)hook_jmp(A_VFS_OPEN, prologue, sizeof prologue, vfs_open);
    static const uint8_t prologue2[] = {PAK_OPEN_PRO};
    o_pak_open = (pak_open_fn)hook_jmp(A_PAK_OPEN, prologue2, sizeof prologue2, pak_open);
    mlog("vfs override hooks %s/%s", o_vfs_open ? "ok" : "FAILED", o_pak_open ? "ok" : "FAILED");
}
