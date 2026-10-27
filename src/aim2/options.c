// The options dialog as the MOD SETTINGS page: the game's own settings stay as they are; the mod's
// options (common/modopts) get one combo box each on the same dialog with its tabs hidden.
#include "aim2.h"
#include "../common/display.h"
#include "../common/modopts.h"
#include <string.h>

typedef void *(__cdecl *alloc_fn)(size_t);
typedef void (THISCALL *ctor_fn)(void *);
typedef void (THISCALL *ctor_cstr_fn)(void *, const char *);
typedef void (THISCALL *dlg_str_fn)(void *, const char *);
typedef void (THISCALL *dlg_fn)(void *);
typedef void (THISCALL *create_fn)(void *, grect, gstr);
typedef void (THISCALL *combo_add_fn)(void *, gstr);
typedef void (THISCALL *combo_setsel_fn)(void *, int);
typedef int (THISCALL *combo_getsel_fn)(void *);
typedef void (THISCALL *ctl_fn)(void *);
typedef void (THISCALL *dlg_addctl_fn)(void *, void *);

#define CALL(addr, T) ((T)(addr))

#define o_load    ((dlg_str_fn)A_OPT_LOADPARAMS)
#define o_save    ((dlg_str_fn)A_OPT_SAVEPARAMS)
#define o_hide    ((dlg_fn)A_OPT_HIDEALL)

// one set per dialog instance (main menu and in-game options)
typedef struct {
    void *dlg;
    int mod;                    // the dialog is the mod settings page
    void *opt[32];              // its combo boxes, one per option (modopts)
} extra;
static extra g_map[4];
static int g_open_mod;          // the next options dialog opens as the mod settings page

static extra *extras(void *dlg, int create)
{
    for (int i = 0; i < 4; i++)
        if (g_map[i].dlg == dlg) return &g_map[i];
    if (!create) return NULL;
    for (int i = 0; i < 4; i++)
        if (!g_map[i].dlg) { g_map[i].dlg = dlg; return &g_map[i]; }
    return NULL;
}

static gstr make_gstr(const char *s)
{
    gstr g;
    CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&g, s);
    return g;    // ownership passes to the callee that receives it by value
}

static void *create_combo(void *dlg, grect r, const char *label, const char *const *items, int n)
{
    uint8_t *c = CALL(A_ALLOC, alloc_fn)(SZ_COMBO);
    CALL(A_COMBO_CTOR, ctor_fn)(c);
    VFN(c, 0xbc, create_fn)(c, r, make_gstr(label));
    for (int i = 0; i < n; i++)
        VFN(c, 0xb0, combo_add_fn)(c, make_gstr(items[i]));
    VFN(dlg, 0xa8, dlg_addctl_fn)(dlg, c);
    return c;
}

static void show(void *c, int on)
{
    if (c) VFN(c, on ? 0x1c : 0x20, ctl_fn)(c);
}

static void set_ui_texture(void *dlg)
{
    // Init assigns the UI texture to all controls before LoadParams; repeat it so the new
    // controls get their frames and buttons too.
    typedef void *(__cdecl *texkey_fn)(void *, const char *);
    typedef void (THISCALL *dlg_settex_fn)(void *, void *);
    char key[40];
    void *k = CALL(A_TEXKEY, texkey_fn)(key, "TEX_INT_UI1");
    VFN(dlg, 0x18, dlg_settex_fn)(dlg, k);
}

// ---- the mod settings page --------------------------------------------------------------
// The same dialog without the game's tabs and controls: one combo box per option of the
// mod (common/modopts), applied with the dialog's USE button, discarded by CLOSE.
#define OPT_TABS 0x330          // the five tab buttons (Video, Sound, Mouse, Buttons, Game)

void aim2_options_open_mod(void) { g_open_mod = 1; }

static void hide_game_page(void *dlg, int unpress)
{
    o_hide(dlg);
    for (int i = 0; i < 5; i++) {
        uint8_t *tab = ((uint8_t **)((uint8_t *)dlg + OPT_TABS))[i];
        if (!tab) continue;
        if (unpress) tab[0x230] = 0;
        show(tab, 0);
        ((int *)(tab + 0x30))[1] = ((int *)(tab + 0x30))[3] = -4000;    // (they still draw: out of sight)
    }
}

