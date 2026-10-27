// Co-op (A.I.M.): the game's adapter for the shared session core
// (common/coop_core.c) - puppet gliders, pilot data, world saves, notices, radar.
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef void *(__cdecl *new_fn)(size_t);
typedef void (__cdecl *delete_fn)(void *);
typedef void (THISCALL *ctor_fn)(void *);
typedef void (THISCALL *stream_io_fn)(void *, gstream *, int);
typedef void *(THISCALL *create_glider_fn)(void *, name32, float *, const char *);
typedef void (THISCALL *assign_fn)(void *, void *);
typedef void (THISCALL *render_fn)(void *, int);
typedef char (THISCALL *devcheck_fn)(void *, int);
typedef void (THISCALL *setpos_fn)(void *, float, float, float);
typedef void (THISCALL *gstr_ctor_fn)(gstr *, const char *);
typedef void (THISCALL *gstr_dtor_fn)(gstr *);

// ---- notices ---------------------------------------------------------------------------------
static char g_notes[8][128];
static int g_nnotes;

void coop_notify(const char *text)
{
    mlog("coop: notice: %s", text);
    if (g_nnotes < 8) lstrcpynA(g_notes[g_nnotes++], text, 128);
}

static void flush_notices(void)
{
    uint8_t *msgr = *(uint8_t **)A_MESSAGER_PTR;
    if (!msgr) return;
    typedef void (THISCALL *add_fn)(void *, gstr *, int, int);
    for (int i = 0; i < g_nnotes; i++) {
        gstr s;
        CALL(A_GSTR_CTOR_CSTR, gstr_ctor_fn)(&s, g_notes[i]);
        CALL(A_ADD_MESSAGE, add_fn)(msgr, &s, 1, 0);
        CALL(A_GSTR_DTOR, gstr_dtor_fn)(&s);
    }
    g_nnotes = 0;
}

// ---- engine helpers ------------------------------------------------------------------------
uint8_t *coop_mechanoid_save(uint8_t *mo, int *len)
{
    gstream s = {0, 0x10000, 0, CALL(A_NEW, new_fn)(0x10000)};
    CALL(A_MECHANOID_SAVE, stream_io_fn)(mo, &s, 1);
    uint8_t *out = malloc(s.pos ? s.pos : 1);
    memcpy(out, s.buf, s.pos);
    *len = s.pos;
    if (!s.owned && s.buf) CALL(A_DELETE, delete_fn)(s.buf);
    return out;
}

// Loading into an existing mechanoid can leave subsystems flagged destroyed
// (the pilot would explode at once): repair it fully afterwards, as landing
// in a building does.
static void mechanoid_load(uint8_t *mo, const uint8_t *blob, int len)
{
    gstream s = {1, len, 0, (uint8_t *)blob};    // owned=1: the stream must not free it
    CALL(A_MECHANOID_LOAD, stream_io_fn)(mo, &s, 1);
    CALL(A_MECHANOID_REPAIR, ctor_fn)(mo);
}

static DWORD g_last_frame;       // the flight loop ran recently (not loading, not in a base)

int coop_in_flight(void)
{
    return G_APP && *(int *)(G_APP + APP_MODE) != 0 && G_PLAYER_GLIDER && G_PLAYER_GLIDER[OBJ_ALIVE] &&
           GetTickCount() - g_last_frame < 1000;
}
#define in_flight coop_in_flight

// the object's own SetPosition (as the movement code uses it): it keeps the
// map cell the object is registered in consistent with its position
static void set_pos(uint8_t *obj, const float *xyz)
{
    VFN(obj, VF_OBJ_SETPOS, setpos_fn)(obj, xyz[0], xyz[1], xyz[2]);
}

