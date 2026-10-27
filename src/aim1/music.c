// Music streaming thread (A.I.M.): stop it cleanly before its buffers are freed.
//
// The game streams its OGG music (CSoundMusicOGG, vtable 0x52f48c) on a worker thread,
// CSoundMusic::NotifyHandler 0x518b70: each time DirectSound signals that half of the sound
// buffer has played, it calls LoadBlock (0x515990, vf 0x24), which decodes the next block into
// the heap buffers at +0x158 / +0x430 under the music lock (+0x134).
//
// Changing the track (CSoundMusic::Play 0x518cb0, every music change: menu, location, fight,
// death) calls Unload (0x5158a0, vf 0x20), which ends that thread with TerminateThread
// (CSoundMusic::Unload 0x518620), then PreLoad (0x5156d0, vf 0x1c) frees and reallocates those
// buffers WITHOUT the lock. TerminateThread only starts the termination and returns at once,
// and it can kill the thread anywhere, also inside DirectSound or the heap. A thread that is
// still running then decodes music into freed memory, or a heap lock dies with it: the heap is
// damaged and the game crashes later somewhere else (CObjectList::Run / Pack,
// CSoundMusicOGG::LoadBlock, ntdll heap functions with MSVCR70 in the stack). The faster the PC,
// the more often the window is hit.
//
// Fix: before the game's Unload runs, stop the DirectSound buffer (under the music lock) and wait,
// without the lock, until the thread has seen that the music no longer plays and has returned
// by itself; its TerminateThread then finds a finished thread. If it does not end in time, the
// game's TerminateThread still runs, but it is made synchronous (waits for the thread to be gone).
#include "engine.h"
#include <string.h>

#define VT_MUSIC_OGG         0x0052f48c   // CSoundMusicOGG vtable
#define VF_MUSIC_UNLOAD      0x20         // thiscall(music, char terminate_thread)
#define A_MUSIC_OGG_UNLOAD   0x005158a0   // CSoundMusicOGG::Unload
#define A_MUSIC_OGG_DTOR_CALL 0x00515b5d  // CSoundMusicOGG destructor: call Unload(1)
#define MUSIC_THREAD         0x08         // HANDLE of the NotifyHandler thread
#define MUSIC_DSBUFFER       0x10         // IDirectSoundBuffer*
#define MUSIC_LOCK           0x134        // CRITICAL_SECTION (LoadBlock / Unload)
#define DSB_STOP             18           // IDirectSoundBuffer::Stop

typedef void (THISCALL *music_unload_fn)(uint8_t *, int);
typedef HRESULT (WINAPI *dsb_stop_fn)(void *);

static BOOL (WINAPI *o_TerminateThread)(HANDLE, DWORD);
static LONG g_waits, g_timeouts;

// End the streaming thread of `m` (not the calling thread) and wait until it is gone.
static void music_stop_thread(uint8_t *m)
{
    HANDLE th = NULL;
    CRITICAL_SECTION *lock = (CRITICAL_SECTION *)(m + MUSIC_LOCK);
    EnterCriticalSection(lock);          // (LoadBlock and the thread's own Unload hold it)
    HANDLE cur = *(HANDLE *)(m + MUSIC_THREAD);
    if (cur && GetThreadId(cur) != GetCurrentThreadId()) {
        // our own handle: the thread's own Unload may close the game's while we wait
        if (!DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &th, SYNCHRONIZE, FALSE, 0)) th = NULL;
        void *buf = *(void **)(m + MUSIC_DSBUFFER);
        if (th && buf) ((dsb_stop_fn)(*(void ***)buf)[DSB_STOP])(buf);   // "not playing" ends its loop
    }
    LeaveCriticalSection(lock);
    if (!th) return;
    // the thread checks the buffer at least every 100 ms (it may first finish a LoadBlock)
    if (WaitForSingleObject(th, 2000) == WAIT_OBJECT_0) InterlockedIncrement(&g_waits);
    else if (InterlockedIncrement(&g_timeouts) <= 5) mlog("music: streaming thread did not end in 2 s");
    CloseHandle(th);
}

static void THISCALL music_unload(uint8_t *m, int terminate)
{
    if (m) music_stop_thread(m);
    CALL(A_MUSIC_OGG_UNLOAD, music_unload_fn)(m, terminate);
}

// The game's only TerminateThread call (CSoundMusic::Unload): wait until the thread is really gone
// before the buffers it uses are freed (TerminateThread itself returns at once).
static BOOL WINAPI h_TerminateThread(HANDLE th, DWORD code)
{
    if (WaitForSingleObject(th, 0) == WAIT_OBJECT_0) return TRUE;    // already ended by itself
    BOOL r = o_TerminateThread(th, code);
    if (r) WaitForSingleObject(th, 2000);
    return r;
}

void aim1_music_install(void)
{
    int ok = 1;
    // every Unload goes through the vtable (Play, NotifyHandler) except the destructor's direct call
    uint32_t was = A_MUSIC_OGG_UNLOAD;
    void *f = (void *)music_unload;
    ok &= patch_bytes(VT_MUSIC_OGG + VF_MUSIC_UNLOAD, &was, &f, 4);
    uint8_t *call = (uint8_t *)A_MUSIC_OGG_DTOR_CALL;
    if (call[0] == 0xe8 && A_MUSIC_OGG_DTOR_CALL + 5 + *(int32_t *)(call + 1) == A_MUSIC_OGG_UNLOAD)
        patch_call(A_MUSIC_OGG_DTOR_CALL, (void *)music_unload);
    else ok = 0;
    ok &= iat_hook_one(GetModuleHandleA(NULL), "kernel32.dll", "TerminateThread", h_TerminateThread,
                       (void **)&o_TerminateThread) && o_TerminateThread;
    mlog("music: streaming thread %s", ok ? "stopped safely on track changes" : "fix NOT installed (unexpected code)");
}

// test: "musicstat" - how often a track change waited for the streaming thread
void aim1_music_stat(void)
{
    mlog("music: %ld clean thread stops, %ld timeouts", g_waits, g_timeouts);
}