// The tabs and DEFAULT (the game's default.ini) do nothing on the mod page.
static void (THISCALL *o_dlg_cmd)(void *, unsigned);
static void THISCALL dlg_cmd(void *dlg, unsigned cmd)
{
    extra *e = extras(dlg, 0);
    if (e && e->mod && (cmd == 0xf79 || cmd == 0xf7a || cmd == 0xf7b || cmd == 0xf80 || cmd == 0xf81 || cmd == 0xf003)) return;
    o_dlg_cmd(dlg, cmd);
}

// Showing the dialog shows all its controls, then the pressed tab's page: on the mod page,
// the game's controls and tabs are hidden again afterwards.
static void (THISCALL *o_dlg_show)(void *);
static void THISCALL dlg_show(void *dlg)
{
    o_dlg_show(dlg);
    extra *e = extras(dlg, 0);
    if (e && e->mod) hide_game_page(dlg, 0);
}

static void create_mod_page(void *dlg, extra *e)
{
    hide_game_page(dlg, 1);             // no tab pressed: no tab page is shown with the dialog
    int n = modopts_count();
    if (n > 32) n = 32;
    // rows fill the page between the top and the DEFAULT / USE / CLOSE buttons (the Game tab's
    // last row ends at 340): 24 apart while they fit, closer for more options
    const int top = 34, bottom = 342;
    int step = (bottom - top) / (n ? n : 1), y = top;
    if (step > 24) step = 24, y = top + 12;
    if (step < 16) step = 16;
    for (int i = 0; i < n; i++, y += step) {
        const modopt *o = modopts_get(i);
        const char *vals[8];
        for (int v = 0; v < o->nvals && v < 8; v++) vals[v] = modopts_value(i, v);
        char label[96];
        modopts_label(i, label, sizeof label);
        e->opt[i] = create_combo(dlg, (grect){420, y, 590, y + step - 1}, label, vals, o->nvals < 8 ? o->nvals : 8);
        VFN(e->opt[i], 0xa0, combo_setsel_fn)(e->opt[i], o->get());
    }
    set_ui_texture(dlg);
    mlog("options: mod settings page (%d options)", n);
}

static void apply_mod_page(void *dlg, extra *e)
{
    int vals[40], n = modopts_count();
    for (int i = 0; i < 40; i++) vals[i] = -1;
    for (int i = 0; i < n && i < 32; i++) if (e->opt[i]) vals[i] = VFN(e->opt[i], 0xa4, combo_getsel_fn)(e->opt[i]);
    modopts_apply(vals);
    for (int i = 0; i < n && i < 32; i++)          // what actually applies (a failed display change reverts)
        if (e->opt[i]) VFN(e->opt[i], 0xa0, combo_setsel_fn)(e->opt[i], modopts_get(i)->get());
}

// call site in COptionDialog::Init: LoadParams("cfg.ini")
// Init runs every time the dialog opens and rebuilds (and frees) all child
// controls, so fresh controls are created each time.
static void THISCALL init_loadparams(void *dlg, const char *ini)
{
    extra *e = extras(dlg, 1);
    if (e) {
        memset((uint8_t *)e + sizeof e->dlg, 0, sizeof *e - sizeof e->dlg);
        e->mod = g_open_mod;
    }
    g_open_mod = 0;
    if (e && e->mod) {
        o_load(dlg, ini);
        create_mod_page(dlg, e);
        return;
    }
    o_load(dlg, ini);                   // the game's own settings page, unchanged
}

// "Apply"
static void THISCALL save_params(void *dlg, const char *ini)
{
    extra *e = extras(dlg, 0);
    if (e && e->mod) {              // the game's own settings are untouched on this page
        apply_mod_page(dlg, e);
        return;
    }
    o_save(dlg, ini);
}

void aim2_options_install(void)
{
    patch_call(S_OPT_INIT_LOADPARAMS, init_loadparams);   // Init -> LoadParams
    patch_call(S_OPT_APPLY_SAVE, save_params);       // command 0xf07e (Apply)
    if (S_OPT_SHOW_SLOT) {
        o_dlg_show = *(void **)S_OPT_SHOW_SLOT;
        void *f = (void *)dlg_show;
        patch_bytes(S_OPT_SHOW_SLOT, NULL, &f, 4);
        o_dlg_cmd = *(void **)(S_OPT_SHOW_SLOT + 0x48);   // OnCommand
        f = (void *)dlg_cmd;
        patch_bytes(S_OPT_SHOW_SLOT + 0x48, NULL, &f, 4);
    }
    mlog("options dialog hooks installed");
}
