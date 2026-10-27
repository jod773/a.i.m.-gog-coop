// The mod's options, shared by both games' settings menus: each option has a
// label and a description (text keys opt_<id> / opt_<id>_desc, English
// defaults here), a list of values and a getter/setter. The games add their
// own options and show the list in their menus (aim2/lobby.c, aim1/lobby.c).
#include "modopts.h"
#include "display.h"
#include <stdio.h>
#include <string.h>

extern char g_ini[MAX_PATH];

static modopt g_opts[40];
static int g_n;
static const char *(*g_ui)(const char *key);
static int (*g_reset)(void);           // live device Reset (NULL: display changes need a restart)

void modopts_set_ui(const char *(*ui)(const char *key)) { g_ui = ui; }
void modopts_set_reset(int (*reset)(void)) { g_reset = reset; }

const char *modopts_text(const char *key, const char *def)
{
    const char *s = g_ui ? g_ui(key) : NULL;
    return s && s[0] && strcmp(s, key) ? s : def;
}

void modopts_add(const modopt *o)
{
    if (g_n < (int)(sizeof g_opts / sizeof g_opts[0])) g_opts[g_n++] = *o;
}

int modopts_count(void) { return g_n; }
const modopt *modopts_get(int i) { return i >= 0 && i < g_n ? &g_opts[i] : NULL; }

static const char *value_text(const modopt *o, int v)
{
    char key[48];
    if (v < 0 || v >= o->nvals) v = 0;
    snprintf(key, sizeof key, "opt_%s_%d", o->id, v);
    return modopts_text(key, o->vals[v]);
}

const char *modopts_value(int i, int v)
{
    const modopt *o = modopts_get(i);
    return o ? value_text(o, v) : "";
}

// the option's name; options applied at the next start say so
void modopts_label(int i, char *out, int n)
{
    const modopt *o = modopts_get(i);
    if (!o) { out[0] = 0; return; }
    char key[48];
    snprintf(key, sizeof key, "opt_%s", o->id);
    int later = o->restart || (o->reset && !g_reset);
    snprintf(out, n, "%s%s", modopts_text(key, o->label), later ? modopts_text("opt_restart_short", " (restart)") : "");
}

// apply several changed values at once: one device Reset for all display options; if it
// fails they go back to their old values
void modopts_apply(const int *vals)
{
    int old[40], reset = 0;
    for (int i = 0; i < g_n; i++) {
        old[i] = g_opts[i].get();
        if (vals[i] < 0 || vals[i] >= g_opts[i].nvals || vals[i] == old[i]) continue;
        g_opts[i].set(vals[i]);
        mlog("options: %s = %d", g_opts[i].id, vals[i]);
        reset |= g_opts[i].reset;
    }
    if (!reset || !g_reset || g_reset()) return;
    mlog("options: the display settings could not be applied, back to the previous ones");
    for (int i = 0; i < g_n; i++)
        if (g_opts[i].reset && g_opts[i].get() != old[i]) g_opts[i].set(old[i]);
    g_reset();
}

// "Anti-aliasing: 4x"
void modopts_line(int i, char *out, int n)
{
    const modopt *o = modopts_get(i);
    if (!o) { out[0] = 0; return; }
    char key[48];
    snprintf(key, sizeof key, "opt_%s", o->id);
    snprintf(out, n, "%s: %s", modopts_text(key, o->label), value_text(o, o->get()));
}

void modopts_desc(int i, char *out, int n)
{
    const modopt *o = modopts_get(i);
    if (!o) { out[0] = 0; return; }
    char key[48];
    snprintf(key, sizeof key, "opt_%s_desc", o->id);
    const char *d = modopts_text(key, o->desc);
    int later = o->restart || (o->reset && !g_reset);
    snprintf(out, n, "%s%s", d, later ? modopts_text("opt_restart", " (Takes effect the next time the game starts.)") : "");
}

// next value; display options are applied with a device Reset when the game supports it
void modopts_cycle(int i)
{
    const modopt *o = modopts_get(i);
    if (!o || o->nvals < 2) return;
    int old = o->get(), v = (old + 1) % o->nvals;
    o->set(v);
    mlog("options: %s = %d", o->id, v);
    if (o->reset && g_reset && !g_reset()) {
        mlog("options: %s could not be applied, back to %d", o->id, old);
        o->set(old);
        g_reset();
    }
}

