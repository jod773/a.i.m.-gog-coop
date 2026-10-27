// Display mode and "save anywhere" settings for A.I.M. 2: Clan Wars.
#include "aim2.h"
#include "../common/lang.h"
#include "../common/display.h"
#include <stdio.h>
#include <string.h>


static const char *const k_modes[] = {"fullscreen", "windowed", "borderless"};

static void cfg_path(char *out, size_t n)
{
    snprintf(out, n, "%sDATA\\CONFIG\\cfg.ini", g_root);
}

// ---- display mode ------------------------------------------------------------
int aim2_display_mode_ini(void)
{
    char v[32];
    GetPrivateProfileStringA("Video", "DisplayMode", "", v, sizeof v, g_ini);
    for (int i = 0; i < 3; i++)
        if (!_stricmp(v, k_modes[i])) return i;
    if (GetPrivateProfileIntA("Video", "Windowed", 0, g_ini) || GetPrivateProfileIntA("Dev", "Windowed", 0, g_ini))
        return DISPLAY_WINDOWED;
    return DISPLAY_FULLSCREEN;
}

// The game's own windowed mode ([CONFIG] Windowed=1 in cfg.ini) is broken
// (zero-sized back buffer, tiny window), so the game always runs its
// fullscreen setup and the display module converts it at the D3D level.
static UINT (WINAPI *o_GetPrivateProfileIntA)(LPCSTR, LPCSTR, INT, LPCSTR);
static UINT WINAPI h_GetPrivateProfileIntA(LPCSTR s, LPCSTR k, INT d, LPCSTR f)
{
    if (s && k && !_stricmp(s, "CONFIG") && !_stricmp(k, "Windowed")) return 0;
    return o_GetPrivateProfileIntA(s, k, d, f);
}

void aim2_display_install(void)
{
    iat_hook_one(GetModuleHandleA(NULL), "kernel32.dll", "GetPrivateProfileIntA", h_GetPrivateProfileIntA,
                 (void **)&o_GetPrivateProfileIntA);
}

// Switch modes in place, the same way the options dialog applies a new
// resolution: release device resources, Reset, restore.
// Reset the device with the current settings (display mode, anti-aliasing, VSync).
// Returns 0 if the Reset failed (the old settings are then restored by the caller).
static int display_reset(void *dlg, void (*undo)(void))
{
    void *app = *(void **)A_RENDER_APP_PTR;
    void *dev = *(void **)A_DEVICE_PTR;
    if (!app || !dev) return 1;    // applied when the device is created
    typedef void (THISCALL *fn)(void *);
    typedef HRESULT (WINAPI *reset_fn)(void *, void *);
    VFN(app, 0x14, fn)(app);
    HRESULT hr = VFN(dev, 0x38, reset_fn)(dev, (void *)A_PRESENT_PP);
    if (hr < 0) {
        mlog("display: Reset failed (%08lx), restoring the previous settings", hr);
        undo();
        VFN(dev, 0x38, reset_fn)(dev, (void *)A_PRESENT_PP);
    }
    VFN(app, 0x18, fn)(app);
    void *ui = dlg ? *(void **)((uint8_t *)dlg + 0x20c) : NULL;
    if (ui) VFN(ui, 0x14, fn)(ui);
    return hr >= 0;
}

static int g_old_msaa, g_old_vsync;
static void undo_quality(void) { display_set_msaa(g_old_msaa); display_set_vsync(g_old_vsync > 0); }

// anti-aliasing (0/2/4/8) and VSync from the options dialog
void aim2_display_quality(void *dlg, int msaa, int vsync)
{
    g_old_msaa = display_msaa();
    g_old_vsync = display_vsync();
    if (msaa == g_old_msaa && vsync == (g_old_vsync > 0)) return;
    display_set_msaa(msaa);
    display_set_vsync(vsync);
    display_reset(dlg, undo_quality);
}