// A puppet: a pilot object without AI (like the PvP mode's network players)
// flying a glider built from the remote player's mechanoid.
uint8_t *coop_spawn_glider(const uint8_t *blob, int len, const float m[16])
{
    uint8_t *mech = CALL(A_NEW, new_fn)(SZ_MECH);
    if (!mech) return NULL;
    CALL(A_MECH_CTOR, ctor_fn)(mech);
    *(int *)(mech + 0x60f4) = 1;               // as CGliderManager::CreateNetObject
    *(int *)(mech + 0x6238) = 1;
    uint8_t *mo = mech + MECH_MECHANOID;
    *(uint8_t **)(mo + MECHANOID_MECH) = mech;
    mechanoid_load(mo, blob, len);
    name32 type;
    memcpy(&type, mo + MECHANOID_TYPE, sizeof type);
    float mat[16];
    memcpy(mat, m, sizeof mat);
    uint8_t *g = CALL(A_CREATE_GLIDER, create_glider_fn)((void *)A_GLIDER_MGR, type, mat, NULL);
    if (!g) return NULL;
    *(uint8_t **)(mo + MECHANOID_GLIDER) = g;
    CALL(A_ASSIGN_MECHANOID, assign_fn)(g, mo);
    g[OBJ_NET] = 0;
    g[OBJ_LOCAL] = 0;
    mlog("coop: puppet %p (%s)", g, (char *)g + OBJ_MODEL);
    return g;
}

// a remote player's glider: the pilot (mechanoid name, shown when targeted) gets the player's name
uint8_t *coop_spawn_player(const uint8_t *blob, int len, const float m[16], const char *name)
{
    uint8_t *g = coop_spawn_glider(blob, len, m);
    uint8_t *mo = g ? *(uint8_t **)(g + GLIDER_MECHANOID) : NULL;
    if (mo && name && name[0]) {
        memset(mo, 0, 32);
        lstrcpynA((char *)mo, name, 32);
    }
    return g;
}

void coop_drive(uint8_t *g, const float m[16], const float vel[3])
{
    memcpy(g + OBJ_MATRIX, m, 12 * sizeof(float));
    set_pos(g, m + 12);
    memcpy(g + GLIDER_VELOCITY, vel, 3 * sizeof(float));
}

void coop_remove_glider(uint8_t *g)
{
    uint8_t *mo = *(uint8_t **)(g + GLIDER_MECHANOID);
    if (mo && *(uint8_t **)(mo + MECHANOID_GLIDER) == g) *(uint8_t **)(mo + MECHANOID_GLIDER) = NULL;
    // explosions and missiles keep their glider (+0x10, followed and blamed for damage): not this one any more
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!o || o == g) continue;
        int type = *(int *)(o + 0x54);
        if ((type == 0x0f || type == 0x10) && *(uint8_t **)(o + 0x10) == g) *(uint8_t **)(o + 0x10) = NULL;
        // gliders aiming at it (the player's selected target, NPC AI): the cockpit and the AI use it every frame.
        // The game's own "glider gone" 0x4dfde0 clears both the AI's target (+0x10d8 -> +0x44, set since
        // npc_aggro sends NPCs at puppets, #22) and the selected one (picks another).
        if (type == 0x0e) {
            typedef void (THISCALL *forget_fn)(uint8_t *, uint8_t *);
            CALL(0x004dfde0, forget_fn)(o, g);
            if (*(uint8_t **)(o + 0xb98) == g) *(uint8_t **)(o + 0xb98) = NULL;
        }
    }
    g[OBJ_ALIVE] = 0;        // CObjectList::Pack deletes it
}

// load a world like the in-game load menu does
static void load_in_game(const char *file)
{
    typedef char (THISCALL *load_fn)(void *, const char *, int);
    uint8_t *app = G_APP;
    if (app && CALL(A_LOAD_GAME, load_fn)(app, file, 1)) {
        *(int *)(app + 0x44) = 1;
        *(uint8_t *)0x00677120 = 1;
        *(int *)(app + APP_MODE) = 2;
    }
}

// The player's name in dialogues ("%NAME", e.g. a base's greeting) is a string of the script
// manager (+0xf88), saved with the world - in the host's world, the host's name. A guest puts its
// own back.
#define SM_PLAYER_NAME 0xf88

