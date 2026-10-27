// Co-op (A.I.M. 2 / Clan Wars): the game's adapter for the shared session core
// (common/coop_core.c) - puppet gliders, pilot data, world saves - plus test commands.
#include "engine.h"
#include "../common/net.h"
#include "../common/lang.h"
#include "coop.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef void *(__cdecl *alloc_fn)(size_t);
typedef void (THISCALL *ctor_fn)(void *);
typedef void (THISCALL *stream_alloc_fn)(gstream *, int);
typedef void (THISCALL *stream_io_fn)(void *, gstream *);
typedef void *(THISCALL *glider_info_fn)(void *, const void *);
typedef void *(THISCALL *create_glider_fn)(void *, name32, float *, const char *);
typedef void (THISCALL *assign_fn)(void *, void *, int);
typedef void (THISCALL *render_fn)(void *, int);
typedef void (THISCALL *setpos_fn)(void *, float, float, float);

#define CALL(addr, T) ((T)(addr))

static void (__cdecl *crt_free)(void *);
static int g_color_hits;               // test: how often the radar used the player colour
typedef void (THISCALL *setcolor_fn)(void *, uint32_t, int);
static setcolor_fn o_setcolor;         // test: colour trace
static volatile DWORD g_trace_until;
static void THISCALL h_setcolor(void *r, uint32_t color, int a);
typedef void (THISCALL *sprite_fn)(void *, int, int, int, int, int, int);
static sprite_fn o_sprite;
static volatile DWORD g_strace_until;
static void THISCALL h_sprite(void *r, int a, int b, int c2, int d, int e, int f);

// ---- engine helpers ------------------------------------------------------------------------
uint8_t *coop_mechanoid_save(uint8_t *src, int *len)
{
    gstream s = {0};
    CALL(A_STREAM_ALLOC, stream_alloc_fn)(&s, 5000);
    CALL(A_MECHANOID_SAVE, stream_io_fn)(src, &s);
    uint8_t *out = malloc(s.pos);
    memcpy(out, s.buf, s.pos);
    *len = s.pos;
    if (!s.owned && s.buf && crt_free) crt_free(s.buf);
    return out;
}

uint8_t *coop_mechanoid_load(const uint8_t *blob, int len)
{
    gstream s = {1, len, 0, (uint8_t *)blob};    // owned=1: the stream must not free it
    uint8_t *m = CALL(A_ALLOC, alloc_fn)(SZ_MECHANOID);
    CALL(A_MECHANOID_CTOR, ctor_fn)(m);
    CALL(A_MECHANOID_LOAD, stream_io_fn)(m, &s);
    return m;
}

// A group of its own for a puppet pilot: never registered in the world's
// group list (so its AI never runs), but it answers relation queries (radar
// colours, NPC targeting) for the clan `org`.
static uint8_t *make_group(const char *name, const char *org)
{
    typedef void (THISCALL *vf0)(void *);
    typedef void (THISCALL *vf1)(void *, int);
    typedef void *(THISCALL *find_org_fn)(void *, const char *);
    uint8_t *g = CALL(A_ALLOC, alloc_fn)(SZ_GROUP);
    CALL(A_GROUP_CTOR, ctor_fn)(g);
    VFN(g, VF_GROUP_INIT, vf0)(g);
    lstrcpynA((char *)g + GRP_NAME, name, 32);
    memset(g + GRP_ORGNAME, 0, 32);
    lstrcpynA((char *)g + GRP_ORGNAME, org, 32);
    uint8_t *o = CALL(A_WORLD_FIND_ORG, find_org_fn)(G_WORLD, org);
    *(uint8_t **)(g + GRP_ORG) = o;
    VFN(g, VF_GROUP_SETAI, vf1)(g, 3);
    uint8_t *ai = *(uint8_t **)(g + GRP_AI);
    if (ai && o) VFN(ai, VF_AI_SETORG, vf1)(ai, *(int *)(*(uint8_t **)(o + ORG_E0) + 8));
    return g;
}

