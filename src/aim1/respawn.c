// A.I.M. co-op: spectator mode for a dead player, and respawn by the mechmind (#24).
//
// The game's death sequence lives in CMMApp::ProcessInput (0x4203a0): while the player's pilot is
// dead it runs the death camera (started once, app +0x144), which follows the object in
// 0x677e00 (the dropped mechmind) from just above it, and counts app +0xf4 down; at -22 s, or when
// ESC is pressed (key byte 0x5413ad), it leaves the game (0x677124 = 2): a black screen, then the
// main menu. In co-op, while a teammate is still up, that would end the session for this player.
// Here, before each ProcessInput of a dead player with a teammate up, the countdown is held,
// ESC is swallowed (TAB switches who to watch) and the camera target is that teammate's glider
// (their puppet; our own glider while they are in a base). With nobody up, the game's own death
// sequence runs as before.
//
// Respawn: a destroyed glider drops its pilot's mechmind (CGlider::Run 0x4f9b80 creates a CTheThing,
// object type 0x15, kind +0x278 = 0x101; 0x203 is the cargo container next to it). Every machine
// links the mechmind its copy of a dead teammate dropped; when the local player picks it up and
// enters a building (CBuildingObject::Run sets 0x677124 = 1, name at 0x677128), MSG_RESPAWN tells
// the dead player, who asks the host for the world again (MSG_WORLD_REQ) - the same path as a
// rejoin: the host's world with our own pilot from the profile, next to the host, alive.
#include "coop.h"
#include "../common/net.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define A_PROCESS_INPUT   0x004203a0
#define APP_DEATH_TIMER   0xf4                       // float, seconds, counts down from 0 to -22
#define DEATH_CAM_TARGET  (*(uint8_t **)0x00677e00)
#define KEYS              ((volatile int8_t *)0x005413ac)   // key state by DirectInput scan code (< 0 = down)
#define DIK_ESCAPE        0x01
#define DIK_TAB           0x0f
#define GLIDER_DYING      0x10d0
#define A_LEAVE_GAME      0x00677124                 // int: 2 = leave the game (death sequence over), 1 = enter a building
#define A_LEAVE_FLAG      0x00677120                 // byte set with it
#define A_ENTER_NAME      0x00677128                 // name32: the building being entered
#define TYPE_THING        0x15                       // CTheThing: things lying around to pick up
#define THING_KIND        0x278
#define KIND_MECHMIND     0x101
// the hold's goods (CGarbage at mechanoid +0x34, CGarbage::AddGoods 0x409740): +4 count, +8 entries of
// 0x2c (int kind, name32, +0x24 count); a picked-up mechmind is an entry named after its pilot
#define MO_GOODS          0x34
#define GOODS_SIZE        0x2c
#define A_MECHANOID_STATS 0x004ae820                 // thiscall(mechanoid): weight etc. after a hold change

enum { MSG_RESPAWN = 46 };                           // char dead[32], char building[32]

typedef void (THISCALL *process_input_fn)(uint8_t *);
static process_input_fn o_process_input;
static int g_watch;                                   // which teammate (index among the remote players)
static int g_spectating;
static int g_tab_was_down;
static int g_world_asked;                             // dead, the world was asked for again (respawn)
static char g_respawn_base[32];                       // the building our mechmind was brought to
static int g_respawn_gen;                             // world generation when it was
static DWORD g_respawn_seen;                          // in flight in the new world since
int aim1_enter_building(const char *name);

static int player_dead(void)
{
    uint8_t *mo = G_PLAYER_GLIDER ? *(uint8_t **)(G_PLAYER_GLIDER + GLIDER_MECHANOID) : NULL;
    return mo && (mo[0x74] || mo[0xac] || mo[0x154]);
}

