// A.I.M. co-op lobby: the main menu's Network button (relabelled MULTIPLAYER)
// opens a list of co-op choices built on the game's own list dialog (the
// network-location flavour of CLoadSaveMenu): host a new game, host from a
// save, join a game found on the LAN, or the original arena network game.
#include "coop.h"
#include "../common/net.h"
#include "../common/modopts.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <shlwapi.h>

typedef void *(__cdecl *new_fn)(size_t);
typedef void (__cdecl *delete_fn)(void *);
typedef void (THISCALL *ctor_fn)(void *);
typedef int (THISCALL *run_menu_fn)(void *, void *, int);
typedef void (THISCALL *cmd_fn)(void *, int);
typedef void (THISCALL *getdata_fn)(void *, int *);
typedef char (THISCALL *search_fn)(void *);

// Each entry's key is "<image>.<action>": the dialog shows Data\Images\<image>.tga
// for the selected entry (the arena pictures), the action is ours.
typedef struct { char key[64]; char caption[128]; char addr[64]; } entry;

static entry g_ent[24];
static int g_nent;
static int g_waiting;               // waiting for the host's world
static int g_world_ready;
static int g_editing;               // the player is typing their name
static int g_settings;              // the dialog is the settings list
static int g_cycle = -1;            // settings: option clicked, changed by the next frame
static void *g_vt[14];
static void *o_run_controls, *o_list_cmd;

static void build_settings(void);

static void add_entry(const char *key, const char *caption, const char *addr)
{
    if (g_nent >= 24) return;
    entry *e = &g_ent[g_nent++];
    lstrcpynA(e->key, key, sizeof e->key);
    lstrcpynA(e->caption, caption, sizeof e->caption);
    lstrcpynA(e->addr, addr ? addr : "", sizeof e->addr);
}

static void THISCALL get_data(uint8_t *menu, int *out)
{
    (void)menu;
    out[0] = g_nent;
    out[1] = 0;
    if (!g_nent) return;
    uint8_t *recs = CALL(A_NEW, new_fn)(g_nent * LISTREC_SIZE);
    memset(recs, 0, g_nent * LISTREC_SIZE);
    for (int i = 0; i < g_nent; i++) {
        uint8_t *r = recs + (g_nent - 1 - i) * LISTREC_SIZE;    // the dialog lists them last first
        lstrcpynA((char *)r, g_ent[i].key, 0x84);
        lstrcpynA((char *)r + 0x84, g_ent[i].caption, 0x80);
    }
    out[1] = (int)recs;
}

static void refresh_list(uint8_t *menu)
{
    uint8_t *html = *(uint8_t **)(menu + 0x204);
    if (html && *(void **)(html + 0x64)) CALL(0x0042f5a0, ctor_fn)(*(void **)(html + 0x64));
    *(int *)(menu + 0x200) = -1;
    menu[MENU_FILE] = 0;
    CALL(0x00429330, ctor_fn)(menu);
}

// ProcessCommand (vtable slot 0x30). 0x63: a list entry was clicked (its key is copied to
// MENU_FILE); 0x100: OK. The network dialog's OK checks its connection state, so ours
// closes the dialog with the selected entry itself. In the settings list a click changes
// the option at once.
static void THISCALL list_cmd(uint8_t *menu, int cmd)
{
    if (cmd == 0x100) {
        if (menu[MENU_FILE] || g_settings) {         // the settings are already applied: OK closes
            *(int *)(menu + MENU_RESULT) = menu[MENU_FILE] ? 6 : 1;
            menu[MENU_DONE] = 1;
        }
        return;
    }
    ((cmd_fn)o_list_cmd)(menu, cmd);
    if (cmd != 0x63 || !g_settings || !menu[MENU_FILE]) return;
    const char *act = strchr((char *)menu + MENU_FILE, '.');
    if (!act) return;
    if (!strncmp(act + 1, "opt", 3)) {
        g_cycle = atoi(act + 4);
    } else {
        *(int *)(menu + MENU_RESULT) = 6;
        menu[MENU_DONE] = 1;
    }
}