void aim2_display_apply(void *dlg, int mode)
{
    int old = display_mode();
    if (mode == old || !display_select(mode)) return;
    WritePrivateProfileStringA("Video", "DisplayMode", k_modes[mode], g_ini);
    void *app = *(void **)A_RENDER_APP_PTR;
    void *dev = *(void **)A_DEVICE_PTR;
    if (!app || !dev) return;      // applied when the device is created
    typedef void (THISCALL *fn)(void *);
    typedef HRESULT (WINAPI *reset_fn)(void *, void *);
    VFN(app, 0x14, fn)(app);
    HRESULT hr = VFN(dev, 0x38, reset_fn)(dev, (void *)A_PRESENT_PP);
    if (hr < 0) {
        mlog("display: switching to %s failed (%08lx), staying in %s", k_modes[mode], hr, k_modes[old]);
        display_select(old);
        WritePrivateProfileStringA("Video", "DisplayMode", k_modes[old], g_ini);
        VFN(dev, 0x38, reset_fn)(dev, (void *)A_PRESENT_PP);
    }
    VFN(app, 0x18, fn)(app);
    void *ui = dlg ? *(void **)((uint8_t *)dlg + 0x20c) : NULL;
    if (ui) VFN(ui, 0x14, fn)(ui);
}

// ---- save anywhere -------------------------------------------------------------
// The game only allows saving inside bases unless [CONFIG] OutsideSave=1
// (which also enables quick save/load on F5/F9). Default it to on.
int aim2_save_anywhere(void)
{
    char cfg[MAX_PATH];
    cfg_path(cfg, sizeof cfg);
    return GetPrivateProfileIntA("CONFIG", "OutsideSave", 1, cfg) == 1;
}

void aim2_set_save_anywhere(int on)
{
    char cfg[MAX_PATH];
    cfg_path(cfg, sizeof cfg);
    WritePrivateProfileStringA("CONFIG", "OutsideSave", on ? "1" : "0", cfg);
    *(uint8_t *)A_OUTSIDE_SAVE = (uint8_t)(on != 0);
}

// ---- intro videos ----------------------------------------------------------------
// CMMApp::Init plays publish0/1C/srs/publish1.bik in their own 800x600 Bink
// fullscreen mode before Direct3D starts (switching modes and losing focus).
// Story videos are played elsewhere and are not affected.
int aim2_intro_videos(void)
{
    return GetPrivateProfileIntA("Video", "IntroVideos", 1, g_ini) != 0;
}

void aim2_set_intro_videos(int on)
{
    WritePrivateProfileStringA("Video", "IntroVideos", on ? "1" : "0", g_ini);
}

static void THISCALL skip_intro(void *app, int skippable) { (void)app; (void)skippable; }   // CMMApp::PlayIntro(bool)

// Test harness: [Dev] SkipVideos=1 skips every video (story videos too).
static void THISCALL skip_video(void *player, const char *file, int skippable) { (void)player; (void)file; (void)skippable; }

// Video sound (#17). The game sets up Bink 1.5a with BinkSetSoundSystem(BinkOpenDirectSound, 0).
// The freeze after "decline training" (install.bik: BinkOpen never returned) came from the
// TerminateThread below, not from Bink; these switches and logs stay for tests.
typedef int (WINAPI *bink_setsound_fn)(void *open, uintptr_t param);
static bink_setsound_fn o_bink_setsound;
static void *g_bink_ds, *g_bink_waveout;

// [Dev] VideoSound (test builds, #17): 0 = videos without sound, 1 = the game's DirectSound, 2 = waveOut.
static int g_video_sound = 1;

static int WINAPI bink_setsound(void *open, uintptr_t param)
{
    static int n;
    if (open == g_bink_ds && g_video_sound == 0) {
        if (!n++) mlog("video: Bink sound system not set (videos without sound)");
        return 1;
    }
    if (open == g_bink_ds && g_bink_waveout && g_video_sound == 2) {
        if (!n++) mlog("video: Bink sound through waveOut instead of DirectSound");
        return o_bink_setsound(g_bink_waveout, 0);
    }
    return o_bink_setsound(open, param);
}