// the teammate to watch: the g_watch-th living remote player, wrapping around
static uint8_t *watch_target(const char **name)
{
    uint8_t *alive[NET_MAX_PEERS];
    const char *names[NET_MAX_PEERS];
    int n = 0;
    uint8_t *p;
    const char *nm;
    for (int i = 0; n < NET_MAX_PEERS && (p = coop_remote_by_index(i, &nm)) != NULL; i++)
        if (p[OBJ_ALIVE] && !p[GLIDER_DYING]) { names[n] = nm; alive[n++] = p; }
    if (!n) return NULL;
    if (g_watch < 0) g_watch = 0;
    g_watch %= n;
    if (name) *name = names[g_watch];
    return alive[g_watch];
}

// ---- the dead teammates' mechminds --------------------------------------------------------------
typedef struct {
    int used, gen, carried;
    char name[32];
    float at[3];                  // where their glider went down
    DWORD t;
    uint8_t *thing;               // their mechmind here (checked against the object list before use)
    float pos[3];
} fallen;
static fallen g_fallen[NET_MAX_PEERS];

static int thing_present(const uint8_t *t)
{
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1])
        if (nd[0] == t) return t[OBJ_ALIVE] && *(const int *)(t + OBJ_TYPE) == TYPE_THING;
    return 0;
}

static float dist2(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static uint8_t *our_mechanoid(void)
{
    return G_PLAYER_GLIDER ? *(uint8_t **)(G_PLAYER_GLIDER + GLIDER_MECHANOID) : NULL;
}

// the n-th goods entry of our hold (NULL past the end)
static uint8_t *goods_entry(int i)
{
    uint8_t *mo = our_mechanoid();
    if (!mo) return NULL;
    int n = *(int *)(mo + MO_GOODS + 4);
    uint8_t *a = *(uint8_t **)(mo + MO_GOODS + 8);
    if (i < 0 || i >= n || n > 500 || !a) return NULL;
    return a + i * GOODS_SIZE;
}

static int g_mind_kind = 2;                           // the goods kind of a mechmind (checked again at a pickup)

// take a teammate's mechmind out of our hold (delivered, or they are back): 1 if there was one
static int remove_mechmind(const char *name)
{
    uint8_t *mo = our_mechanoid(), *e;
    for (int i = 0; (e = goods_entry(i)) != NULL; i++) {
        if (lstrcmpiA((const char *)e + 4, name) || (g_mind_kind != -1 && *(int *)e != g_mind_kind)) continue;
        int n = *(int *)(mo + MO_GOODS + 4);
        memmove(e, e + GOODS_SIZE, (size_t)(n - i - 1) * GOODS_SIZE);
        *(int *)(mo + MO_GOODS + 4) = n - 1;
        typedef void (THISCALL *stats_fn)(uint8_t *);
        CALL(A_MECHANOID_STATS, stats_fn)(mo);
        mlog("respawn: %s's mechmind taken out of our hold", name);
        return 1;
    }
    return 0;
}

// a teammate's puppet is going to explode here (mirror.c, MSG_PLAYER_DOWN)
void respawn_player_down(int peer, const char *name, const uint8_t *puppet)
{
    if (peer < 0 || peer >= NET_MAX_PEERS || !puppet) return;
    fallen *f = &g_fallen[peer];
    memset(f, 0, sizeof *f);
    f->used = 1;
    f->gen = coop_world_gen();
    lstrcpynA(f->name, name, sizeof f->name);
    memcpy(f->at, puppet + OBJ_POS, sizeof f->at);
    f->t = GetTickCount();
}

static void link_mechmind(fallen *f)
{
    uint8_t *best = NULL;
    float bd = 80.0f * 80.0f;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!o || !o[OBJ_ALIVE] || *(int *)(o + OBJ_TYPE) != TYPE_THING || *(int *)(o + THING_KIND) != KIND_MECHMIND) continue;
        int taken = 0;
        for (int i = 0; i < NET_MAX_PEERS; i++) taken |= g_fallen[i].used && g_fallen[i].thing == o;
        float d = dist2((float *)(o + OBJ_POS), f->at);
        // the dead player's own mechmind (its goods are named after the pilot): a hold full of
        // other mechminds drops with it, and the nearest one was often someone else's (#24)
        int own = *(int *)(o + 0x40c) > 0 && !lstrcmpiA((const char *)o + 0x27c + 4, f->name);
        if (own && !taken && d < 300.0f * 300.0f) { best = o; break; }
        if (!taken && d < bd) { bd = d; best = o; }
    }
    if (!best) return;
    f->thing = best;
    memcpy(f->pos, best + OBJ_POS, sizeof f->pos);
    mlog("respawn: %s's mechmind is lying at %.0f %.0f %.0f", f->name, f->pos[0], f->pos[1], f->pos[2]);
}