// per-frame (vtable slot 0x1c): progress while waiting for the world
static void THISCALL run_controls(uint8_t *menu)
{
    ((ctor_fn)o_run_controls)(menu);
    if (g_settings && g_cycle >= 0) {
        int i = g_cycle;
        g_cycle = -1;
        modopts_cycle(i);
        build_settings();
        refresh_list(menu);
        return;
    }
    if (g_editing) {
        char name[32], b[128];
        static char shown_name[40] = "\x01";
        int r = textinput_poll(name, sizeof name);
        if (r) {
            if (r > 0) coop_set_name(name);
            g_editing = 0;
            lstrcpyA(shown_name, "\x01");
            *(int *)(menu + MENU_RESULT) = 1;
            menu[MENU_DONE] = 1;
        } else if (strcmp(name, shown_name)) {
            lstrcpyA(shown_name, name);
            snprintf(b, sizeof b, aim1_ui("name_typing"), name);
            g_nent = 0;
            add_entry("net_s1.typing", b, NULL);
            refresh_list(menu);
        }
        return;
    }
    if (!g_waiting) return;
    int pct;
    static int shown = -2;
    if (g_world_ready) {
        lstrcpyA((char *)menu + MENU_FILE, "ready.go");
        *(int *)(menu + MENU_RESULT) = 6;
        menu[MENU_DONE] = 1;
        return;
    }
    if (net_mode() != NET_CLIENT) {                     // lost the connection
        *(int *)(menu + MENU_RESULT) = 1;
        menu[MENU_DONE] = 1;
        return;
    }
    int now = coop_world_receiving(&pct) ? pct : -1;
    if (now != shown) {
        shown = now;
        char b[128];
        snprintf(b, sizeof b, aim1_ui("receiving"), now < 0 ? 0 : now);
        g_nent = 0;
        add_entry("net_s1.wait", b, NULL);
        refresh_list(menu);
    }
}

// Runs the list dialog; returns the chosen entry or NULL.
static entry *run_lobby(void)
{
    uint8_t *menu = CALL(A_NEW, new_fn)(0x400);
    memset(menu, 0, 0x400);
    CALL(A_LISTMENU_CTOR, ctor_fn)(menu);
    *(void ***)menu = g_vt;
    int r = CALL(A_RUN_MENU, run_menu_fn)(G_APP, menu, 1);
    char key[64];
    lstrcpynA(key, (char *)menu + MENU_FILE, sizeof key);
    *(void **)menu = (void *)VT_LISTMENU_NET;
    CALL(A_LISTMENU_DTOR, ctor_fn)(menu);
    CALL(A_DELETE, delete_fn)(menu);
    mlog("lobby: result %d '%s'", r, key);
    if (r != 6) return NULL;
    for (int i = 0; i < g_nent; i++)
        if (!strcmp(g_ent[i].key, key)) return &g_ent[i];
    return NULL;
}

static void build_main_list(void)
{
    net_found found[8];
    int n = net_discover(found, 8, 700);
    g_nent = 0;
    char nm[96];
    snprintf(nm, sizeof nm, aim1_ui("name"), coop_my_name());
    add_entry("net_s1.name", nm, NULL);
    add_entry("netarena.hostnew", aim1_ui("host_new"), NULL);
    add_entry("Limit.hostload", aim1_ui("host_load"), NULL);
    for (int i = 0; i < n && i < 8; i++) {
        char cap[128], key[64];
        snprintf(cap, sizeof cap, aim1_ui("join"), found[i].name, found[i].players);
        snprintf(key, sizeof key, "net_s2.join%d", i);
        add_entry(key, cap, found[i].addr);
    }
    if (!n) add_entry("net_s1.none", aim1_ui("none_found"), NULL);
    add_entry("Seep.refresh", aim1_ui("refresh"), NULL);
    add_entry("Spinescent.classic", aim1_ui("classic"), NULL);
}

int lobby_waiting(void) { return g_waiting; }
void lobby_world_ready(void) { g_world_ready = 1; }

static int g_user_cmd = 1;          // ProcessCommand calls coming from the player

