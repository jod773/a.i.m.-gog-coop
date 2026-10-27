// Main menu "Multiplayer" item and the co-op lobby panel.
//
// The lobby is the Mods dialog (list + description + one button) with its
// own list entries and command handler: host a new game, host from a save,
// join a game found on the LAN.
#include "engine.h"
#include "coop.h"
#include "../common/net.h"
#include "../common/modopts.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef void *(__cdecl *alloc_fn)(size_t);
typedef void (THISCALL *ctor_fn)(void *);
typedef void (THISCALL *ctor_cstr_fn)(void *, const char *);
typedef void (THISCALL *btn_create_fn)(void *, grect, gstr, int, int);
typedef void (THISCALL *btn_setcmd_fn)(void *, int);
typedef void (THISCALL *addctl_fn)(void *, void *);
typedef void (THISCALL *oncmd_fn)(void *, int, int);
typedef void (THISCALL *proc_fn)(void *);
typedef void (THISCALL *post_fn)(void *, int);
typedef void (THISCALL *loadinfo_fn)(void *, void *);
typedef void (THISCALL *list_push_fn)(void *, void *);
typedef void (THISCALL *list_add_fn)(void *, gstr);
typedef void *(__cdecl *panel_get_fn)(void *);

#define CALL(addr, T) ((T)(addr))

#define SZ_MENU_BTN     0x210
#define SZ_MODREC       0xb0c
#define CMD_MP          0xf90
#define CMD_MODSET      0xf9a         // Mod settings (ours)
#define CMD_MODS        0xf68
#define CMD_MODS_LOAD   0xf82
#define CMD_SETTINGS    0xf67

void aim2_options_open_mod(void);

// ---- main menu button --------------------------------------------------------------
static void *g_menu_panel;
static int g_building_main;

void coop_menu_shown(void);

static void *__cdecl get_menu_panel(void *arg)
{
    g_menu_panel =CALL(A_PANEL_GET, panel_get_fn)(arg);
    return g_menu_panel;
}

// Rows of the main menu: New game, Load, Multiplayer, Mods, Settings, Mod settings, Credits, Exit.
static int row_top(int row) { return 0x77 + row * 34; }

static gstr make_gstr(const char *s)
{
    gstr g;
    CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&g, s);
    return g;
}

static void add_menu_button(grect r, int row, const char *label, int cmd)
{
    uint8_t *b = CALL(A_ALLOC, alloc_fn)(SZ_MENU_BTN);
    CALL(A_MENU_BTN_CTOR, ctor_fn)(b);
    grect mr = {r.l, row_top(row), r.r, row_top(row) + 0x20};
    CALL(A_BTN_CREATE, btn_create_fn)(b, mr, make_gstr(label), 0, cmd);
    CALL(A_BTN_SETCMD, btn_setcmd_fn)(b, cmd);
    if (g_menu_panel) VFN(g_menu_panel, 0xa8, addctl_fn)(g_menu_panel, b);
}

static void THISCALL menu_btn_create(void *btn, grect r, gstr text, int a, int cmd)
{
    int row = -1;
    switch (cmd) {
    case 0xf65: row = 0; g_building_main = 1; coop_menu_shown(); break;   // new game: the main menu (the in-game ESC menu shares the panel)
    case 0xf66: row = 1; break;
    case 0xf68: row = 3; break;
    case 0xf67: row = 4; break;
    case 0xf69: row = 6; break;
    case 0xf6a: row = 7; break;                         // exit (shared with the in-game menu)
    }
    if (g_building_main && row >= 0) {
        r.t = row_top(row);
        r.b = r.t + 0x20;
    }
    CALL(A_BTN_CREATE, btn_create_fn)(btn, r, text, a, cmd);
    if (g_building_main && cmd == 0xf6a) {
        g_building_main = 0;
        add_menu_button(r, 2, aim2_ui("multiplayer"), CMD_MP);
        add_menu_button(r, 5, aim2_ui("mod_settings"), CMD_MODSET);
    }
}

// Menu button labels are atlas sprites chosen by command; ours are added to TEX_INT_UI1 by
// tools/cw_mp_atlas.py at these origins: plain, selected, hover.
static const int k_mp_spr[3][2] = {{450, 746}, {362, 555}, {540, 555}};
static const int k_modset_spr[3][2] = {{444, 0}, {286, 98}, {466, 98}};