// Creates a glider flown by a new pilot with the given mechanoid, at matrix m.
// org: the pilot's clan (NULL = the local player's).
uint8_t *coop_spawn_glider(uint8_t *mechanoid, const char *name, const char *org, const float m[16])
{
    uint8_t *mech = CALL(A_ALLOC, alloc_fn)(SZ_MECH);
    CALL(A_MECH_CTOR, ctor_fn)(mech);
    // Same clan as the local player for now.
    uint32_t vt = *(uint32_t *)mech;
    memcpy(mech + 4, G_PLAYER_MECH + 4, SZ_MECH - 4);
    *(uint32_t *)mech = vt;
    memset(mech + MECH_NAME, 0, 32);
    lstrcpynA((char *)mech + MECH_NAME, name, 32);
    if (!strcmp((char *)mech + MECH_SHOWNAME, "PLAYER")) {   // copied from our pilot: shown when targeted
        memset(mech + MECH_SHOWNAME, 0, 32);
        lstrcpynA((char *)mech + MECH_SHOWNAME, name, 32);
    }
    if (org) {
        memset(mech + MECH_ORGNAME, 0, 32);
        lstrcpynA((char *)mech + MECH_ORGNAME, org, 32);
    }
    *(uint8_t **)(mech + MECH_MECHANOID) = mechanoid;
    uint8_t *grp = make_group(name, (char *)mech + MECH_ORGNAME);
    *(uint8_t **)(mech + MECH_GROUP) = grp;
    typedef void (THISCALL *addmech_fn)(void *, void *);
    VFN(grp, VF_GROUP_ADDMECH, addmech_fn)(grp, mech);

    uint8_t info[0x400];
    CALL(A_GLIDER_INFO, glider_info_fn)(info, mechanoid + MECHANOID_CONFIG);
    name32 type;
    memcpy(type.s, info, 32);
    float mat[16];
    memcpy(mat, m, sizeof mat);
    uint8_t *g = CALL(A_CREATE_GLIDER, create_glider_fn)(G_GLIDER_MGR, type, mat, NULL);
    if (!g) return NULL;
    *(uint8_t **)(mechanoid + MECHANOID_GLIDER) = g;
    mech[0x65] = 1;
    CALL(A_ASSIGN_MECH, assign_fn)(g, mech, 1);
    mlog("coop: glider %p for '%s' (%s)", g, name, (char *)mech + MECH_ORGNAME);
    return g;
}

// The player pointers keep pointing at the freed glider after the game returns to the
// main menu: the menu counts as "not in flight" until the world is rendered again.
static volatile int g_in_menu;
void coop_leave(void);
void coop_menu_shown(void)
{
    // back from a game: that co-op session is over (else the lobby lists it and stays blocked)
    int was = g_in_menu;
    g_in_menu = 1;                                   // first: the gliders are gone with the world
    coop_show_version(1);
    if (!was && net_mode() != NET_OFF) { mlog("coop: back in the main menu, session closed"); coop_leave(); }
}

int coop_in_flight(void)
{
    uint8_t *g = G_PLAYER_GLIDER;
    return !g_in_menu && g && G_PLAYER_MECH && !IsBadReadPtr(g, OBJ_ALIVE + 1) && g[OBJ_ALIVE];
}
#define in_flight coop_in_flight

void coop_drive(uint8_t *g, const float m[16], const float vel[3])
{
    memcpy(g + OBJ_MATRIX, m, 12 * sizeof(float));
    CALL(A_OBJ_SETPOS, setpos_fn)(g, m[12], m[13], m[14]);
    memcpy(g + GLIDER_VELOCITY, vel, 3 * sizeof(float));
}

void coop_remove_glider(uint8_t *g)
{
    uint8_t *mech = *(uint8_t **)(g + GLIDER_MECH);
    uint8_t *mechanoid = mech ? *(uint8_t **)(mech + MECH_MECHANOID) : NULL;
    if (mechanoid && *(uint8_t **)(mechanoid + MECHANOID_GLIDER) == g) *(uint8_t **)(mechanoid + MECHANOID_GLIDER) = NULL;
    // explosions and missiles keep their glider (+0x10, followed and blamed for damage): not this one any more
    for (void **nd = *(void ***)A_OBJLIST_HEAD; nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!o || o == g) continue;
        int type = *(int *)(o + 0x54);
        if ((type == 0x0f || type == 0x10) && *(uint8_t **)(o + 0x10) == g) *(uint8_t **)(o + 0x10) = NULL;
        // gliders aiming at it (the player's selected target, NPC AI)
        if (type == 0x0e /* glider */ && *(uint8_t **)(o + GLIDER_TARGET) == g) *(uint8_t **)(o + GLIDER_TARGET) = NULL;
    }
    g[OBJ_ALIVE] = 0;        // CObjectList::Pack deletes it
}


// ---- adapter for the session core ------------------------------------------------------------
int cg_in_flight(void) { return coop_in_flight(); }
int cg_pilot_saveable(void) { return 1; }

int cg_pilot_name(char *out, int n)
{
    const char *p = A_PLAYER_NAME_PTR ? *(const char **)A_PLAYER_NAME_PTR : NULL;
    if (!p || IsBadStringPtrA(p, 32)) return 0;
    lstrcpynA(out, p, n);
    return 1;
}