// in flight, every frame: follow the mechminds; the one that vanishes next to us was picked up by us
static void track_mechminds(void)
{
    static DWORD last;
    if (GetTickCount() - last < 200) return;
    last = GetTickCount();
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        fallen *f = &g_fallen[i];
        if (!f->used) continue;
        if (f->gen != coop_world_gen()) {                    // the world was rebuilt: that one is gone
            if (!f->carried) { memset(f, 0, sizeof *f); continue; }
            f->gen = coop_world_gen();
        }
        if (f->carried) continue;
        if (!f->thing) {
            if (GetTickCount() - f->t < 15000) link_mechmind(f);
            else {
                mlog("respawn: no mechmind of %s found here", f->name);
                memset(f, 0, sizeof *f);
            }
            continue;
        }
        if (thing_present(f->thing)) { memcpy(f->pos, f->thing + OBJ_POS, sizeof f->pos); continue; }
        f->thing = NULL;
        if (G_PLAYER_GLIDER && !player_dead() && dist2((float *)(G_PLAYER_GLIDER + OBJ_POS), f->pos) < 60.0f * 60.0f) {
            f->carried = 1;
            uint8_t *e;
            for (int k = 0; (e = goods_entry(k)) != NULL; k++)
                if (!lstrcmpiA((const char *)e + 4, f->name)) g_mind_kind = *(int *)e;
            mlog("respawn: we picked up %s's mechmind (goods kind %d)", f->name, g_mind_kind);
            char msg[160];
            snprintf(msg, sizeof msg, aim1_ui("mind_picked"), f->name);
            coop_notify(msg);
        } else {
            mlog("respawn: %s's mechmind is gone (not picked up by us)", f->name);
            memset(f, 0, sizeof *f);
        }
    }
}

// we are flying into a building: the mechminds we carry are delivered
static void deliver(void)
{
    char where[32];
    lstrcpynA(where, (const char *)A_ENTER_NAME, sizeof where);
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        fallen *f = &g_fallen[i];
        if (!f->used || !f->carried) continue;
        char m[64] = {0};
        lstrcpynA(m, f->name, 32);
        lstrcpynA(m + 32, where, 32);
        net_send(NET_ALL, MSG_RESPAWN, m, sizeof m, 1);
        mlog("respawn: %s's mechmind delivered to %s - told them", f->name, where);
        remove_mechmind(f->name);
        char msg[160];
        snprintf(msg, sizeof msg, aim1_ui("mind_delivered"), f->name);
        coop_notify(msg);
        memset(f, 0, sizeof *f);
    }
}

int respawn_handle(int type, int from, const uint8_t *data, int len)
{
    (void)from;
    if (type != MSG_RESPAWN) return 0;
    if (len != 64 || data[31] || data[63]) return 1;
    const char *dead = (const char *)data, *where = (const char *)data + 32;
    char msg[160];
    if (strcmp(dead, coop_my_name())) {                       // someone else's: they come back
        snprintf(msg, sizeof msg, aim1_ui("mind_delivered"), dead);
        coop_notify(msg);
        return 1;
    }
    if (!G_PLAYER_GLIDER || !player_dead()) { mlog("respawn: mechmind delivered to %s, but we are not down", where); return 1; }
    if (net_mode() != NET_CLIENT) {
        mlog("respawn: our mechmind was delivered to %s - the host's respawn is not built yet", where);
        coop_notify(aim1_ui("respawn_host"));
        return 1;
    }
    if (g_world_asked) return 1;
    g_world_asked = 1;
    // back in the world, we start in that building: flying out of it is the respawn
    lstrcpynA(g_respawn_base, where, sizeof g_respawn_base);
    g_respawn_gen = coop_world_gen();
    net_send(0, MSG_WORLD_REQ, NULL, 0, 1);
    mlog("respawn: our mechmind was delivered to %s - asked the host for the world", where);
    coop_notify(aim1_ui("respawning"));
    return 1;
}