static void guest_player_name(void)
{
    static DWORD last;
    if (net_mode() != NET_CLIENT || !coop_in_flight() || GetTickCount() - last < 1000) return;
    last = GetTickCount();
    uint8_t *sm = *(uint8_t **)A_TEXT_DB_PTR;
    const char *me = coop_my_name();
    if (!sm || !me[0]) return;
    gstr *cur = (gstr *)(sm + SM_PLAYER_NAME);
    if (cur->buf && !strcmp(cur->buf, me)) return;
    gstr tmp;
    CALL(A_GSTR_CTOR_CSTR, gstr_ctor_fn)(&tmp, me);
    static int nlog;
    if (tmp.vt != cur->vt) {                       // not an engine string here: leave it
        if (nlog++ < 3) mlog("coop: player name string not found (%p vs %p)", cur->vt, tmp.vt);
        CALL(A_GSTR_DTOR, gstr_dtor_fn)(&tmp);
        return;
    }
    if (nlog++ < 5) mlog("coop: dialogues call us '%s' (the world said '%s')", me, cur->buf ? cur->buf : "");
    char f = cur->flag;
    char *b = cur->buf;
    cur->flag = tmp.flag;
    cur->buf = tmp.buf;
    tmp.flag = f;
    tmp.buf = b;
    CALL(A_GSTR_DTOR, gstr_dtor_fn)(&tmp);         // frees the host's name
}

// ---- adapter for the session core ------------------------------------------------------------
int cg_in_flight(void) { return coop_in_flight(); }

int cg_pilot_saveable(void)                    // not while dying
{
    uint8_t *mo = G_PLAYER_MECHANOID;
    return !mo[0x74] && !mo[0xac] && !mo[0x154] && !G_PLAYER_GLIDER[0x10d0];
}

uint8_t *cg_pilot_save(int *len) { return coop_mechanoid_save(G_PLAYER_MECHANOID, len); }

int cg_pilot_name(char *out, int n)
{
    lstrcpynA(out, (char *)G_PLAYER_MECHANOID, n);           // the mechanoid's name comes first
    return 1;
}

uint8_t *cg_spawn_player(const uint8_t *blob, int len, const float m[16], const char *name, int same_clan)
{
    (void)same_clan;
    return coop_spawn_player(blob, len, m, name);
}

void cg_drive(uint8_t *g, const float m[16], const float vel[3]) { coop_drive(g, m, vel); }
// (back in the main menu the world and its gliders are already freed: nothing to remove)
void cg_remove_glider(uint8_t *g) { if (coop_in_flight() || g_base_world) coop_remove_glider(g); }

void cg_player_state(float m[16], float vel[3])
{
    memcpy(m, G_PLAYER_GLIDER + OBJ_MATRIX, 16 * sizeof(float));
    memcpy(vel, G_PLAYER_GLIDER + GLIDER_VELOCITY, 3 * sizeof(float));
}

void cg_player_setpos(float x, float y, float z)
{
    float p[3] = {x, y, z};
    set_pos(G_PLAYER_GLIDER, p);
}

void cg_save_world(const char *file)
{
    typedef int (THISCALL *save_fn)(void *, const char *, gstr *, int, name32, int);
    gstr title;
    CALL(A_GSTR_CTOR_CSTR, gstr_ctor_fn)(&title, "Co-op");
    // No base: the joining player starts in flight where the host is (an
    // empty name would match some building and start the player there).
    name32 base;
    memset(&base, 0, sizeof base);
    lstrcpyA(base.s, "COOP_IN_FLIGHT");
    CALL(A_SAVE_GAME, save_fn)(G_APP, file, &title, 1, base, 1);
    CALL(A_GSTR_DTOR, gstr_dtor_fn)(&title);
}

void cg_load_world(const char *file)
{
    if (lobby_waiting()) lobby_world_ready();
    else load_in_game(file);
}

void cg_world_progress(int percent) { (void)percent; }    // the lobby polls coop_world_receiving
const char *cg_text(const char *key) { return aim1_ui(key); }

int cg_handle(int type, int from, const uint8_t *data, int len, const char *name)
{
    (void)name;
    return mirror_handle(type, from, data, len) || questlog_handle(type, from, data, len) ||
           respawn_handle(type, from, data, len) || loot_handle(type, from, data, len) ||
           killcredit_handle(type, from, data, len);
}

void cg_frame(void)
{
    mirror_frame();
    loot_frame();
    guest_player_name();
    questlog_frame();
    if (coop_in_flight()) { respawn_frame(); mapmark_frame(); }
    if (coop_in_flight()) flush_notices();
}

