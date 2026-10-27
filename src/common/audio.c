// Audio: sound buffers with global focus.
//
// DirectSound mutes (and on focus changes stops/restarts) the buffers of a
// window that doesn't have the focus. Starting music from the game thread
// while that happens can deadlock inside DirectSound (seen when entering bases
// with the game window in the background). With DSBCAPS_GLOBALFOCUS the
// buffers keep playing regardless of the focus, and that path isn't taken.
#include "util.h"
#include <string.h>

#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBCAPS_GLOBALFOCUS   0x00008000

typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; void *lpwfxFormat; } dsbdesc;
typedef HRESULT (WINAPI *create_fn)(const GUID *, void **, void *);
typedef HRESULT (WINAPI *createbuf_fn)(void *, dsbdesc *, void **, void *);
static create_fn o_create;
static createbuf_fn o_createbuf;

static HRESULT WINAPI h_createbuf(void *ds, dsbdesc *d, void **out, void *outer)
{
    if (d && d->dwSize >= 8 && !(d->dwFlags & DSBCAPS_PRIMARYBUFFER)) d->dwFlags |= DSBCAPS_GLOBALFOCUS;
    return o_createbuf(ds, d, out, outer);
}

static HRESULT WINAPI h_create(const GUID *g, void **out, void *outer)
{
    HRESULT hr = o_create(g, out, outer);
    if (hr == 0 && out && *out && !o_createbuf) {
        void **vt = *(void ***)*out;
        o_createbuf = (createbuf_fn)vt[3];                 // IDirectSound::CreateSoundBuffer
        void *f = (void *)h_createbuf;
        uint32_t cur = (uint32_t)(uintptr_t)vt[3];
        if (patch_bytes((uintptr_t)&vt[3], &cur, &f, 4)) mlog("audio: sound buffers use global focus");
    }
    return hr;
}

// The game imports DirectSoundCreate by ordinal: find its import slot by value.
void audio_install(void)
{
    HMODULE ds = LoadLibraryA("dsound.dll");
    void *real = ds ? (void *)GetProcAddress(ds, "DirectSoundCreate") : NULL;
    if (!real) return;
    uint8_t *exe = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)exe + d->Name, "dsound.dll")) continue;
        for (void **slot = (void **)(exe + d->FirstThunk); *slot; slot++) {
            if (*slot != real) continue;
            o_create = (create_fn)real;
            void *f = (void *)h_create;
            uint32_t cur = (uint32_t)(uintptr_t)real;
            patch_bytes((uintptr_t)slot, &cur, &f, 4);
            return;
        }
    }
}