// BinkOpen / BinkWait logged: tells a hang while opening from a video whose clock never runs.
typedef void *(WINAPI *bink_open_fn)(const char *, uint32_t);
typedef int (WINAPI *bink_wait_fn)(void *);
typedef void (WINAPI *bink_track_fn)(uint32_t, uint32_t *);
typedef void (WINAPI *bink_onoff_fn)(void *, int);
static bink_open_fn o_bink_open;
static bink_wait_fn o_bink_wait;
static bink_track_fn g_bink_track;
static bink_onoff_fn g_bink_onoff;
static unsigned g_wait_busy, g_frames;

static void *WINAPI bink_open(const char *f, uint32_t flags)
{
    if (g_video_sound == 0 && g_bink_track) {
        static uint32_t none;
        g_bink_track(0, &none);
        flags |= 0x4000;                     // BINKSNDTRACK: open only the tracks set above (none)
    }
    mlog("video: BinkOpen(%s, %08x)", f ? f : "", flags);
    void *b = o_bink_open(f, flags);
    mlog("video: BinkOpen = %p", b);
    if (b && g_video_sound == 0 && g_bink_onoff) g_bink_onoff(b, 0);
    g_wait_busy = g_frames = 0;
    return b;
}

static int WINAPI bink_wait(void *b)
{
    int r = o_bink_wait(b);
    if (r) {
        if (++g_wait_busy == 2000 || g_wait_busy % 50000 == 0)
            mlog("video: BinkWait still waiting (%u calls, %u frames shown)", g_wait_busy, g_frames);
    } else {
        if (!g_frames++) mlog("video: first frame");
        g_wait_busy = 0;
    }
    return r;
}

static void bink_waveout_install(void)
{
    HMODULE bink = GetModuleHandleA("binkw32.dll"), exe = GetModuleHandleA(NULL);
    if (!bink) return;
    g_video_sound = GetPrivateProfileIntA("Dev", "VideoSound", 1, g_ini);
    g_bink_ds = (void *)GetProcAddress(bink, "_BinkOpenDirectSound@4");
    g_bink_waveout = (void *)GetProcAddress(bink, "_BinkOpenWaveOut@4");
    g_bink_track = (bink_track_fn)GetProcAddress(bink, "_BinkSetSoundTrack@8");
    g_bink_onoff = (bink_onoff_fn)GetProcAddress(bink, "_BinkSetSoundOnOff@8");
    if (g_bink_ds && g_bink_waveout &&
        iat_hook_one(exe, "binkw32.dll", "_BinkSetSoundSystem@8", bink_setsound, (void **)&o_bink_setsound) &&
        o_bink_setsound)
        mlog("video: Bink sound mode %d (0 none, 1 DirectSound, 2 waveOut)", g_video_sound);
    else
        mlog("video: Bink sound NOT changed (binkw32 without waveOut, or no import)");
    if (!iat_hook_one(exe, "binkw32.dll", "_BinkOpen@8", bink_open, (void **)&o_bink_open) || !o_bink_open ||
        !iat_hook_one(exe, "binkw32.dll", "_BinkWait@4", bink_wait, (void **)&o_bink_wait) || !o_bink_wait)
        mlog("video: BinkOpen/BinkWait not hooked");
}

// Sound streams (music) are stopped with TerminateThread (#17). The stop function (0x5e40b0) stops the
// DirectSound buffer and kills the stream thread at once: when that thread is inside DirectSound
// (refilling the buffer) at that moment, a lock of the audio stack dies with it and every later
// sound call of the process waits forever - the next video's BinkOpen froze the game. The thread
// ends by itself within 100 ms once the buffer is stopped: wait for that first.
static BOOL (WINAPI *o_TerminateThread)(HANDLE, DWORD);
static LONG g_tt_clean, g_tt_killed;