void cg_session_started(void) {}
void cg_reset(void) { mirror_reset(); questlog_reset(); respawn_reset(); loot_reset(); mapmark_reset(); }
void cg_peer_joined(int peer) { mirror_peer_joined(peer); questlog_peer_joined(peer); loot_peer_joined(peer); }

// ---- hooks ----------------------------------------------------------------------------------
static void THISCALL frame(void *app, int first)
{
    g_last_frame = GetTickCount();
    coop_show_version(0);
    void aim1_lights_frame(void);
    aim1_lights_frame();
    void aim1_debug_frame(void);
    aim1_debug_frame();
    coop_frame();
    CALL(A_RENDER, render_fn)(app, first);
}

// CMMApp::InitMainMech -> CreateGlider: a new world (new game, loaded game,
// other location); the puppets of the old one are gone.
static void *THISCALL create_player_glider(void *mgr, name32 type, float *m, const char *name)
{
    int len;
    uint8_t *b = coop_new_world(&len);
    if (b) {
        mechanoid_load(G_PLAYER_MECHANOID, b, len);    // our own pilot instead of the host's
        memcpy(&type, G_PLAYER_MECHANOID + MECHANOID_TYPE, sizeof type);
        free(b);
    }
    return CALL(A_CREATE_GLIDER, create_glider_fn)(mgr, type, m, name);
}

// Panels opened in flight from CMMApp::ProcessInput: the hold / journal / map / stats panel
// (CBaseMenu::DoModal at 0x420650) and the ESC menu (RunMenu at 0x4204e9). The flight loop stops
// while they are open, so the others lost our glider after 2 s; it is still out there.
static int g_flight_panel;
typedef int (THISCALL *domodal_fn)(void *, int);
typedef int (THISCALL *runmenu_fn)(void *, void *, int);

// a joining player's profile is saved as a panel opens: the ESC menu is the way out of the game (#42)
static void save_before_panel(void) { if (net_mode() == NET_CLIENT) coop_profile_save(); }

static int THISCALL panel_modal(void *menu, int a)
{
    save_before_panel();
    g_flight_panel++;
    int r = CALL(0x0042d5e0, domodal_fn)(menu, a);
    g_flight_panel--;
    return r;
}

static int THISCALL esc_menu(void *app, void *menu, int a)
{
    save_before_panel();
    g_flight_panel++;
    int r = CALL(A_RUN_MENU, runmenu_fn)(app, menu, a);
    g_flight_panel--;
    return r;
}

// ---- the host's world while the host is in a base (#37) ---------------------------------------
// CMMApp::CycleGame shows a base with the base screen 0x423b20 (DoModal at 0x423bd9) and the
// flight loop (CMMApp::Render) stops: the guests, who run no NPCs of their own, saw every NPC
// frozen. The world stays loaded meanwhile, so the host runs the world part of Render from the
// base screen's menu loop: group AI, object Run / PreMove / collisions / Move, Pack. Not the
// sensors (building entry), camera, rendering, effects or the hourly organisation / script update.
// The host's own glider, parked outside, is left out (its run flags +0x59 / +0x5a cleared for the
// tick) and takes no damage (mirror.c h_damage).
// On the way out CycleGame catches up 25 s of world time (0x503a40): with the camera put far away
// (0x68f0d4 = -10000) the groups drop their gliders and make new ones, new pilot ids for the
// guests. Not needed once the world ran: skipped.
#define FRAME_DT        (*(float *)0x00578088)     // the frame time the game's Run functions use
#define A_GROUP_LIST    0x00678f50                 // CMechGroupList
#define OBJ_RUN_ON      0x59                       // CObjectList::Run runs it
#define OBJ_MOVE_ON     0x5a                       // CObjectList::PreMove / Move move it

int g_base_world;
static int g_world_in_base = 1;      // [Coop] HostWorldInBase
static int g_in_base;                // the base screen is open
static int g_base_gen;               // the world it was opened in
static int g_modal_pump;             // menu_pump is called by DoModal's loop (not a loading screen's)
static int g_world_ran;              // the world ran during this visit
typedef unsigned (THISCALL *grouplist_run_fn)(void *, float, int, int);
typedef void (THISCALL *objlist_fn)(void *);
typedef void (THISCALL *pack_fn)(void *, int);
typedef void (THISCALL *catchup_fn)(void *, float);