static void THISCALL menu_btn_render(uint8_t *btn)
{
    int cmd = *(int *)(btn + 0x208);
    if (cmd != CMD_MP && cmd != CMD_MODSET) {
        CALL(A_MENU_BTN_RENDER, proc_fn)(btn);
        return;
    }
    const int (*spr)[2] = cmd == CMD_MP ? k_mp_spr : k_modset_spr;
    int s = *(int *)(btn + 0x20c) | *(int *)(btn + 0x1fc) ? 1 : *(int *)(btn + 0x44) ? 2 : 0;
    int x = spr[s][0], y = spr[s][1];
    int rect[4] = {x, y, x + 0x400, y + 0x400};
    void *r = *(void **)(*(uint8_t **)(btn + 8) + 4);
    typedef void (THISCALL *alpha_fn)(void *, float);
    typedef void (THISCALL *draw_fn)(void *, void *, void *, int *, int);
    VFN(r, 0x2c, alpha_fn)(r, *(int *)(btn + 0xd8) ? 1.0f : 0.7f);
    VFN(r, 0x24, draw_fn)(r, btn + 0x100, btn + 0x30, rect, 0x13);
}

// Panel buttons (Load/Save/Delete...) also draw atlas sprites chosen by an id
// at +0x20c. The atlas still has the old network dialog's buttons (unused by
// the game): Start search, Stop search, Join, Create game, Stop game, each in
// four states (disabled, hover, normal, pressed) at x 859, y 48 + 26 * n.
enum { SPR_SEARCH = 0xf90, SPR_STOP_SEARCH, SPR_JOIN, SPR_CREATE, SPR_STOP_GAME, SPR_LAST };

static void THISCALL panel_btn_render(uint8_t *btn)
{
    int id = *(int *)(btn + 0x20c);
    if (id < SPR_SEARCH || id >= SPR_LAST) {
        CALL(A_PANEL_BTN_RENDER, proc_fn)(btn);
        return;
    }
    if (!btn[0x228]) return;
    void *r = *(void **)(*(uint8_t **)(btn + 8) + 4);
    typedef void (THISCALL *int_fn)(void *, int);
    VFN(r, 0x38, int_fn)(r, *(int *)(btn + 0x208));
    int enabled = *(int *)(btn + 0xd8) != 0;
    int state = !enabled ? 0 : *(int *)(btn + 0x1fc) ? 3 : *(int *)(btn + 0x44) ? 1 : 2;
    int y = 48 + 26 * (state * 5 + (id - SPR_SEARCH));
    int rect[4] = {859, y, 859 + 0x400, y + 0x400};
    typedef void (THISCALL *alpha_fn)(void *, float);
    typedef void (THISCALL *draw_fn)(void *, void *, void *, int *, int);
    VFN(r, 0x2c, alpha_fn)(r, enabled ? 1.0f : 0.5f);
    VFN(r, 0x24, draw_fn)(r, btn + 0x100, btn + 0x30, rect, 0x13);
}

// ---- lobby -----------------------------------------------------------------------------
enum { IT_HOST_NEW, IT_HOST_SAVE, IT_JOIN, IT_REFRESH, IT_NAME };
typedef struct { int kind; char arg[MAX_PATH]; char title[64]; int clan; } item;

static uint8_t *g_lobby;              // the panel (a converted CModsDialog)
static void *g_lobby_vt[64];
static item g_items[24];
static int g_nitems;
static char g_status[512];            // replaces the description while connecting
static int g_editing;                 // the player is typing their name

static uint8_t *lobby_list(void) { return *(uint8_t **)(g_lobby + 0x26c); }
static void *lobby_owner(void) { return *(void **)(g_lobby + 0x20c); }

static char g_descs[24][512];

static void list_add(const char *title, const char *desc)
{
    uint8_t *list = lobby_list();
    uint8_t *rec = CALL(A_ALLOC, alloc_fn)(SZ_MODREC);
    CALL(A_MODREC_CTOR, ctor_fn)(rec);
    lstrcpynA((char *)rec, title, 0x400);
    lstrcpynA((char *)rec + 0x400, desc, 0x400);
    CALL(A_LIST_PUSH, list_push_fn)(list + 0x290, &rec);
    VFN(list, 0xdc, list_add_fn)(list, make_gstr(title));
}

// The Mods list fills itself with the mod files after the dialog opens;
// put our entries back whenever its contents differ.
static void keep_entries(void)
{
    uint8_t *list = lobby_list();
    if (*(int *)(list + 0x29c) == g_nitems) return;
    CALL(A_LIST_CLEAR, proc_fn)(list);
    for (int i = 0; i < g_nitems; i++) list_add(g_items[i].title, g_descs[i]);
}

static void add_item(int kind, const char *title, const char *desc, const char *arg)
{
    if (g_nitems >= 24) return;
    lstrcpynA(g_descs[g_nitems], desc, sizeof g_descs[0]);
    item *it = &g_items[g_nitems++];
    it->kind = kind;
    it->clan = 0;
    lstrcpynA(it->title, title, sizeof it->title);
    lstrcpynA(it->arg, arg ? arg : "", sizeof it->arg);
    uint8_t *list = lobby_list();
    uint8_t *rec = CALL(A_ALLOC, alloc_fn)(SZ_MODREC);
    CALL(A_MODREC_CTOR, ctor_fn)(rec);
    lstrcpynA((char *)rec, title, 0x400);
    lstrcpynA((char *)rec + 0x400, desc, 0x400);
    CALL(A_LIST_PUSH, list_push_fn)(list + 0x290, &rec);
    VFN(list, 0xdc, list_add_fn)(list, make_gstr(title));
}