uint8_t *cg_pilot_save(int *len)
{
    return coop_mechanoid_save(*(uint8_t **)(G_PLAYER_MECH + MECH_MECHANOID), len);
}

uint8_t *cg_spawn_player(const uint8_t *blob, int len, const float m[16], const char *name, int same_clan)
{
    return coop_spawn_glider(coop_mechanoid_load(blob, len), name, same_clan ? NULL : "ORG_FREE", m);
}

void cg_drive(uint8_t *g, const float m[16], const float vel[3]) { coop_drive(g, m, vel); }
// (back in the main menu the world and its gliders are already freed: nothing to remove)
void cg_remove_glider(uint8_t *g) { if (coop_in_flight()) coop_remove_glider(g); }

void cg_player_state(float m[16], float vel[3])
{
    memcpy(m, G_PLAYER_GLIDER + OBJ_MATRIX, 16 * sizeof(float));
    memcpy(vel, G_PLAYER_GLIDER + GLIDER_VELOCITY, 3 * sizeof(float));
}

void cg_player_setpos(float x, float y, float z) { CALL(A_OBJ_SETPOS, setpos_fn)(G_PLAYER_GLIDER, x, y, z); }

void cg_save_world(const char *file)
{
    typedef int (THISCALL *save_fn)(void *, const char *, gstr *, int, name32 *, int);
    typedef void (THISCALL *ctor_cstr_fn)(void *, const char *);
    typedef void (THISCALL *dtor_fn)(void *);
    gstr title;
    CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&title, "Co-op");
    name32 key;
    memset(&key, 0, sizeof key);
    CALL(A_SAVE_GAME, save_fn)(*(void **)A_APP_PTR, file, &title, 0, &key, 0);
    CALL(A_GSTR_DTOR, dtor_fn)(&title);
}

void cg_load_world(const char *file)
{
    if (lobby_load_world(file)) return;
    // already in a game: load like the quick-load key does
    typedef char (THISCALL *load_fn)(void *, const char *, int);
    uint8_t *app = *(uint8_t **)A_APP_PTR;
    if (app && CALL(A_LOAD_GAME, load_fn)(app, file, 1)) {
        *(int *)(app + 0x410) = 2;
        *(int *)(app + 0x44) = 1;
        *(uint8_t *)A_INGAME_LOAD_FLAG = 1;
    }
}

void cg_world_progress(int percent)
{
    char buf[256];
    snprintf(buf, sizeof buf, aim2_ui("mp_receiving"), percent);
    lobby_set_status(buf);
}

const char *cg_text(const char *key)
{
    char k[32];
    snprintf(k, sizeof k, "mp_%s", key);
    return aim2_ui(k);
}

int cg_handle(int type, int from, const uint8_t *data, int len, const char *name)
{
    return mirror_handle(type, from, data, len) || questlog_handle(type, from, data, len) ||
           notify_handle(type, from, data, len, name);
}

void cg_frame(void) { mirror_frame(); questlog_frame(); notify_frame(); }
void cg_session_started(void) { notify_session_started(); }
void cg_reset(void) { mirror_reset(); questlog_reset(); }
void cg_peer_joined(int peer) { mirror_peer_joined(peer); questlog_peer_joined(peer); }
uint8_t *cg_profile_extra(int *len) { return questlog_blob(len); }

void cg_profile_extra_restore(const uint8_t *data, int len)
{
    if (coop_my_clan()) questlog_apply(data, len);                             // own missions
    else net_send(0, 41 /* MSG_LOG_UP */, data, len, 1);                       // most complete log wins
}

// ---- hooks ----------------------------------------------------------------------------------
void debugmenu_frame(void);

static void THISCALL frame(void *app, int first)
{
    g_in_menu = 0;
    coop_show_version(0);
    void lights_frame(void);
    lights_frame();
    debugmenu_frame();
    coop_frame();
    CALL(A_RENDER, render_fn)(app, first);
}

// CMMApp::InitMainMech -> CGliderManager::CreateGlider: a new world (new
// game, loaded game, other location); all puppets of the old one are gone.
static void *THISCALL create_player_glider(void *mgr, name32 type, float *m, const char *name)
{
    int len;
    uint8_t *b = coop_new_world(&len);
    if (b && G_PLAYER_MECH) {
        // our own pilot instead of the host's
        uint8_t *mechanoid = coop_mechanoid_load(b, len);
        *(uint8_t **)(G_PLAYER_MECH + MECH_MECHANOID) = mechanoid;
        uint8_t info[0x400];
        CALL(A_GLIDER_INFO, glider_info_fn)(info, mechanoid + MECHANOID_CONFIG);
        memcpy(type.s, info, 32);
    }
    free(b);
    return CALL(A_CREATE_GLIDER, create_glider_fn)(mgr, type, m, name);
}