// Mechminds of teammates who are up again (delivered by someone else, rejoined) stay in a hold for
// good, and drop with its cargo whenever the carrier is destroyed - the "second mechmind" (#24).
static void drop_stale_mechminds(void)
{
    static DWORD last;
    if (GetTickCount() - last < 2000 || g_mind_kind == -1) return;
    last = GetTickCount();
    uint8_t *e;
    for (int i = 0; (e = goods_entry(i)) != NULL; i++) {
        char name[33];
        lstrcpynA(name, (const char *)e + 4, sizeof name);
        if (*(int *)e == g_mind_kind && coop_player_up(name) && remove_mechmind(name)) {
            for (int k = 0; k < NET_MAX_PEERS; k++)
                if (g_fallen[k].used && !lstrcmpiA(g_fallen[k].name, name)) memset(&g_fallen[k], 0, sizeof g_fallen[k]);
            char msg[160];
            snprintf(msg, sizeof msg, aim1_ui("mind_returned"), name);
            coop_notify(msg);
            return;                                   // the list changed
        }
    }
}

void respawn_frame(void)
{
    if (net_mode() == NET_OFF) return;
    // The game leaves 0x677124 at 1 after a building visit (only the event byte 0x677120 is cleared
    // when it is handled): the next mechmind picked up looked delivered at once (#24). In flight, a 1
    // without the event byte is stale.
    if (*(int *)A_LEAVE_GAME == 1 && !*(uint8_t *)A_LEAVE_FLAG) *(int *)A_LEAVE_GAME = 0;
    // respawned in the host's world: into the base our mechmind was delivered to (a moment after the
    // world is up), so we come out of that base - not next to the host wherever they are
    if (g_respawn_base[0] && coop_world_gen() != g_respawn_gen && !player_dead()) {
        if (!g_respawn_seen) g_respawn_seen = GetTickCount();
        else if (GetTickCount() - g_respawn_seen > 1500) {
            int ok = aim1_enter_building(g_respawn_base);
            mlog("respawn: back in %s%s", g_respawn_base, ok ? "" : " - building not found, staying here");
            g_respawn_base[0] = 0;
            g_respawn_seen = 0;
        }
    }
    track_mechminds();
    drop_stale_mechminds();
}

void respawn_reset(void)
{
    memset(g_fallen, 0, sizeof g_fallen);
    g_world_asked = 0;
    g_respawn_base[0] = 0;
    g_respawn_seen = 0;
}

// ---- spectator mode ----------------------------------------------------------------------------
// The death branch of ProcessInput starts the camera once with CCamera "look at object" (0x41b2e0,
// mode 3 in 0x677e04, target at camera +0x2cc) on our own glider, then puts the camera just above
// 0x677e00 every frame - with the teammate there, the camera sat on them but kept looking at our
// wreck: "locked on the death spot". While spectating, the camera's target is the teammate too,
// and the camera orbits them: the mouse turns it, the wheel zooms.
#define G_CAMERA        (*(uint8_t **)0x00677dfc)
#define CAM_TARGET      0x2cc
#define CAM_POS         0x190                 // as the death branch writes it
#define A_CAM_LOOK_AT   0x0041b2e0            // thiscall(camera, object)

static float g_cam_yaw, g_cam_pitch = 0.35f, g_cam_dist = 60.0f;
int g_spect_cam = 1;                          // test: "spectcam 0" leaves the game's camera alone