static int THISCALL base_modal(void *menu, int a)
{
    g_in_base = 1;
    g_base_gen = coop_world_gen();
    g_world_ran = 0;
    int r = CALL(0x0042d5e0, domodal_fn)(menu, a);
    g_in_base = 0;
    if (g_world_ran) mlog("coop: the world ran while we were in the base");
    return r;
}

static int guests_here(void)
{
    for (int i = 1; i < NET_MAX_PEERS; i++) if (net_peer_connected(i)) return 1;
    return 0;
}

static void base_world_tick(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    uint8_t *app = G_APP, *pg = G_PLAYER_GLIDER;
    if (!g_world_in_base || !g_in_base || !g_modal_pump || net_mode() != NET_HOST || !guests_here() ||
        coop_world_gen() != g_base_gen || !app || *(int *)(app + APP_MODE) == 0 || *(int *)(app + 0x44) ||
        !pg || !pg[OBJ_ALIVE]) {
        last = now;
        return;
    }
    if (now - last < 33) return;
    float dt = (now - last) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    last = now;
    float keep_dt = FRAME_DT;
    uint8_t run_on = pg[OBJ_RUN_ON], move_on = pg[OBJ_MOVE_ON];
    pg[OBJ_RUN_ON] = pg[OBJ_MOVE_ON] = 0;
    FRAME_DT = dt;
    g_base_world = 1;
    CALL(0x00500760, grouplist_run_fn)((void *)A_GROUP_LIST, dt, 1, 1);   // CMechGroupList::Run
    CALL(0x00508220, objlist_fn)((void *)A_OBJECT_LIST);                 // CObjectList::Run
    CALL(0x00508110, objlist_fn)((void *)A_OBJECT_LIST);                 // PreMove
    CALL(0x0050d0f0, objlist_fn)((void *)A_OBJECT_LIST);                 // CheckCollision
    CALL(0x00508000, objlist_fn)((void *)A_OBJECT_LIST);                 // Move
    CALL(0x0050a7f0, pack_fn)((void *)A_OBJECT_LIST, 0);                 // Pack: dead objects freed
    FRAME_DT = keep_dt;
    if (G_PLAYER_GLIDER == pg) pg[OBJ_RUN_ON] = run_on, pg[OBJ_MOVE_ON] = move_on;
    coop_world_tick();               // the guests' gliders
    mirror_base_frame();             // the NPCs to the guests
    g_base_world = 0;
    if (!g_world_ran) mlog("coop: in a base with guests here - the world keeps running");
    g_world_ran = 1;
}

// the fly-out catch-up (see above)
static void THISCALL base_catch_up(void *groups, float t)
{
    if (g_world_ran && coop_world_gen() == g_base_gen) {
        mlog("coop: fly-out catch-up skipped (the world ran; hour %.3f)", *(float *)0x00676614);
        return;
    }
    CALL(0x00503a40, catchup_fn)(groups, t);
}

// every menu loop checks the device each frame: keep the session alive there
static char THISCALL menu_pump(void *app, int a)
{
    char r = CALL(A_DEVICE_CHECK, devcheck_fn)(app, a);
    if (g_flight_panel) coop_menu_keepalive();
    coop_show_version(G_APP && *(int *)(G_APP + APP_MODE) == 0);      // the start menu
    if (g_modal_pump) base_world_tick();
    static int last_mode = -1;
    if (G_APP && *(int *)(G_APP + APP_MODE) != last_mode) {
        int was = last_mode;
        last_mode = *(int *)(G_APP + APP_MODE);
        if (was == 1 && last_mode == 0 && net_mode() != NET_OFF) {   // back in the main menu: that session is over
            mlog("coop: back in the main menu, session closed");
            coop_leave();
        }
        mlog("coop: app mode %d (quit flag %d)", last_mode, *(int *)(G_APP + 0x44));
    }
    coop_menu_poll();
    // (no journal reads here: menu loops also run while a save loads, when the quest data is rebuilt)
    return r;
}

