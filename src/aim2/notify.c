// Co-op notices on the HUD and player status (flying / in a base / in the
// main menu), so everybody knows where the others are.
#include "engine.h"
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CALL(addr, T) ((T)(addr))
#define G_MESSAGER    (*(void **)A_MESSAGER_PTR)   // CMessager (HUD messages)
#define G_TEXTDB      (*(void **)A_TEXTDB_PTR)

enum { MSG_STATUS = 44 };
enum { ST_FLYING, ST_BASE, ST_MENU };

#pragma pack(push, 1)
typedef struct { uint8_t status; char where[32]; } status_msg;
#pragma pack(pop)

typedef void (THISCALL *ctor_cstr_fn)(void *, const char *);
typedef void (THISCALL *gstr_fn)(void *);

static char g_queue[8][160];
static int g_nq;
static int g_my_status = -1;
static char g_my_where[32];

// localized display text of a text-database key (falls back to the key)
static void key_text(const char *key, char *out, int n)
{
    lstrcpynA(out, key, n);
    if (!G_TEXTDB || !key[0]) return;
    name32 k;
    memset(&k, 0, sizeof k);
    lstrcpynA(k.s, key, 32);
    gstr g;
    CALL(A_GSTR_CTOR, gstr_fn)(&g);
    typedef void (THISCALL *gettext_fn)(void *, name32, gstr *);
    VFN(G_TEXTDB, 0x20, gettext_fn)(G_TEXTDB, k, &g);
    if (g.buf && g.buf[0]) lstrcpynA(out, g.buf, n);
    CALL(A_GSTR_DTOR, gstr_fn)(&g);
}

static void show(const char *text)
{
    gstr g;
    CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&g, text);
    typedef void (THISCALL *add_fn)(void *, gstr *, int, const char *);
    CALL(A_ADD_MESSAGE, add_fn)(G_MESSAGER, &g, 1, NULL);
    CALL(A_GSTR_DTOR, gstr_fn)(&g);
}

// shown at once in flight, otherwise when back in flight
void coop_notify(const char *text)
{
    mlog("notice: %s", text);
    if (coop_in_flight() && !g_coop_menu && G_MESSAGER) { show(text); return; }
    if (g_nq < 8) lstrcpynA(g_queue[g_nq++], text, sizeof g_queue[0]);
}

static void send_status(int st, const char *where)
{
    if (net_mode() == NET_OFF) return;
    if (st == g_my_status && !strcmp(where ? where : "", g_my_where)) return;
    g_my_status = st;
    lstrcpynA(g_my_where, where ? where : "", sizeof g_my_where);
    status_msg m;
    memset(&m, 0, sizeof m);
    m.status = (uint8_t)st;
    lstrcpynA(m.where, g_my_where, 32);
    net_send(NET_ALL, MSG_STATUS, &m, sizeof m, 1);
}

void notify_frame(void)
{
    if (net_mode() != NET_OFF && coop_in_flight()) send_status(ST_FLYING, NULL);
    if (g_nq && coop_in_flight() && G_MESSAGER) {
        for (int i = 0; i < g_nq; i++) show(g_queue[i]);
        g_nq = 0;
    }
}

void notify_session_started(void) { g_my_status = -1; }

int notify_handle(int type, int from, const uint8_t *data, int len, const char *name)
{
    if (type != MSG_STATUS) return 0;
    if (len < (int)sizeof(status_msg)) return 1;
    const status_msg *m = (const status_msg *)data;
    char where[32], place[96], buf[200];
    memcpy(where, m->where, 31);
    where[31] = 0;
    if (m->status == ST_BASE) {
        key_text(where, place, sizeof place);
        snprintf(buf, sizeof buf, aim2_ui("mp_entered"), name, place);
        coop_notify(buf);
    } else if (m->status == ST_MENU) {
        snprintf(buf, sizeof buf, aim2_ui("mp_in_menu"), name);
        coop_notify(buf);
    }
    (void)from;
    return 1;
}

// ---- hooks ----------------------------------------------------------------------------------
typedef void (THISCALL *building_fn)(void *, void *, int, int);
typedef void (THISCALL *menu_fn)(void *);
static building_fn o_building;
static menu_fn o_mainmenu;

static void THISCALL h_building(void *app, void *name, int a, int b)
{
    char key[33];
    memcpy(key, name, 32);
    key[32] = 0;
    send_status(ST_BASE, key);
    o_building(app, name, a, b);
}

static void THISCALL h_mainmenu(void *app)
{
    send_status(ST_MENU, NULL);
    o_mainmenu(app);
}

void notify_install(void)
{
    static const uint8_t pro5[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
    static const uint8_t pro_menu[] = {MAINMENU_LOOP_PRO};
    o_building = (building_fn)hook_jmp(A_RUN_BUILDING_MENU, pro5, 5, h_building);   // CMMApp::RunBuildingMenu
    o_mainmenu = (menu_fn)hook_jmp(A_MAINMENU_LOOP, pro_menu, sizeof pro_menu, h_mainmenu);       // main menu loop
}