// The start menu's Network button (the call to the arena server search).
static char THISCALL mp_button(uint8_t *smenu)
{
    typedef void (THISCALL *pcmd_fn)(void *, int);
    for (;;) {
        build_main_list();
        entry *e = run_lobby();
        if (!e) return 0;
        const char *act = strchr(e->key, '.') + 1;
        if (!strcmp(act, "classic")) return CALL(A_NET_SEARCH, search_fn)(smenu);
        if (!strcmp(act, "name")) {
            g_nent = 0;
            add_entry("net_s1.typing", "", NULL);
            textinput_start(coop_my_name(), 24);
            g_editing = 1;
            run_lobby();
            textinput_stop();             // the dialog may have been closed some other way
            g_editing = 0;
            continue;
        }
        if (!strcmp(act, "hostnew")) {
            coop_set_host_intent(1);
            g_user_cmd = 0;
            CALL(A_START_MENU_CMD, pcmd_fn)(smenu, 3);      // difficulty choice, then the game starts
            g_user_cmd = 1;
            return 0;
        }
        if (!strcmp(act, "hostload")) {
            coop_set_host_intent(1);
            g_user_cmd = 0;
            CALL(A_START_MENU_CMD, pcmd_fn)(smenu, 9);      // the load dialog
            g_user_cmd = 1;
            if (*(int *)(smenu + MENU_RESULT) == 7) return 0;
            coop_set_host_intent(0);
            continue;
        }
        if (!strncmp(act, "join", 4)) {
            char b[128];
            if (!coop_join(e->addr) && (Sleep(500), !coop_join(e->addr))) {
                snprintf(b, sizeof b, aim1_ui("connect_failed"), e->addr);
                mlog("lobby: %s", b);
                continue;
            }
            g_world_ready = 0;
            g_waiting = 1;
            g_nent = 0;
            snprintf(b, sizeof b, aim1_ui("connecting"), e->addr);
            add_entry("net_s1.wait", b, NULL);
            entry *w = run_lobby();
            g_waiting = 0;
            if (w || g_world_ready) {
                if (g_world_ready) {
                    lstrcpyA((char *)smenu + MENU_FILE, COOP_SYNC_SAVE);
                    *(int *)(smenu + MENU_RESULT) = 7;
                    return 0;
                }
            }
            coop_leave();                                    // cancelled
            continue;
        }
        // "none" / "refresh": search again
    }
}

// The player backing out of the difficulty choice (or picking another
// command) cancels a pending "host a new game".
static void (THISCALL *o_start_cmd)(void *, int);
static void THISCALL start_cmd(void *menu, int cmd)
{
    if (g_user_cmd && cmd != 0x65 && cmd != 0x66 && cmd != 0x67) coop_set_host_intent(0);
    o_start_cmd(menu, cmd);
}

// ---- settings --------------------------------------------------------------------------------
// The start menu's Options button runs SetOptions.exe (the game quits for it). The mod's
// settings list comes first; its first entry still opens the original tool.
// A click on an option changes it in place (list_cmd, run_controls); OK or a click on the
// first entry opens the original tool. Returns 1 if the player chose the original tool.
// rough width of a text in the list's font (pixels at 1920x1080)
static int text_width(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        char c = *s;
        w += strchr(".,:;'!iIlj| ", c) ? 3 : strchr("mwMW", c) ? 10 : (c >= 'A' && c <= 'Z') ? 8 : 6;
    }
    return w;
}

// "Label: value" -> "Label ........ value": only an entry's text takes clicks, so the dots
// make every row clickable across the list
static void leader_line(char *s, int n)
{
    char *colon = strstr(s, ": ");
    if (!colon) return;
    char label[96], value[96], out[128];
    lstrcpynA(label, s, (int)(colon - s) + 1 > (int)sizeof label ? (int)sizeof label : (int)(colon - s) + 1);
    lstrcpynA(value, colon + 2, sizeof value);
    int w = text_width(label) + text_width(value) + 6, k = lstrlenA(label);
    lstrcpynA(out, label, sizeof out);
    out[k++] = ' ';
    while (w < 205 && k < (int)sizeof out - (int)strlen(value) - 3) { out[k++] = '.'; w += 5; }
    out[k++] = ' ';
    out[k] = 0;
    lstrcpynA(out + k, value, (int)sizeof out - k);
    lstrcpynA(s, out, n);
}

static void build_settings(void)
{
    static unsigned gen;                // the list ignores a click on the link clicked last: new names each time
    gen++;
    g_nent = 0;
    add_entry("Limit.orig", modopts_text("opt_orig", "Game options (original tool)..."), NULL);
    for (int i = 0; i < modopts_count(); i++) {
        char cap[128], key[32];
        modopts_line(i, cap, sizeof cap);
        leader_line(cap, sizeof cap);
        snprintf(key, sizeof key, "nopicture.opt%d_%u", i, gen);      // no arena picture beside the options
        add_entry(key, cap, NULL);
    }
}