static uint8_t *selected_rec(void)
{
    uint8_t *list = lobby_list();
    int sel = *(int *)(list + 0x244);
    if (sel < 0 || sel >= g_nitems) return NULL;
    return (*(uint8_t ***)(list + 0x298))[sel];
}

typedef struct { char file[64]; FILETIME t; } savefile;

static int cmp_save(const void *a, const void *b)
{
    return CompareFileTime(&((const savefile *)b)->t, &((const savefile *)a)->t);
}

static void fill(void)
{
    CALL(A_LIST_CLEAR, proc_fn)(lobby_list());
    g_nitems = 0;
    char buf[512], buf2[512];
    snprintf(buf, sizeof buf, aim2_ui("mp_name"), coop_my_name());
    add_item(IT_NAME, buf, aim2_ui("mp_name_desc"), NULL);
    add_item(IT_HOST_NEW, aim2_ui("mp_host_new"), aim2_ui("mp_host_new_desc"), NULL);
    // LAN games
    net_found f[8];
    int n = net_discover(f, 8, 400);
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, aim2_ui("mp_join"), f[i].name, f[i].players);
        snprintf(buf2, sizeof buf2, aim2_ui("mp_join_desc"), f[i].name, f[i].addr);
        add_item(IT_JOIN, buf, buf2, f[i].addr);
        snprintf(buf, sizeof buf, aim2_ui("mp_join_own"), f[i].name);
        snprintf(buf2, sizeof buf2, aim2_ui("mp_join_own_desc"), f[i].name, f[i].addr);
        add_item(IT_JOIN, buf, buf2, f[i].addr);
        g_items[g_nitems - 1].clan = 1;
    }
    // saves, newest first
    savefile sv[64];
    int ns = 0;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%sSAVES\\*.sav", g_root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (ns < 64 && _stricmp(fd.cFileName, COOP_SYNC_SAVE)) {
                lstrcpynA(sv[ns].file, fd.cFileName, 64);
                sv[ns].t = fd.ftLastWriteTime;
                ns++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(sv, ns, sizeof *sv, cmp_save);
    for (int i = 0; i < ns && i < 10; i++) {
        SYSTEMTIME st, lt;
        FileTimeToSystemTime(&sv[i].t, &st);
        SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
        char when[64], name[64];
        snprintf(when, sizeof when, "%02d:%02d %02d.%02d.%04d", lt.wHour, lt.wMinute, lt.wDay, lt.wMonth, lt.wYear);
        lstrcpynA(name, sv[i].file, sizeof name);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        snprintf(buf, sizeof buf, aim2_ui("mp_host_save"), name);
        snprintf(buf2, sizeof buf2, aim2_ui("mp_host_save_desc"), when);
        add_item(IT_HOST_SAVE, buf, buf2, sv[i].file);
    }
    add_item(IT_REFRESH, aim2_ui("mp_refresh"), aim2_ui("mp_refresh_desc"), NULL);
    g_status[0] = 0;
}

// called by the co-op layer (also when the lobby is gone)
void lobby_set_status(const char *text)
{
    lstrcpynA(g_status, text, sizeof g_status);
}

// start a loaded game from the main menu (owner = CMainUI)
static int menu_load(const char *file, const char *title)
{
    if (!g_lobby) return 0;
    struct { char title[64]; char file[256]; } info;
    memset(&info, 0, sizeof info);
    lstrcpynA(info.title, title, 64);
    lstrcpynA(info.file, file, 256);
    void *owner = lobby_owner();
    VFN(owner, 0x18, loadinfo_fn)(owner, &info);
    VFN(owner, 0x3c, post_fn)(owner, 0x3eb);   // main menu result: load
    return 1;
}

static DWORD g_lobby_tick;            // last time the lobby panel was alive

int lobby_load_world(const char *file)
{
    if (!g_lobby || GetTickCount() - g_lobby_tick > 1000) return 0;
    mlog("lobby: loading %s", file);
    int ok = menu_load(file, "Co-op");
    g_lobby = NULL;
    return ok;
}