static BOOL WINAPI h_TerminateThread(HANDLE th, DWORD code)
{
    if (A_STREAM_STOP_TT_RET && (uintptr_t)__builtin_return_address(0) == A_STREAM_STOP_TT_RET &&
        GetThreadId(th) != GetCurrentThreadId()) {
        if (WaitForSingleObject(th, 1000) == WAIT_OBJECT_0) {
            if (InterlockedIncrement(&g_tt_clean) <= 20) mlog("sound: stream thread ended by itself");
            return TRUE;
        }
        InterlockedIncrement(&g_tt_killed);
        mlog("sound: stream thread did not end in 1 s, terminated");
    }
    BOOL r = o_TerminateThread(th, code);
    if (r) WaitForSingleObject(th, 2000);
    return r;
}

void aim2_intro_install(void)
{
    if (A_STREAM_STOP_TT_RET &&
        iat_hook_one(GetModuleHandleA(NULL), "kernel32.dll", "TerminateThread", h_TerminateThread,
                     (void **)&o_TerminateThread) && o_TerminateThread)
        mlog("sound: stream threads end cleanly");
    bink_waveout_install();
    if (VT_BINK_PLAYER && GetPrivateProfileIntA("Dev", "SkipVideos", 0, g_ini)) {
        void *f = (void *)skip_video;
        uint32_t cur = *(uint32_t *)(VT_BINK_PLAYER + VF_BINK_PLAY);
        if (patch_bytes(VT_BINK_PLAYER + VF_BINK_PLAY, &cur, &f, 4)) mlog("all videos skipped (test harness)");
    }
    if (aim2_intro_videos()) return;
    if (patch_call(S_INTRO_VIDEOS, skip_intro)) mlog("intro videos skipped");
}

void aim2_saves_install(void)
{
    // GetPrivateProfileIntA("CONFIG", "OutsideSave", 0 -> 1, cfg)
    uintptr_t s = OUTSIDESAVE_STR;     // push 0; push "OutsideSave"
    uint8_t expect[7] = {0x6a, 0x00, 0x68, (uint8_t)s, (uint8_t)(s >> 8), (uint8_t)(s >> 16), (uint8_t)(s >> 24)};
    uint8_t data[sizeof expect];
    memcpy(data, expect, sizeof data);
    data[1] = 1;                // push 0 -> push 1
    if (patch_bytes(S_OUTSIDESAVE_DEFAULT, expect, data, sizeof data)) mlog("saves: OutsideSave defaults to 1");
}