// ---- options of the shared modules -------------------------------------------------------------
static int get_aa(void) { int m = display_msaa(); return m == 8 ? 3 : m == 4 ? 2 : m == 2 ? 1 : 0; }
static void set_aa(int v) { display_set_msaa(v ? 1 << v : 0); }
static int get_vsync(void) { return display_vsync() + 1; }          // game's / off / on
static void set_vsync(int v) { display_set_vsync(v - 1); }
static int get_aniso(void) { int a = display_aniso(); return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; }
static void set_aniso(int v) { display_set_aniso(v ? 1 << v : 0); }
static int get_renderer(void) { return GetPrivateProfileIntA("Video", "Renderer", 8, g_ini) == 9; }
static void set_renderer(int v) { WritePrivateProfileStringA("Video", "Renderer", v ? "9" : "8", g_ini); }
static int get_shadows(void) { int s = GetPrivateProfileIntA("Video", "ShadowScale", 1, g_ini); return s >= 4 ? 2 : s >= 2 ? 1 : 0; }
static void set_shadows(int v) { WritePrivateProfileStringA("Video", "ShadowScale", v == 2 ? "4" : v == 1 ? "2" : "1", g_ini); }
#include "d9light.h"
static int get_dynl(void) { return d9light_enabled(); }
static void set_dynl(int v) { d9light_set_enabled(v); }
static const int k_fps[5] = {0, -1, 60, 120, 144};
static int get_fps(void) { int f = display_fps_limit(); for (int i = 0; i < 5; i++) if (k_fps[i] == f) return i; return 0; }
static void set_fps(int v) { display_set_fps_limit(k_fps[v < 0 || v > 4 ? 0 : v]); }
static int get_pad(void) { return gamepad_enabled(); }
static void set_pad(int v) { gamepad_set_enabled(v); }
int mouse_sensitivity(void);
int mouse_smoothing(void);
void mouse_set(int sens, int smooth_ms);
static const int k_msens[6] = {100, 75, 50, 35, 25, 15};
static const int k_msmooth[4] = {0, 15, 35, 70};
static int get_msens(void) { int s = mouse_sensitivity(); for (int i = 0; i < 6; i++) if (s >= k_msens[i]) return i; return 5; }
static void set_msens(int v) { mouse_set(k_msens[v < 0 || v > 5 ? 0 : v], mouse_smoothing()); }
static int get_msmooth(void) { int s = mouse_smoothing(); for (int i = 3; i > 0; i--) if (s >= k_msmooth[i]) return i; return 0; }
static void set_msmooth(int v) { mouse_set(mouse_sensitivity(), k_msmooth[v < 0 || v > 3 ? 0 : v]); }
static int get_focus(void) { return GetPrivateProfileIntA("Audio", "GlobalFocus", 1, g_ini) != 0; }
static void set_focus(int v) { WritePrivateProfileStringA("Audio", "GlobalFocus", v ? "1" : "0", g_ini); }

void modopts_add_common(int shadows)
{
    modopt o[] = {
        {"renderer", "Renderer", "Direct3D 8 is the game's original renderer. Direct3D 9 runs the game on a newer interface, needed for the mod's improved graphics.",
         2, {"Direct3D 8 (original)", "Direct3D 9"}, get_renderer, set_renderer, 1, 0},
        {"shadows", "Shadow resolution", "Sharper shadows: the shadow textures are rendered at 2 or 4 times the original resolution (Direct3D 9 renderer only).",
         3, {"Original", "2x", "4x"}, get_shadows, set_shadows, 1, 0},
        {"dynlight", "Dynamic lighting", "Explosions, missiles and shots light up the ground, buildings and gliders around them (Direct3D 9 renderer only).",
         2, {"Off", "On"}, get_dynl, set_dynl, 0, 0},
        {"aa", "Anti-aliasing", "Smooths the jagged edges of objects (multisampling). Higher values cost more performance.",
         4, {"Off", "2x", "4x", "8x"}, get_aa, set_aa, 0, 1},
        {"vsync", "VSync", "Removes tearing (horizontal lines when turning) in fullscreen; in a window, frames are paced to the monitor's refresh rate.",
         3, {"Game's setting", "Off", "On"}, get_vsync, set_vsync, 0, 1},
        {"fpslimit", "Frame rate limit", "Evenly paced frames at a fixed rate. Uncapped menus run at hundreds of frames per second, which makes some G-SYNC / FreeSync monitors flicker.",
         5, {"Off", "Monitor refresh", "60", "120", "144"}, get_fps, set_fps, 0, 0},
        {"aniso", "Anisotropic filtering", "Keeps ground and wall textures sharp at a distance and at steep angles.",
         5, {"Game's setting", "2x", "4x", "8x", "16x"}, get_aniso, set_aniso, 0, 0},
        {"gamepad", "Gamepad", "XInput gamepads. In flight: left stick thrust and strafe, right stick aim, RB fire, LB second weapon, X rocket, A jump, B / LT brake, RT afterburner, Y target, START menu (buttons: the ini, [Gamepad]). In menus the sticks move the cursor, A clicks.",
         2, {"Off", "On"}, get_pad, set_pad, 0, 0},
        {"msens", "Mouse sensitivity", "In flight, on top of the game's own sensitivity: lower values turn slower, for when the game's lowest setting is still too fast.",
         6, {"100% (game's)", "75%", "50%", "35%", "25%", "15%"}, get_msens, set_msens, 0, 0},
        {"msmooth", "Mouse smoothing", "In flight: spreads each mouse movement over the next frames, so turning and aiming are smooth instead of jumpy. Stronger = smoother, a little more delay.",
         4, {"Off", "Light", "Medium", "Strong"}, get_msmooth, set_msmooth, 0, 0},
        {"focus", "Background sound", "Sounds keep playing while the game window is in the background (also avoids a sound freeze when entering bases).",
         2, {"Off", "On"}, get_focus, set_focus, 1, 0},
    };
    for (size_t i = 0; i < sizeof o / sizeof o[0]; i++) if ((shadows & 1 || i != 1) && (shadows & 2 || i != 2)) modopts_add(&o[i]);
}