static void THISCALL lobby_oncmd(void *self, int cmd, int param)
{
    if (cmd != CMD_MODS_LOAD) {
        CALL(A_MODS_ONCMD, oncmd_fn)(self, cmd, param);
        return;
    }
    uint8_t *list = lobby_list();
    int sel = *(int *)(list + 0x244);
    if (sel < 0 || sel >= g_nitems) return;
    item *it = &g_items[sel];
    char buf[256];
    switch (it->kind) {
    case IT_REFRESH:
        fill();
        break;
    case IT_NAME:
        textinput_start(coop_my_name(), 24);
        g_editing = 1;
        break;
    case IT_HOST_NEW:
        if (coop_host()) {
            VFN(lobby_owner(), 0x3c, post_fn)(lobby_owner(), 0x3ed);
            g_lobby = NULL;                 // the menu closes
        }
        break;
    case IT_HOST_SAVE:
        if (coop_host()) {
            menu_load(it->arg, it->title);
            g_lobby = NULL;
        }
        break;
    case IT_JOIN:
        lobby_set_status(aim2_ui("mp_connecting"));
        coop_set_clan(it->clan);
        if (coop_join(it->arg)) {
            lobby_set_status(aim2_ui("mp_waiting"));
        } else {
            snprintf(buf, sizeof buf, aim2_ui("mp_failed"), it->arg);
            lobby_set_status(buf);
        }
        break;
    }
}

static void THISCALL lobby_proc(void *self)
{
    g_lobby_tick = GetTickCount();
    keep_entries();
    // the action button shows what the selected entry does
    uint8_t *btn = *(uint8_t **)(g_lobby + 0x268);
    int sel = *(int *)(lobby_list() + 0x244);
    if (btn) {
        int spr = SPR_CREATE;
        if (sel >= 0 && sel < g_nitems) {
            if (g_items[sel].kind == IT_JOIN) spr = SPR_JOIN;
            else if (g_items[sel].kind == IT_REFRESH) spr = SPR_SEARCH;
        }
        *(int *)(btn + 0x20c) = spr;
        *(int *)(btn + 0xd8) = sel >= 0 && net_mode() == NET_OFF && !g_editing;
    }
    if (g_editing) {
        char name[32], b[256];
        int r = textinput_poll(name, sizeof name);
        if (r) {
            g_editing = 0;
            if (r > 0) coop_set_name(name);
            fill();
        } else {
            snprintf(b, sizeof b, aim2_ui("mp_name_typing"), name);
            lobby_set_status(b);
        }
    }
    if (g_status[0]) {
        uint8_t *rec = selected_rec();
        if (rec) lstrcpynA((char *)rec + 0x400, g_status, 0x400);
    }
    CALL(A_MODS_PROC, proc_fn)(self);
    coop_poll();                          // the frame hook does not run in menus
}

static void open_lobby(void *mainui)
{
    // The lobby is created exactly like the Mods dialog, then converted.
    CALL(A_MAINUI_ONCMD, oncmd_fn)(mainui, CMD_MODS, 0);
    uint8_t *panel = ((uint8_t **)mainui)[0x73];
    if (!panel) return;
    if (!g_lobby_vt[0]) {
        memcpy(g_lobby_vt, (void *)VT_MODS, sizeof g_lobby_vt);
        g_lobby_vt[0x64 / 4] = (void *)lobby_oncmd;
        g_lobby_vt[0x68 / 4] = (void *)lobby_proc;
    }
    *(void ***)panel = g_lobby_vt;
    g_lobby = panel;
    fill();
    mlog("lobby: opened (%d entries)", g_nitems);
}

static void THISCALL mainui_oncmd(void *self, int cmd, int param)
{
    if (cmd == CMD_MP) { open_lobby(self); return; }
    if (cmd == CMD_MODSET) {            // the options dialog as the mod settings page
        aim2_options_open_mod();
        cmd = CMD_SETTINGS;
    }
    // any other panel replaces (and frees) the lobby
    if (cmd >= 0xf65 && cmd <= 0xf6e && cmd != 0xf6a) g_lobby = NULL;
    CALL(A_MAINUI_ONCMD, oncmd_fn)(self, cmd, param);
}

void lobby_menu_closed(void) { g_lobby = NULL; }

void lobby_idle(void) {}

void aim2_lobby_install(void)
{
    patch_call(S_MAINMENU_PANEL, get_menu_panel);
    const uintptr_t sites[] = {S_MENU_BTN_SITES};
    for (int i = 0; i < 6; i++) patch_call(sites[i], menu_btn_create);
    const uintptr_t slots[] = {S_MAINUI_ONCMD_SLOTS};
    for (int i = 0; i < 2; i++) {
        void *fn = (void *)mainui_oncmd;
        patch_bytes(slots[i], NULL, &fn, 4);
    }
    void *rf = (void *)menu_btn_render;
    patch_bytes(VT_MENU_BTN + 0x24, NULL, &rf, 4);
    rf = (void *)panel_btn_render;
    patch_bytes(VT_PANEL_BTN + 0x24, NULL, &rf, 4);
    mlog("lobby: installed");
}