static int settings_menu(void)
{
    g_settings = 1;
    g_cycle = -1;
    build_settings();
    entry *e = run_lobby();
    g_settings = 0;
    return e && !strcmp(strchr(e->key, '.') + 1, "orig");
}

// The Options button minimizes the game, then runs SetOptions.exe and waits for it. The
// minimize waits until the player actually chooses the original tool.
static HWND g_opt_wnd;
static BOOL WINAPI opt_showwindow(HWND w, int cmd) { g_opt_wnd = w; (void)cmd; return TRUE; }

static BOOL (WINAPI *o_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static BOOL WINAPI h_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD fl,
                                   LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    if ((cmd && StrStrIA(cmd, "SetOptions")) || (app && StrStrIA(app, "SetOptions"))) {
        if (!settings_menu()) {
            SetLastError(ERROR_CANCELLED);
            return FALSE;
        }
        if (g_opt_wnd) ShowWindow(g_opt_wnd, SW_MINIMIZE);
    }
    return o_CreateProcessA(app, cmd, pa, ta, inh, fl, env, dir, si, pi);
}

#define S_OPT_MINIMIZE 0x004272b8   // call [ShowWindow] (SW_MINIMIZE) before SetOptions.exe
#define S_CAPTION_ROW  0x0042ef28   // mov esi, [esi+0x1ec]: the list dialog's type picks its title

// The title is a row of SUF_CAPTIONS (LOAD GAME, SAVE GAME, NET GAME); the mod's copy of
// the texture adds MULTIPLAYER and SETTINGS (tools/aim1_captions.py).
static int __cdecl caption_row(uint8_t *menu)
{
    if (*(void ***)menu == g_vt) return g_settings ? 4 : 3;
    return *(int *)(menu + 0x1ec);
}

static void caption_install(void)
{
    // push ecx; push edx; push eax; push esi; call caption_row; add esp, 4; mov esi, eax;
    // pop eax; pop edx; pop ecx; ret
    static const uint8_t stub[] = {0x51, 0x52, 0x50, 0x56, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0x89, 0xc6, 0x58, 0x5a, 0x59, 0xc3};
    uint8_t *code = VirtualAlloc(NULL, sizeof stub, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return;
    memcpy(code, stub, sizeof stub);
    int32_t rel = (int32_t)((uintptr_t)caption_row - (uintptr_t)(code + 9));
    memcpy(code + 5, &rel, 4);
    static const uint8_t expect[6] = {0x8b, 0xb6, 0xec, 0x01, 0x00, 0x00};
    uint8_t call[6] = {0xe8, 0, 0, 0, 0, 0x90};
    rel = (int32_t)((uintptr_t)code - (S_CAPTION_ROW + 5));
    memcpy(call + 1, &rel, 4);
    patch_bytes(S_CAPTION_ROW, expect, call, 6);
}

void aim1_lobby_install(void)
{
    iat_hook_one(GetModuleHandleA(NULL), "kernel32.dll", "CreateProcessA", h_CreateProcessA, (void **)&o_CreateProcessA);
    static const uint8_t sw_call[6] = {0xff, 0x15, 0xf0, 0x12, 0x52, 0x00};
    uint8_t sw_new[6] = {0xe8, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)opt_showwindow - (S_OPT_MINIMIZE + 5));
    memcpy(sw_new + 1, &rel, 4);
    patch_bytes(S_OPT_MINIMIZE, sw_call, sw_new, 6);
    caption_install();
    memcpy(g_vt, (void *)VT_LISTMENU_NET, sizeof g_vt);
    g_vt[0x34 / 4] = (void *)get_data;
    o_run_controls = g_vt[0x1c / 4];
    g_vt[0x1c / 4] = (void *)run_controls;
    o_list_cmd = g_vt[0x30 / 4];
    g_vt[0x30 / 4] = (void *)list_cmd;
    patch_call(A_NET_SEARCH_CALL, mp_button);
    static const uint8_t je[] = {0x74, 0x17}, nop2[] = {0x90, 0x90};
    patch_bytes(0x0042ea41, je, nop2, 2);       // open the lobby even with [NET] Enable=0
    // CMMStartMenu::ProcessCommand (vtable slot 0x30)
    o_start_cmd = *(void **)(VT_START_MENU + 0x30);
    void *p = (void *)start_cmd;
    patch_bytes(VT_START_MENU + 0x30, NULL, &p, 4);
    mlog("lobby: installed");
}