static char THISCALL modal_pump(void *app, int a)
{
    g_modal_pump = 1;
    char r = menu_pump(app, a);
    g_modal_pump = 0;
    return r;
}

// ---- commands ---------------------------------------------------------------------------------
int cg_command(const char *cmd, const char *arg, const char *line)
{
    int aim1_debug_cmd(const char *cmd, const char *arg);
    if (aim1_debug_cmd(cmd, arg)) return 1;
    if (!strcmp(cmd, "objstat")) { void aim1_objstat(void); aim1_objstat(); return 1; }
    if (!strcmp(cmd, "musicstat")) { void aim1_music_stat(void); aim1_music_stat(); return 1; }
    if (!strcmp(cmd, "testlight")) {
        float dx = 0, dy = 0, dz = -10, r = 120;
        sscanf(line, "%*s %f %f %f %f", &dx, &dy, &dz, &r);
        void d9light_test(float, float, float, float);
        d9light_test(dx, dy, dz, r);
        return 1;
    }
    if ((!strcmp(cmd, "qldump") || !strcmp(cmd, "qladd") || !strcmp(cmd, "qlflag") || !strcmp(cmd, "qlunflag") || !strcmp(cmd, "qljournal") || !strcmp(cmd, "qltimer")) && in_flight()) {
        questlog_test(cmd, arg);
    } else if (!strcmp(cmd, "selfdestruct") && in_flight()) {
        G_PLAYER_MECHANOID[0x74] = 1;              // dead pilot: CGlider::Run explodes the glider
    } else if ((!strcmp(cmd, "selfhit") || !strcmp(cmd, "playerhp")) && in_flight()) {
        void mirror_test_player(int hit, float dmg);
        mirror_test_player(!strcmp(cmd, "selfhit"), arg[0] ? (float)atof(arg) : 100.0f);
    } else if (!strcmp(cmd, "mystats") && in_flight()) {
        void stats_test(void);
        stats_test();
    } else if (!strcmp(cmd, "setstats") && in_flight()) {
        void stats_test_set(int rating);
        stats_test_set(atoi(arg));
    } else if (!strcmp(cmd, "mapnames") && in_flight()) {
        mapmark_test();
    } else if ((!strcmp(cmd, "mindstat") || !strcmp(cmd, "pickmind")) && in_flight()) {
        respawn_test(cmd);
    } else if (!strcmp(cmd, "spectcam")) {
        extern int g_spect_cam;
        g_spect_cam = atoi(arg);
    } else if (!strcmp(cmd, "face")) {
        extern float g_face;
        g_face = (float)atof(arg);
    } else if (!strcmp(cmd, "lootstat") && in_flight()) {
        loot_test();
    } else if ((!strcmp(cmd, "tonpc") || !strcmp(cmd, "killnpc") || !strcmp(cmd, "npchit")) && in_flight()) {
        void mirror_test_tonpc(float);
        extern char g_test_pilot[32];
        extern float g_test_dmg;
        g_test_dmg = !strcmp(cmd, "npchit") ? (float)atof(arg) : 0;
        if (!strcmp(cmd, "killnpc")) lstrcpynA(g_test_pilot, arg, 32);
        else sscanf(line, "%*s %*s %31s", g_test_pilot);       // "tonpc <dist> [pilot]"
        mirror_test_tonpc(!strcmp(cmd, "killnpc") ? -1.0f : !strcmp(cmd, "npchit") ? -0.6f : (float)atof(arg));
    } else if (!strcmp(cmd, "mirrorstat") && in_flight()) {
        void mirror_test_stat(void);
        mirror_test_stat();
    } else if (!strcmp(cmd, "npchp") && in_flight()) {
        void mirror_test_hp(int);
        mirror_test_hp(!strcmp(arg, "all"));
    } else if (!strcmp(cmd, "provoke") && in_flight()) {
        void mirror_test_provoke(void);
        mirror_test_provoke();
    } else if (!strcmp(cmd, "npcai") && in_flight()) {
        void mirror_test_ai(void);
        mirror_test_ai();
    } else if (!strcmp(cmd, "puppethit") && in_flight()) {
        void mirror_test_hit(float);
        mirror_test_hit((float)atof(arg));
    } else if (!strcmp(cmd, "npcs") && in_flight()) {
        // test: gliders near the player (model, position)
        const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
            uint8_t *o = (uint8_t *)nd[0];
            if (!o[OBJ_ALIVE] || *(int *)(o + OBJ_TYPE) != 0x0e) continue;
            const float *p = (const float *)(o + OBJ_POS);
            float dx = p[0] - pp[0], dy = p[1] - pp[1];
            if (dx * dx + dy * dy > 3000.0f * 3000.0f) continue;
            uint8_t *pmo = *(uint8_t **)(o + GLIDER_MECHANOID);
            mlog("npcs: %p %-16s %-14.32s %9.1f %9.1f %7.1f%s", o, (char *)o + OBJ_MODEL, pmo ? (char *)pmo : "?", p[0], p[1], p[2],
                 coop_is_remote(o) ? " (player)" : o == G_PLAYER_GLIDER ? " (me)" : "");
        }
    } else return 0;
    return 1;
}