// CMMApp's message pump also runs in every menu loop (main, base, pause):
// keep the session alive there.
static void THISCALL pump(void *app)
{
    typedef void (THISCALL *pump_fn)(void *);
    CALL(A_APP_PUMP, pump_fn)(app);
    void lobby_idle(void);
    lobby_idle();
    coop_menu_poll();
}

// ---- commands ---------------------------------------------------------------------------------
int cg_command(const char *cmd, const char *arg, const char *line)
{
    if (!strcmp(cmd, "save") && in_flight()) {        // test: save to SAVES\<arg>
        typedef int (THISCALL *save_fn)(void *, const char *, gstr *, int, name32 *, int);
        typedef void (THISCALL *ctor_cstr_fn)(void *, const char *);
        typedef void (THISCALL *dtor_fn)(void *);
        gstr title;
        CALL(A_GSTR_CTOR_CSTR, ctor_cstr_fn)(&title, *arg ? arg : "test");
        name32 key;
        memset(&key, 0, sizeof key);
        int r = CALL(A_SAVE_GAME, save_fn)(*(void **)A_APP_PTR, arg[0] ? arg : "test.sav", &title, 0, &key, 0);
        CALL(A_GSTR_DTOR, dtor_fn)(&title);
        mlog("coop: saved %s (%d)", arg[0] ? arg : "test.sav", r);
    } else if (!strcmp(cmd, "tp") && in_flight() && A_OBJLIST_FIND) {   // test: tp <building> [dx dy dz]
        float dx = 0, dy = 0, dz = 30;
        sscanf(line, "%*s %*s %f %f %f", &dx, &dy, &dz);
        typedef uint8_t *(THISCALL *find_fn)(void *, name32, int);
        name32 n;
        memset(&n, 0, sizeof n);
        lstrcpynA(n.s, arg, 32);
        uint8_t *b = ((find_fn)A_OBJLIST_FIND)(G_OBJECT_LIST, n, 0xb);
        if (b) {
            const float *p = (const float *)(b + OBJ_POS);
            CALL(A_OBJ_SETPOS, setpos_fn)(G_PLAYER_GLIDER, p[0] + dx, p[1] + dy, p[2] + dz);
            memset(G_PLAYER_GLIDER + GLIDER_VELOCITY, 0, 12);
            mlog("coop: teleported to %s (%.0f %.0f %.0f)", arg, p[0] + dx, p[1] + dy, p[2] + dz);
        } else mlog("coop: no building %s", arg);
    } else if (!strcmp(cmd, "enter") && in_flight() && A_ENTER_BUILDING) {   // test: open a base menu
        typedef void (THISCALL *enter_fn)(void *, name32 *);
        name32 n;
        memset(&n, 0, sizeof n);
        lstrcpynA(n.s, arg, 32);
        ((enter_fn)A_ENTER_BUILDING)(NULL, &n);
        mlog("coop: entering %s", arg);
    } else if (!strcmp(cmd, "colortrace") && A_SET_COLOR) {
        if (!o_setcolor) {
            static const uint8_t pro[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x10};
            o_setcolor = (setcolor_fn)hook_jmp(A_SET_COLOR, pro, 6, h_setcolor);
        }
        g_trace_until = GetTickCount() + 1000;
    } else if (!strcmp(cmd, "spritetrace")) {
        if (!o_sprite) {
            static const uint8_t pro[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x18};
            o_sprite = (sprite_fn)hook_jmp(RA(0x004d4980), pro, 6, h_sprite);
        }
        g_strace_until = GetTickCount() + 1000;
    } else if (!strcmp(cmd, "colorhits")) {
        mlog("coop: player colour used %d times", g_color_hits);
    } else if (!strcmp(cmd, "goto") && in_flight()) {      // test: goto x y z
        float x, y, z;
        if (sscanf(line, "%*s %f %f %f", &x, &y, &z) == 3) {
            CALL(A_OBJ_SETPOS, setpos_fn)(G_PLAYER_GLIDER, x, y, z);
            memset(G_PLAYER_GLIDER + GLIDER_VELOCITY, 0, 12);
            mlog("coop: moved to %.0f %.0f %.0f", x, y, z);
        }
    } else if (!strcmp(cmd, "time") && G_WORLD) {          // test: time [hour of the day]
        float *t = (float *)(G_WORLD + 4), h;
        if (sscanf(line, "%*s %f", &h) == 1) *t = floorf(*t / 86400.0f) * 86400.0f + h * 3600.0f;
        mlog("coop: world time %.0f s (%.2f h)", *t, fmodf(*t, 86400.0f) / 3600.0f);
    } else if (!strcmp(cmd, "lamps") && in_flight()) {     // test: the level's light objects (type 0x13)
        const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        int n = 0;
        for (void **nd = *(void ***)A_OBJLIST_HEAD; nd && n < 40; nd = (void **)nd[1]) {
            uint8_t *o = (uint8_t *)nd[0];
            if (!o || *(int *)(o + 0x54) != 0x13) continue;
            const float *p = (const float *)(o + OBJ_POS);
            float dx = p[0] - pp[0], dy = p[1] - pp[1];
            mlog("lamps: %9.0f %9.0f %7.0f dist %.0f r %.1f '%.20s'", p[0], p[1], p[2], sqrtf(dx * dx + dy * dy), *(float *)(o + 0x88), (char *)o + 0x14);
            n++;
        }
        mlog("lamps: %d", n);
    } else if (!strcmp(cmd, "pos") && in_flight()) {
        const float *p = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        const float *m = (const float *)(G_PLAYER_GLIDER + OBJ_MATRIX), *v = (const float *)(G_PLAYER_GLIDER + GLIDER_VELOCITY);
        mlog("coop: position %.1f %.1f %.1f vel %.2f %.2f %.2f | r0 %.2f %.2f %.2f r1 %.2f %.2f %.2f r2 %.2f %.2f %.2f",
             p[0], p[1], p[2], v[0], v[1], v[2], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    } else if (!strcmp(cmd, "pilotptrs") && in_flight()) {
        mlog("pilotptrs: glider %p mech %p mechanoid %p delta %lx", G_PLAYER_GLIDER, G_PLAYER_MECH, *(void **)(G_PLAYER_MECH + MECH_MECHANOID), (unsigned long)g_exe_delta);
    } else if (!strcmp(cmd, "findpilot") && in_flight()) {     // test: where our pilot's name is kept
        uint8_t *mech = G_PLAYER_MECH, *mo = *(uint8_t **)(mech + MECH_MECHANOID);
        struct { const char *what; uint8_t *p; int n; } objs[] = {{"mech", mech, SZ_MECH}, {"mechanoid", mo, SZ_MECHANOID}};
        for (int k = 0; k < 2; k++)
            for (int i = 0; objs[k].p && i < objs[k].n - 4; i++)
                if (!memcmp(objs[k].p + i, *arg ? arg : "SMT", strlen(*arg ? arg : "SMT")))
                    mlog("findpilot: %s+0x%x: %.24s", objs[k].what, i, (char *)objs[k].p + i);
    } else if (!strcmp(cmd, "findname")) {          // test: where a puppet holds "PLAYER"
        uint8_t *g = coop_remote_by_index(0, NULL);
        if (g) {
            uint8_t *mech = *(uint8_t **)(g + GLIDER_MECH);
            uint8_t *mo = mech ? *(uint8_t **)(mech + MECH_MECHANOID) : NULL;
            struct { const char *what; uint8_t *p; int n; } objs[] = {{"glider", g, 0x1400}, {"mech", mech, SZ_MECH}, {"mechanoid", mo, SZ_MECHANOID},
                {"group", mech ? *(uint8_t **)(mech + MECH_GROUP) : NULL, SZ_GROUP}};
            for (int k = 0; k < 4; k++)
                for (int i = 0; objs[k].p && i < objs[k].n - 6; i++)
                    if (!memcmp(objs[k].p + i, "PLAYER", 6) || !memcmp(objs[k].p + i, "Guest", 5) || !memcmp(objs[k].p + i, "Host", 4))
                        mlog("findname: %s+0x%x: %.20s", objs[k].what, i, (char *)objs[k].p + i);
        }
    } else if (!strcmp(cmd, "video") && A_VIDEO_IFACE) {   // test: play a video like CMMApp does
        typedef void *(__cdecl *get_fn)(void *);
        typedef int (THISCALL *init_fn)(void *, HWND, int, int, int);
        typedef void (THISCALL *play_fn)(void *, const char *, int);
        typedef void (THISCALL *v0_fn)(void *);
        typedef void (THISCALL *del_fn)(void *, int);
        uint8_t *ap = *(uint8_t **)A_APP_PTR;
        void *pl = ((get_fn)A_PANEL_GET)(*(void **)A_VIDEO_IFACE);
        mlog("coop: playing %s", arg);
        if (pl && VFN(pl, 0x14, init_fn)(pl, *(HWND *)(ap + 0x3c), 800, 600, 0x20) >= 0)
            VFN(pl, 0x1c, play_fn)(pl, arg[0] ? arg : "Data\\Video\\1C.bik", 1);
        if (pl) { VFN(pl, 0x28, v0_fn)(pl); VFN(pl, 0, del_fn)(pl, 1); }
        mlog("coop: video finished");
    } else if (!strcmp(cmd, "econ")) {
        economy_dump();
    } else if (!strcmp(cmd, "econhour")) {
        economy_run_hour();
    } else if (!strcmp(cmd, "profnpc") && in_flight()) {
        // test: make the nearest NPC's pilot our co-op profile
        const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        uint8_t *best = NULL;
        float bd = 1e18f;
        for (void **nd = *(void ***)A_OBJLIST_HEAD; nd; nd = (void **)nd[1]) {
            uint8_t *o = (uint8_t *)nd[0];
            if (!o[OBJ_ALIVE] || *(int *)(o + 0x54) != 0x0e || o == G_PLAYER_GLIDER || coop_is_remote(o)) continue;
            uint8_t *mech = *(uint8_t **)(o + GLIDER_MECH);
            if (!mech || !*(uint8_t **)(mech + MECH_MECHANOID)) continue;
            {   // a different glider type than ours
                int l1, l2;
                uint8_t *b1 = coop_mechanoid_save(*(uint8_t **)(mech + MECH_MECHANOID), &l1);
                uint8_t *b2 = coop_mechanoid_save(*(uint8_t **)(G_PLAYER_MECH + MECH_MECHANOID), &l2);
                int same = l1 > 0x40 && l2 > 0x40 && !memcmp(b1 + 0x20, b2 + 0x20, 0x20);
                free(b1);
                free(b2);
                if (same) continue;
            }
            const float *p = (const float *)(o + OBJ_POS);
            float d = (p[0] - pp[0]) * (p[0] - pp[0]) + (p[1] - pp[1]) * (p[1] - pp[1]);
            if (d < bd) { bd = d; best = o; }
        }
        if (best) {
            uint8_t *mech = *(uint8_t **)(best + GLIDER_MECH);
            int len;
            uint8_t *b = coop_mechanoid_save(*(uint8_t **)(mech + MECH_MECHANOID), &len);
            char p[MAX_PATH];
            coop_profile_path(p, sizeof p);
            FILE *f = fopen(p, "wb");
            if (f) { uint32_t ml = len, xl = 0; fwrite("RRP1", 1, 4, f); fwrite(&ml, 4, 1, f); fwrite(b, 1, len, f); fwrite(&xl, 4, 1, f); fclose(f); }
            free(b);
            coop_profile_keep();                        // keep it for the test
            mlog("coop: profile set from %s", (char *)mech + MECH_NAME);
        }
    } else if ((!strcmp(cmd, "qldump") || !strcmp(cmd, "qladd")) && in_flight()) {
        void questlog_test(const char *cmd, const char *arg);
        questlog_test(cmd, arg);
    } else if (!strcmp(cmd, "testlight")) {
        float dx = 0, dy = 0, dz = -10, r = 120;
        sscanf(line, "%*s %f %f %f %f", &dx, &dy, &dz, &r);
        void lights_test(float, float, float, float);
        lights_test(dx, dy, dz, r);
        const float *p = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        mlog("testlight: player at %.0f %.0f %.0f", p[0], p[1], p[2]);
    } else if (!strcmp(cmd, "objstat") && in_flight()) {
        // test: objects within 1500 m by vtable and type (finding missiles, explosions, lights)
        struct { uintptr_t vt; int type, n; float d; uint8_t *o; } st[64];
        int ns = 0;
        const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        for (void **nd = *(void ***)A_OBJLIST_HEAD; nd; nd = (void **)nd[1]) {
            uint8_t *o = (uint8_t *)nd[0];
            if (!o) continue;
            const float *p = (const float *)(o + OBJ_POS);
            float dx = p[0] - pp[0], dy = p[1] - pp[1], d = sqrtf(dx * dx + dy * dy);
            if (d > 1500.0f) continue;
            uintptr_t vt = *(uintptr_t *)o - g_exe_delta;
            int k;
            for (k = 0; k < ns; k++) if (st[k].vt == vt) break;
            if (k == ns && ns < 64) { st[ns].vt = vt; st[ns].type = *(int *)(o + 0x54); st[ns].n = 0; st[ns].d = d; st[ns].o = o; ns++; }
            if (k < ns) { st[k].n++; if (d < st[k].d) { st[k].d = d; st[k].o = o; } }
        }
        for (int k = 0; k < ns; k++)
            mlog("objstat: vt %06x type %02x x%d nearest %.0f m (%p '%.20s')", (unsigned)st[k].vt, st[k].type, st[k].n, st[k].d, st[k].o, (char *)st[k].o + 0x14);
    } else if (!strcmp(cmd, "npcs") && in_flight()) {
        // test: log gliders near the player (name, position)
        const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        for (void **nd = *(void ***)A_OBJLIST_HEAD; nd; nd = (void **)nd[1]) {
            uint8_t *o = (uint8_t *)nd[0];
            if (!o[OBJ_ALIVE] || *(int *)(o + 0x54) != 0x0e) continue;
            const float *p = (const float *)(o + OBJ_POS);
            float dx = p[0] - pp[0], dy = p[1] - pp[1];
            if (dx * dx + dy * dy > 6000.0f * 6000.0f) continue;
            uint8_t *mech = *(uint8_t **)(o + GLIDER_MECH);
            mlog("npcs: %-16s %9.1f %9.1f %7.1f dist %.0f lod %d flags 6a=%d 6b=%d 7b=%d 288=%d%s", mech ? (char *)mech + MECH_NAME : "?", p[0], p[1], p[2], sqrtf(dx * dx + dy * dy), *(uint8_t **)(o + 0x1180) ? *(int *)(*(uint8_t **)(o + 0x1180) + 0x78) : -1,
                 o[0x6a], o[0x6b], o[0x7b], *(int *)(o + 0x288), coop_is_remote(o) ? " (player)" : o == G_PLAYER_GLIDER ? " (me)" : "");
        }
    } else return 0;
    return 1;
}

// test: which code sets which colours (CRender::SetColor) during one second
static struct { uintptr_t ret; uint32_t color; int n; } g_ctrace[64];
static int g_nctrace;
static void THISCALL h_setcolor(void *r, uint32_t color, int a)
{
    if (g_trace_until && GetTickCount() < g_trace_until) {
        uintptr_t ret = (uintptr_t)__builtin_return_address(0) - g_exe_delta;
        int i;
        for (i = 0; i < g_nctrace; i++) if (g_ctrace[i].ret == ret && g_ctrace[i].color == color) break;
        if (i == g_nctrace && g_nctrace < 64) { g_ctrace[g_nctrace].ret = ret; g_ctrace[g_nctrace].color = color; g_ctrace[g_nctrace++].n = 0; }
        if (i < 64) g_ctrace[i].n++;
    } else if (g_trace_until) {
        g_trace_until = 0;
        for (int i = 0; i < g_nctrace; i++) mlog("colortrace: %08lx %08lx x%d", (unsigned long)g_ctrace[i].ret, (unsigned long)g_ctrace[i].color, g_ctrace[i].n);
        g_nctrace = 0;
    }
    o_setcolor(r, color, a);
}

// test: who draws sprites (0x4d4980, 6 args) during one second
static void THISCALL h_sprite(void *r, int a, int b, int c2, int d, int e, int f)
{
    if (g_strace_until && GetTickCount() < g_strace_until) {
        uintptr_t ret = (uintptr_t)__builtin_return_address(0) - g_exe_delta;
        int i;
        for (i = 0; i < g_nctrace; i++) if (g_ctrace[i].ret == ret && g_ctrace[i].color == (uint32_t)a) break;
        if (i == g_nctrace && g_nctrace < 64) { g_ctrace[g_nctrace].ret = ret; g_ctrace[g_nctrace].color = (uint32_t)a; g_ctrace[g_nctrace++].n = 0; }
        if (i < 64) g_ctrace[i].n++;
    } else if (g_strace_until) {
        g_strace_until = 0;
        for (int i = 0; i < g_nctrace; i++) mlog("spritetrace: %08lx arg %08lx x%d", (unsigned long)g_ctrace[i].ret, (unsigned long)g_ctrace[i].color, g_ctrace[i].n);
        g_nctrace = 0;
    }
    o_sprite(r, a, b, c2, d, e, f);
}

// Radar / map colour: CGroupAI vf 0x50 (GetPlayerColor) of the glider's group;
// players of our clan get their own shade of green.
typedef uint32_t (THISCALL *color_fn)(void *);
static color_fn o_color[4];

static int remote_ai(void *ai)
{
    const char *name;
    uint8_t *g;
    for (int i = 0; (g = coop_remote_by_index(i, &name)) != NULL; i++) {
        uint8_t *mech = *(uint8_t **)(g + GLIDER_MECH);
        uint8_t *grp = mech ? *(uint8_t **)(mech + MECH_GROUP) : NULL;
        if (grp && *(void **)(grp + GRP_AI) == ai) return coop_glider_same_clan(g) ? 1 : -1;
    }
    return 0;
}

#define COLOR_HOOK(n) static uint32_t THISCALL h_color##n(void *ai) \
    { if (net_mode() != NET_OFF && remote_ai(ai) == 1) { g_color_hits++; return coop_player_color(); } return o_color[n](ai); }
COLOR_HOOK(0) COLOR_HOOK(1) COLOR_HOOK(2) COLOR_HOOK(3)

static void color_install(void)
{
    const uintptr_t vts[] = {VT_GROUP_AIS};
    void *hooks[4] = {(void *)h_color0, (void *)h_color1, (void *)h_color2, (void *)h_color3};
    for (int i = 0; i < 4 && vts[i]; i++) {
        uintptr_t slot = vts[i] + VF_AI_COLOR;
        o_color[i] = *(color_fn *)slot;
        uint32_t cur = (uint32_t)(uintptr_t)o_color[i];
        patch_bytes(slot, &cur, &hooks[i], 4);
    }
}

// Full map (CRadar::RenderBigMap): after the player's arrow, one arrow per other
// player in this world, tinted with the player colour.
typedef void (THISCALL *arrow_fn)(void *, int, int, const float *, uint32_t, float);
typedef void (THISCALL *setcolor2_fn)(void *, uint32_t, int);
static void THISCALL bigmap_arrow(void *radar, int x, int y, const float *pos, uint32_t color, float scale)
{
    CALL(A_BIGMAP_ARROW, arrow_fn)(radar, x, y, pos, color, scale);
    if (net_mode() == NET_OFF) return;
    const char *name;
    uint8_t *g;
    void *rs = A_RENDER_STATE_PTR ? *(void **)A_RENDER_STATE_PTR : NULL;
    int tinted = 0;
    for (int i = 0; (g = coop_remote_by_index(i, &name)) != NULL; i++) {
        if (!g[OBJ_ALIVE]) continue;
        if (rs && !tinted) { CALL(A_SET_COLOR, setcolor2_fn)(rs, coop_player_color(), 3); tinted = 1; }
        CALL(A_BIGMAP_ARROW, arrow_fn)(radar, x, y, (const float *)(g + 0x190), coop_player_color(), scale);
        // the arrow keeps the game's green: the name next to it marks the player
        const uint8_t *r = radar;
        const float *p = (const float *)(g + 0x190);
        float k = (A_BIGMAP_UNITS ? *(const float *)A_BIGMAP_UNITS : 1.0f) / (float)(1 << (*(const int *)(r + 0x78) & 31));
        float sx = *(const float *)(r + 4) * (p[0] * k - (*(const float *)(r + 8) + *(const int *)(r + 200) * 64.0f));
        float sy = *(const float *)(r + 4) * (p[1] * k - (*(const float *)(r + 0xc) + *(const int *)(r + 0xcc) * 64.0f));
        if (sx > 2 && sy > 2 && sx < *(const int *)(r + 0x90) - 2 && sy < *(const int *)(r + 0x94) - 2) {
            void hud_text(float x, float y, const char *s, uint32_t color);
            hud_text(x + sx + 9.0f, y + sy - 7.0f, name, coop_player_color());
        }
    }
    if (tinted) CALL(A_SET_COLOR, setcolor2_fn)(rs, 0xe6ffffff, 3);   // RenderBigMap's own 0.9 alpha
}

// The game refuses to start when another A.I.M. 2 window exists; allow it
// (several co-op players on one PC, testing).
static HWND (WINAPI *o_FindWindowA)(LPCSTR, LPCSTR);
static HWND WINAPI h_FindWindowA(LPCSTR cls, LPCSTR title)
{
    if (cls == (LPCSTR)A_WNDCLASS) return NULL;
    return o_FindWindowA(cls, title);
}

void aim2_coop_install(void)
{
    coop_core_install();
    HMODULE crt = GetModuleHandleA(CRT_DLL);
    crt_free = crt ? (void *)GetProcAddress(crt, "free") : NULL;
    patch_call(S_FRAME_RENDER, frame);                  // CMMApp::CycleGame -> CMMApp::Render
    patch_call(S_INITMAINMECH_CREATE, create_player_glider);   // CMMApp::InitMainMech
    if (VF_AI_COLOR) color_install();
    if (S_BIGMAP_ARROW) patch_call(S_BIGMAP_ARROW, bigmap_arrow);   // other players on the full map
    mirror_install();
    questlog_install();
    notify_install();
    const uintptr_t pumps[] = {S_PUMPS};
    for (size_t i = 0; i < sizeof pumps / sizeof pumps[0]; i++) patch_call(pumps[i], pump);
    if (GetPrivateProfileIntA("Coop", "MultiInstance", 0, g_ini) || GetPrivateProfileIntA("Dev", "Harness", 0, g_ini))
        iat_hook_one(GetModuleHandleA(NULL), "user32.dll", "FindWindowA", h_FindWindowA, (void **)&o_FindWindowA);
}