static void spectator_camera(void)
{
    uint8_t *cam = G_CAMERA, *t = DEATH_CAM_TARGET;
    if (!g_spect_cam || !cam || !t || t == G_PLAYER_GLIDER) return;
    if (*(uint8_t **)(cam + CAM_TARGET) != t) {
        typedef void (THISCALL *look_fn)(uint8_t *, uint8_t *);
        CALL(A_CAM_LOOK_AT, look_fn)(cam, t);
        const float *m = (const float *)(t + OBJ_MATRIX);
        g_cam_yaw = atan2f(-m[6], -m[2]);                    // start behind them (third axis = forward)
        // the death branch darkened the screen (0x497cd0 / 0x497df0: a darker gamma ramp, or with the
        // gamma faded by the renderer thread, fade mode 2); 0x497c00 brings the picture back (fade mode 1)
        typedef void (__cdecl *v_fn)(void);
        CALL(0x00497c00, v_fn)();
        mlog("respawn: camera on %s", coop_remote_name(t) ? coop_remote_name(t) : "?");
    }
    // The dead player's game doesn't use the mouse: we read it ourselves (the gamepad's right stick
    // comes through the same read)
    int mdx, mdy, mdz;
    int devtools_read_mouse(int *, int *, int *);
    devtools_read_mouse(&mdx, &mdy, &mdz);
    g_cam_yaw -= mdx * 0.006f;
    g_cam_pitch += mdy * 0.004f;
    if (g_cam_pitch < -0.2f) g_cam_pitch = -0.2f;
    if (g_cam_pitch > 1.3f) g_cam_pitch = 1.3f;
    g_cam_dist -= mdz * 0.05f;
    if (g_cam_dist < 25.0f) g_cam_dist = 25.0f;
    if (g_cam_dist > 250.0f) g_cam_dist = 250.0f;
    const float *p = (const float *)(t + OBJ_POS);
    float *c = (float *)(cam + CAM_POS);
    float h = cosf(g_cam_pitch) * g_cam_dist;
    c[0] = p[0] + cosf(g_cam_yaw) * h;
    c[1] = p[1] + sinf(g_cam_yaw) * h;
    c[2] = p[2] + sinf(g_cam_pitch) * g_cam_dist;
    static DWORD dbg;
    static int sx;
    sx += mdx;
    if (g_spect_cam > 1 && GetTickCount() - dbg > 500) {
        dbg = GetTickCount();
        const float *c0 = (const float *)(cam + 0x180);
        mlog("spectcam: mouse %d yaw %.2f cam %.0f %.0f %.0f (+0x180 %.0f %.0f %.0f) target %.0f %.0f %.0f", sx, g_cam_yaw,
             c[0], c[1], c[2], c0[0], c0[1], c0[2], p[0], p[1], p[2]);
        sx = 0;
    }
}