// Radar: CMMApp radar code (0x43e650) colours a glider green when its group is the
// player's, else by relation. Remote players get their own colour: the load of
// the player pointer at 0x43ec8e (ebx = glider) jumps to a stub that asks us first.
static int __cdecl radar_is_player(const uint8_t *g) { return net_mode() != NET_OFF && coop_is_remote(g); }

static void radar_install(void)
{
    uint8_t *site = (uint8_t *)0x43ec8e;
    static const uint8_t expect[5] = {0xa1, 0x38, 0x7b, 0x5b, 0x00};   // mov eax, [0x5b7b38]
    if (memcmp(site, expect, 5)) { mlog("radar: unexpected code"); return; }
    uint8_t *s = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return;
    uint8_t *p = s;
    *p++ = 0x53;                                                        // push ebx
    *p++ = 0xe8; *(int32_t *)p = (int32_t)((uintptr_t)radar_is_player - (uintptr_t)(p + 4)); p += 4;
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04;                              // add esp, 4
    *p++ = 0x85; *p++ = 0xc0;                                           // test eax, eax
    *p++ = 0x74; *p++ = 0x0a;                                           // jz original
    *p++ = 0xb8; *(uint32_t *)p = coop_player_color(); p += 4;               // mov eax, colour
    *p++ = 0xe9; *(int32_t *)p = (int32_t)(0x43ecf5 - (uintptr_t)(p + 4)); p += 4;   // -> draw
    memcpy(p, expect, 5); p += 5;                                       // original instruction
    *p++ = 0xe9; *(int32_t *)p = (int32_t)(0x43ec93 - (uintptr_t)(p + 4)); p += 4;
    uint8_t jmp[5] = {0xe9};
    *(int32_t *)(jmp + 1) = (int32_t)((uintptr_t)s - (uintptr_t)(site + 5));
    if (patch_bytes((uintptr_t)site, expect, jmp, 5)) mlog("radar: players shown in %08lx", (unsigned long)coop_player_color());
}

void aim1_coop_install(void)
{
    coop_core_install();
    patch_call(0x00424496, frame);                  // CMMApp::RunGame -> CMMApp::Render
    patch_call(0x00423050, create_player_glider);   // CMMApp::InitMainMech
    patch_call(0x0042d676, modal_pump);             // menu loops: CBaseMenu::DoModal
    patch_call(0x004272fd, menu_pump);
    patch_call(0x00427326, menu_pump);
    patch_call(0x00420650, panel_modal);            // ProcessInput: hold / journal / map panel
    patch_call(0x004204e9, esc_menu);               // ProcessInput: ESC menu
    g_world_in_base = GetPrivateProfileIntA("Coop", "HostWorldInBase", 1, g_ini);
    patch_call(0x00423bd9, base_modal);             // CycleGame -> base screen: DoModal
    patch_call(0x00424624, base_catch_up);          // CycleGame: catch-up after a base visit
    radar_install();
    mirror_install();
    questlog_install();
    respawn_install();
    mapmark_install();
    killcredit_install();
    loot_install();
}