// ---- shadow distance ---------------------------------------------------------------------------------
// Gliders cast their (rendered) shadow only up to detail level 3; the comparison reads our
// limit instead of the game's 3.05 constant.
static float g_shadow_lod = 3.05f;
static const float k_shadow_lod[3] = {3.05f, 5.05f, 100.0f};
int aim2_shadow_distance(void)
{
    int v = GetPrivateProfileIntA("Video", "ShadowDistance", 0, g_ini);
    return v < 0 || v > 2 ? 0 : v;
}
void aim2_set_shadow_distance(int v)
{
    char b[8];
    snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA("Video", "ShadowDistance", b, g_ini);
    g_shadow_lod = k_shadow_lod[v < 0 || v > 2 ? 0 : v];
}
void aim2_shadows_install(void)
{
    if (!S_SHADOW_LOD_CMP) return;
    g_shadow_lod = k_shadow_lod[aim2_shadow_distance()];
    uintptr_t a = A_SHADOW_LOD_MAX, n = (uintptr_t)&g_shadow_lod;
    uint8_t expect[7] = {0x0f, 0x2f, 0x05, (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16), (uint8_t)(a >> 24)};
    uint8_t data[7] = {0x0f, 0x2f, 0x05, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    if (patch_bytes(S_SHADOW_LOD_CMP, expect, data, 7)) mlog("shadows: distance limit LOD %.2f", g_shadow_lod);
}

// ---- the mod settings page (modopts) -------------------------------------------------------------
#include "../common/modopts.h"
void *g_reset_ui;                     // the panel the settings page belongs to (its owner is redrawn)

static int device_reset(void)
{
    void *app = *(void **)A_RENDER_APP_PTR;
    void *dev = *(void **)A_DEVICE_PTR;
    if (!app || !dev) return 1;
    typedef void (THISCALL *fn)(void *);
    typedef HRESULT (WINAPI *reset_fn)(void *, void *);
    VFN(app, 0x14, fn)(app);
    HRESULT hr = VFN(dev, 0x38, reset_fn)(dev, (void *)A_PRESENT_PP);
    VFN(app, 0x18, fn)(app);
    // (the main menu's relayout, as the options dialog does it, crashes with the settings page open)
    mlog("options: device Reset %08lx", hr);
    return hr >= 0;
}

static int get_lang(void) { return g_lang; }
static void set_lang(int v) { if (v != g_lang && v < g_nlangs) { lang_select(v); aim2_apply_language(); } }
static int get_mode(void) { return display_mode(); }
static void set_mode(int v)
{
    if (display_select(v)) WritePrivateProfileStringA("Video", "DisplayMode", k_modes[v], g_ini);
}
static int get_names(void) { return names_enabled(); }
static void set_names(int v) { names_set_enabled(v); }
static int get_econ(void) { return economy_auto_supply(); }
static void set_econ(int v) { economy_set_auto_supply(v); }
static int get_debug(void) { return GetPrivateProfileIntA("Dev", "DebugMenu", 1, g_ini) != 0; }
static void set_debug(int v) { WritePrivateProfileStringA("Dev", "DebugMenu", v ? "1" : "0", g_ini); }

void aim2_modopts_install(void)
{
    modopts_set_ui(aim2_ui);
    if (FEAT_DISPLAY_APPLY) modopts_set_reset(device_reset);
    modopt lang = {"language", "Language", "Language of the game's texts, voices and videos (where translated).",
                   g_nlangs < 8 ? g_nlangs : 8, {0}, get_lang, set_lang, 0, 0};
    for (int i = 0; i < lang.nvals; i++) lang.vals[i] = g_langs[i].name;
    modopt game[] = {
        {"display", "Display mode", "Fullscreen, a normal window, or a borderless window covering the screen.",
         3, {"Fullscreen", "Windowed", "Borderless"}, get_mode, set_mode, 0, 1},
        {"saves", "Save anywhere", "Saving outside bases, quick save (F5) and quick load (F9).",
         2, {"Off", "On"}, aim2_save_anywhere, aim2_set_save_anywhere, 0, 0},
        {"intro", "Intro videos", "The publisher logos and intro played when the game starts.",
         2, {"Off", "On"}, aim2_intro_videos, aim2_set_intro_videos, 1, 0},
        {"econ", "Smart supply", "Every game hour, buildings get the goods they need from the surplus of the buildings that produce them, so production, clan income and glider purchases don't collapse.",
         2, {"Off", "On"}, get_econ, set_econ, 0, 0},
        {"names", "Show player names", "Other players' names above their gliders in co-op games.",
         2, {"Off", "On"}, get_names, set_names, 0, 0},
        {"shadowdist", "Shadow distance", "How far away gliders still cast shadows.",
         3, {"Original", "Far", "Maximum"}, aim2_shadow_distance, aim2_set_shadow_distance, 0, 0},
        {"debug", "Debug menu", "Tilde menu > 4. DEBUG: teleport to buildings and players, enter buildings, unlock passages.",
         2, {"Off", "On"}, get_debug, set_debug, 1, 0},
    };
    if (g_nlangs > 1) modopts_add(&lang);
    modopts_add(&game[0]);
    modopts_add_common(3);
    for (size_t i = 1; i < sizeof game / sizeof game[0]; i++)
        if ((i != 3 && i != 4) || FEAT_ECONOMY) if (i != 5 || S_SHADOW_LOD_CMP) modopts_add(&game[i]);
}