static void THISCALL h_process_input(uint8_t *app)
{
    if (net_mode() != NET_OFF && G_PLAYER_GLIDER && *(int *)(app + APP_MODE) != 0 && !player_dead() &&
        *(int *)A_LEAVE_GAME == 1)
        deliver();
    if (net_mode() != NET_OFF && G_PLAYER_GLIDER && *(int *)(app + APP_MODE) != 0 && player_dead()) {
        int tab = KEYS[DIK_TAB] < 0;
        if (tab && !g_tab_was_down) g_watch++;        // next teammate
        g_tab_was_down = tab;
        const char *name = NULL;
        uint8_t *t = watch_target(&name);
        // a teammate in a base or a menu has no glider here, but is still up (and may bring us back)
        if (t || coop_teammates_up() > 0) {
            float *timer = (float *)(app + APP_DEATH_TIMER);
            if (*timer < -15.0f) *timer = -15.0f;     // never reaches -22: no leaving the game
            KEYS[DIK_ESCAPE] = 0;                     // ESC would leave it at once
            DEATH_CAM_TARGET = t ? t : G_PLAYER_GLIDER;
            if (!g_spectating) {
                g_spectating = 1;
                mlog("respawn: we are down - watching %s", name ? name : "(teammate not in flight)");
                char msg[128];
                snprintf(msg, sizeof msg, aim1_ui("spectating"), name ? name : "?");
                coop_notify(msg);
                coop_notify(aim1_ui("down_hint"));
            }
        } else if (g_spectating) {
            g_spectating = 0;
            DEATH_CAM_TARGET = NULL;                  // it may be a puppet that is gone now
            mlog("respawn: nobody left alive - the game's death sequence goes on");
            coop_notify(aim1_ui("all_down"));
        }
    } else if (g_spectating) {
        g_spectating = 0;
        g_world_asked = 0;
        DEATH_CAM_TARGET = NULL;
        mlog("respawn: spectating over%s", G_PLAYER_GLIDER && !player_dead() ? " - we are back" : "");
    }
    // ProcessInput reads the keys itself: an ESC can still make its death branch leave the game
    // (0x677124 = 2, 0x677120 = 1). While spectating, that is undone.
    int leave = *(int *)A_LEAVE_GAME, flag = *(uint8_t *)A_LEAVE_FLAG;
    o_process_input(app);
    if (g_spectating) spectator_camera();
    if (g_spectating && *(int *)A_LEAVE_GAME == 2 && leave != 2) {
        *(int *)A_LEAVE_GAME = leave;
        *(uint8_t *)A_LEAVE_FLAG = (uint8_t)flag;
        static int nlog;
        if (nlog++ < 5) mlog("respawn: leaving the game ignored while spectating");
    }
}

int respawn_spectating(void) { return g_spectating; }

// harness: "mindstat" - the mechminds lying around; "pickmind" - put our glider on the nearest one
void respawn_test(const char *cmd)
{
    uint8_t *best = NULL;
    float bd = 1e30f;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!o || !o[OBJ_ALIVE] || *(int *)(o + OBJ_TYPE) != TYPE_THING) continue;
        float *p = (float *)(o + OBJ_POS);
        float d = G_PLAYER_GLIDER ? dist2(p, (float *)(G_PLAYER_GLIDER + OBJ_POS)) : 0;
        mlog("mindstat: thing %p kind %x at %.0f %.0f %.0f (%.0f away)", o, *(int *)(o + THING_KIND), p[0], p[1], p[2],
             d > 0 ? (double)sqrtf(d) : 0.0);
        int own = 0;                               // a fallen teammate's own mechmind first
        for (int i = 0; i < NET_MAX_PEERS; i++) own |= g_fallen[i].used && g_fallen[i].thing == o;
        if (*(int *)(o + THING_KIND) == KIND_MECHMIND && (own || d < bd)) { bd = own ? -1.0f : d; best = o; }
    }
    uint8_t *e;
    for (int i = 0; (e = goods_entry(i)) != NULL; i++)
        mlog("mindstat: hold goods %d: kind %d '%.32s' x%d", i, *(int *)e, (const char *)e + 4, *(int *)(e + 0x24));
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_fallen[i].used)
            mlog("mindstat: fallen %s: thing %p carried %d", g_fallen[i].name, g_fallen[i].thing, g_fallen[i].carried);
    if (!strcmp(cmd, "pickmind") && best && G_PLAYER_GLIDER) {
        float *p = (float *)(best + OBJ_POS);
        typedef void (THISCALL *setpos_fn)(void *, float, float, float);
        VFN(G_PLAYER_GLIDER, VF_OBJ_SETPOS, setpos_fn)(G_PLAYER_GLIDER, p[0], p[1], p[2] + 2.0f);
        mlog("pickmind: moved onto %p", best);
    }
}

void respawn_install(void)
{
    static const uint8_t pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};   // push ebp; mov ebp, esp; push -1
    o_process_input = (process_input_fn)hook_jmp(A_PROCESS_INPUT, pro, 5, (void *)h_process_input);
    mlog("respawn: spectator mode %s", o_process_input ? "installed" : "NOT installed (unexpected code)");
}
