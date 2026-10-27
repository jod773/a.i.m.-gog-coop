// Host-authoritative NPC gliders (co-op phase 2).
//
// The host sends every NPC glider of its location: a roster entry (pilot
// name, clan, mechanoid) when it first appears, its state ten times a
// second, and a "gone" message when it disappears (landed, destroyed).
// A joining player stops its own NPC simulation (glider AI, group AI, world
// update) and shows the host's NPCs instead, matched by pilot name.
#include "engine.h"
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CALL(addr, T) ((T)(addr))
#define MAXNPC 512
#define OBJ_TYPE      0x54
#define TYPE_GLIDER   0x0e
#define OBJLIST_HEAD  A_OBJLIST_HEAD

enum { MSG_NPC_SPAWN = 32, MSG_NPC_GONE = 33, MSG_NPC_STATES = 34, MSG_NPC_ROSTER_END = 35 };

#pragma pack(push, 1)
typedef struct { uint16_t id; char name[32]; char org[32]; } spawn_hdr;
typedef struct { uint16_t id; float pos[3]; float r[9]; float vel[3]; } npc_state;
#pragma pack(pop)

// ---- shared ------------------------------------------------------------------------------
typedef struct {
    int used;
    char name[32], org[32];
    uint8_t *glider;
    int gen;
    // host
    int present;
    // client
    uint8_t *blob;
    int blob_len;
    snapshot snap[4];
    int nsnap;
    DWORD next_lookup;
    int gone;             // remove at the next frame (received in a menu)
} npc;

static npc g_npc[MAXNPC];
static int g_roster_done, g_cleanup_gen = -1;
static DWORD g_last_scan;

static const char *glider_pilot(uint8_t *g)
{
    uint8_t *mech = *(uint8_t **)(g + GLIDER_MECH);
    if (!mech || !mech[MECH_NAME]) return NULL;
    return (const char *)mech + MECH_NAME;
}

static int is_npc_glider(uint8_t *o)
{
    return o[OBJ_ALIVE] && *(int *)(o + OBJ_TYPE) == TYPE_GLIDER && o != G_PLAYER_GLIDER &&
           !coop_is_remote(o) && glider_pilot(o);
}

#define FOR_EACH_OBJECT(o) \
    for (void **node_ = *(void ***)OBJLIST_HEAD; node_; node_ = (void **)node_[1]) \
        for (uint8_t *o = (uint8_t *)node_[0]; o; o = NULL)

void mirror_reset(void)
{
    for (int i = 0; i < MAXNPC; i++) free(g_npc[i].blob);
    memset(g_npc, 0, sizeof g_npc);
    g_roster_done = 0;
    g_cleanup_gen = -1;
}

// ---- host --------------------------------------------------------------------------------
static int host_id(const char *name)
{
    int free_slot = -1;
    for (int i = 0; i < MAXNPC; i++) {
        if (g_npc[i].used && !strcmp(g_npc[i].name, name)) return i;
        if (!g_npc[i].used && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

static void send_spawn(int to, int id)
{
    npc *n = &g_npc[id];
    uint8_t *mech = *(uint8_t **)(n->glider + GLIDER_MECH);
    uint8_t *mechanoid = *(uint8_t **)(mech + MECH_MECHANOID);
    if (!mechanoid) return;
    int len;
    uint8_t *blob = coop_mechanoid_save(mechanoid, &len);
    uint8_t *buf = malloc(sizeof(spawn_hdr) + len);
    spawn_hdr *h = (spawn_hdr *)buf;
    memset(h, 0, sizeof *h);
    h->id = (uint16_t)id;
    lstrcpynA(h->name, n->name, 32);
    lstrcpynA(h->org, n->org, 32);
    memcpy(buf + sizeof *h, blob, len);
    net_send(to, MSG_NPC_SPAWN, buf, sizeof *h + len, 1);
    free(buf);
    free(blob);
}

static void host_scan(void)
{
    DWORD now = GetTickCount();
    if (now - g_last_scan < 100) return;
    g_last_scan = now;
    int gen = coop_world_gen();
    for (int i = 0; i < MAXNPC; i++) g_npc[i].present = 0;
    uint8_t pkt[2 + 20 * sizeof(npc_state)];
    int n = 0;
    FOR_EACH_OBJECT(o) {
        if (!is_npc_glider(o)) continue;
        const char *name = glider_pilot(o);
        int id = host_id(name);
        if (id < 0) continue;
        npc *e = &g_npc[id];
        int fresh = !e->used || e->glider != o || e->gen != gen;
        if (!e->used) {
            e->used = 1;
            lstrcpynA(e->name, name, 32);
        }
        e->glider = o;
        e->gen = gen;
        e->present = 1;
        if (fresh) {
            uint8_t *mech = *(uint8_t **)(o + GLIDER_MECH);
            lstrcpynA(e->org, (char *)mech + MECH_ORGNAME, 32);
            send_spawn(NET_ALL, id);
        }
        npc_state *s = (npc_state *)(pkt + 2) + n;
        s->id = (uint16_t)id;
        const float *m = (const float *)(o + OBJ_MATRIX);
        memcpy(s->pos, m + 12, 12);
        memcpy(s->r, m, 12);
        memcpy(s->r + 3, m + 4, 12);
        memcpy(s->r + 6, m + 8, 12);
        memcpy(s->vel, o + GLIDER_VELOCITY, 12);
        if (++n == 20) {
            *(uint16_t *)pkt = (uint16_t)n;
            net_send(NET_ALL, MSG_NPC_STATES, pkt, 2 + n * sizeof(npc_state), 0);
            n = 0;
        }
    }
    if (n) {
        *(uint16_t *)pkt = (uint16_t)n;
        net_send(NET_ALL, MSG_NPC_STATES, pkt, 2 + n * sizeof(npc_state), 0);
    }
    for (int i = 0; i < MAXNPC; i++) {
        if (g_npc[i].used && !g_npc[i].present) {
            uint16_t id = (uint16_t)i;
            net_send(NET_ALL, MSG_NPC_GONE, &id, 2, 1);
            memset(&g_npc[i], 0, sizeof g_npc[i]);
        }
    }
}

// the object list still holds g (saving the game packs the list: dead gliders are freed)
static int alive_object(const uint8_t *g)
{
    for (void **nd = *(void ***)A_OBJLIST_HEAD; nd; nd = (void **)nd[1])
        if (nd[0] == g) return g[OBJ_ALIVE];
    return 0;
}

void mirror_peer_joined(int peer)
{
    if (!coop_in_flight()) return;
    for (int i = 0; i < MAXNPC; i++)
        if (g_npc[i].used && g_npc[i].glider && g_npc[i].gen == coop_world_gen() && alive_object(g_npc[i].glider)) send_spawn(peer, i);
    net_send(peer, MSG_NPC_ROSTER_END, NULL, 0, 1);
}

// ---- client ------------------------------------------------------------------------------
static int g_client;       // NPC simulation is off

static uint8_t *find_glider(const char *name)
{
    FOR_EACH_OBJECT(o) {
        if (!is_npc_glider(o)) continue;
        if (!strcmp(glider_pilot(o), name)) return o;
    }
    return NULL;
}

static int in_roster(const char *name)
{
    for (int i = 0; i < MAXNPC; i++)
        if (g_npc[i].used && !strcmp(g_npc[i].name, name)) return 1;
    return 0;
}

static void client_frame(void)
{
    int gen = coop_world_gen();
    DWORD now = GetTickCount();
    // the snapshot we loaded may contain NPC gliders the host no longer has
    if (g_roster_done && g_cleanup_gen != gen) {
        g_cleanup_gen = gen;
        int removed = 0;
        FOR_EACH_OBJECT(o) {
            if (is_npc_glider(o) && !in_roster(glider_pilot(o))) { coop_remove_glider(o); removed++; }
        }
        mlog("mirror: removed %d local NPC gliders not on the host", removed);
    }
    for (int i = 0; i < MAXNPC; i++) {
        npc *e = &g_npc[i];
        if (!e->used) continue;
        if (e->glider && e->gen != gen) e->glider = NULL;
        if (e->gone) {
            if (e->glider && e->glider[OBJ_ALIVE]) coop_remove_glider(e->glider);
            free(e->blob);
            memset(e, 0, sizeof *e);
            continue;
        }
        if (e->glider && !e->glider[OBJ_ALIVE]) e->glider = NULL;
        float m[16], vel[3];
        if (!coop_sample(e->snap, e->nsnap, m, vel)) continue;
        if (!e->glider && (int)(now - e->next_lookup) >= 0) {
            e->next_lookup = now + 1000;
            e->glider = find_glider(e->name);
            if (!e->glider && e->blob)
                e->glider = coop_spawn_glider(coop_mechanoid_load(e->blob, e->blob_len), e->name, e->org, m);
            e->gen = gen;
        }
        if (e->glider) coop_drive(e->glider, m, vel);
    }
}

int mirror_handle_combat(int type, const uint8_t *data, int len);
int mirror_handle_time(int type, const uint8_t *data, int len);

int mirror_handle(int type, int from, const uint8_t *data, int len)
{
    (void)from;
    if (mirror_handle_combat(type, data, len) || mirror_handle_time(type, data, len)) return 1;
    switch (type) {
    case MSG_NPC_SPAWN: {
        if (len < (int)sizeof(spawn_hdr)) return 1;
        const spawn_hdr *h = (const spawn_hdr *)data;
        if (h->id >= MAXNPC) return 1;
        npc *e = &g_npc[h->id];
        if (e->used && strncmp(e->name, h->name, 32)) {            // id reused for another pilot
            if (!g_coop_menu && e->glider && e->gen == coop_world_gen() && e->glider[OBJ_ALIVE]) coop_remove_glider(e->glider);
            free(e->blob);
            memset(e, 0, sizeof *e);
        }
        e->used = 1;
        e->gone = 0;
        memcpy(e->name, h->name, 31);
        memcpy(e->org, h->org, 31);
        free(e->blob);
        e->blob_len = len - sizeof *h;
        e->blob = malloc(e->blob_len);
        memcpy(e->blob, data + sizeof *h, e->blob_len);
        e->next_lookup = 0;
        return 1;
    }
    case MSG_NPC_GONE:
        if (len >= 2) {
            uint16_t id = *(const uint16_t *)data;
            if (id < MAXNPC && g_npc[id].used) {
                npc *e = &g_npc[id];
                if (g_coop_menu) { e->gone = 1; return 1; }
                if (e->glider && e->gen == coop_world_gen() && e->glider[OBJ_ALIVE]) coop_remove_glider(e->glider);
                free(e->blob);
                memset(e, 0, sizeof *e);
            }
        }
        return 1;
    case MSG_NPC_ROSTER_END:
        g_roster_done = 1;
        g_cleanup_gen = -1;
        mlog("mirror: roster received");
        return 1;
    case MSG_NPC_STATES: {
        if (len < 2) return 1;
        int n = *(const uint16_t *)data;
        if (len < 2 + n * (int)sizeof(npc_state)) return 1;
        DWORD now = GetTickCount();
        const npc_state *s = (const npc_state *)(data + 2);
        for (int i = 0; i < n; i++, s++) {
            if (s->id >= MAXNPC || !g_npc[s->id].used) continue;
            npc *e = &g_npc[s->id];
            memmove(&e->snap[1], &e->snap[0], 3 * sizeof(snapshot));
            snapshot *p = &e->snap[0];
            p->t = now;
            memset(p->m, 0, sizeof p->m);
            memcpy(p->m, s->r, 12);
            memcpy(p->m + 4, s->r + 3, 12);
            memcpy(p->m + 8, s->r + 6, 12);
            memcpy(p->m + 12, s->pos, 12);
            p->m[15] = 1.0f;
            memcpy(p->vel, s->vel, 12);
            if (e->nsnap < 4) e->nsnap++;
        }
        return 1;
    }
    }
    return 0;
}

// The world clock (CWorld + 4, game seconds) runs in CWorld::RunWorld, which
// joining players skip: the host sends it once a second.
enum { MSG_WORLD_TIME = 38 };
#define WORLD_TIME 4

void mirror_frame(void)
{
    g_client = net_mode() == NET_CLIENT;
    if (!coop_in_flight()) return;
    if (net_mode() == NET_HOST) {
        host_scan();
        static DWORD last;
        if (GetTickCount() - last >= 1000 && G_WORLD) {
            last = GetTickCount();
            float t = *(float *)(G_WORLD + WORLD_TIME);
            net_send(NET_ALL, MSG_WORLD_TIME, &t, 4, 0);
        }
    } else if (g_client) {
        client_frame();
    }
}

int mirror_handle_time(int type, const uint8_t *data, int len)
{
    if (type != MSG_WORLD_TIME) return 0;
    if (len == 4 && G_WORLD && coop_in_flight() && !g_coop_menu) *(float *)(G_WORLD + WORLD_TIME) = *(const float *)data;
    return 1;
}

// ---- combat -------------------------------------------------------------------------------
// Every shot is a call of the weapon's fire function (only made when the
// weapon is ready). Shots by the local player (and, on the host, by NPCs)
// are sent and replayed by the same glider on the other side; on the host a
// joining player's replayed shots do the real damage to NPCs.
// Damage rules: NPCs on joining players and remote players' gliders on the
// host are not damaged locally (the host decides NPC deaths; every player's
// own glider takes the hits it sees).
enum { MSG_FIRE = 36, MSG_NPC_BOOM = 37 };

typedef struct { float mtx[16]; float x, y, z; void *target; float k; } fire_tail;
#pragma pack(push, 1)
typedef struct { char shooter[32]; char target[32]; uint8_t slot; float m0[16]; fire_tail t; } fire_msg;
#pragma pack(pop)

#define WEAPON0      0x0c50
#define WEAPON_SIZE  0x148
#define WEAPON_OWNER 0x118
#define WEAPON_DATA  0x114      // the gun's weapon data; NULL = empty slot (the game's fire skips it)

typedef void (THISCALL *fire_fn)(void *, float *, fire_tail);
typedef void (THISCALL *damage_fn)(void *, void *, float, float, float, int, void *, int, float);
typedef void (THISCALL *boom_fn)(void *);
static fire_fn o_fire;
static damage_fn o_damage;
static boom_fn o_boom;
static int g_replaying;

static int npc_id_of(uint8_t *g)
{
    for (int i = 0; i < MAXNPC; i++)
        if (g_npc[i].used && g_npc[i].glider == g && g_npc[i].gen == coop_world_gen()) return i;
    return -1;
}

const char *coop_remote_name(const uint8_t *g);    // coop.c
uint8_t *coop_remote_glider(const char *name);
const char *coop_my_name(void);

// "@player" for players, the pilot name for NPCs
static int glider_ref(uint8_t *g, char out[32])
{
    memset(out, 0, 32);
    if (!g) return 0;
    if (g == G_PLAYER_GLIDER) { out[0] = '@'; lstrcpynA(out + 1, coop_my_name(), 31); return 1; }
    const char *rn = coop_remote_name(g);
    if (rn) { out[0] = '@'; lstrcpynA(out + 1, rn, 31); return 1; }
    if (*(int *)(g + OBJ_TYPE) == TYPE_GLIDER && glider_pilot(g)) { lstrcpynA(out, glider_pilot(g), 32); return 1; }
    return 0;
}

static uint8_t *resolve_ref(const char *ref)
{
    if (!ref[0]) return NULL;
    if (ref[0] == '@') {
        if (!strcmp(ref + 1, coop_my_name())) return G_PLAYER_GLIDER;
        return coop_remote_glider(ref + 1);
    }
    for (int i = 0; i < MAXNPC; i++)
        if (g_npc[i].used && !strcmp(g_npc[i].name, ref) && g_npc[i].glider && g_npc[i].gen == coop_world_gen())
            return g_npc[i].glider;
    return find_glider(ref);
}

static void THISCALL h_fire(uint8_t *w, float *m0, fire_tail t)
{
    o_fire(w, m0, t);
    if (g_replaying || net_mode() == NET_OFF) return;
    uint8_t *g = *(uint8_t **)(w + WEAPON_OWNER);
    if (!g) return;
    int slot = (int)(w - g - WEAPON0) / WEAPON_SIZE;
    if (slot < 0 || slot > 3 || g + WEAPON0 + slot * WEAPON_SIZE != w) return;
    int mine = g == G_PLAYER_GLIDER;
    int host_npc = net_mode() == NET_HOST && is_npc_glider(g);
    if (!mine && !host_npc) return;
    fire_msg f;
    memset(&f, 0, sizeof f);
    if (!glider_ref(g, f.shooter)) return;
    glider_ref((uint8_t *)t.target, f.target);
    f.slot = (uint8_t)slot;
    memcpy(f.m0, m0, sizeof f.m0);
    f.t = t;
    f.t.target = NULL;
    net_send(NET_ALL, MSG_FIRE, &f, sizeof f, 1);
}

static void replay_fire(const fire_msg *f)
{
    uint8_t *g = resolve_ref(f->shooter);
    if (!g || !g[OBJ_ALIVE] || f->slot > 3) return;
    uint8_t *w = g + WEAPON0 + f->slot * WEAPON_SIZE;
    if (!*(void **)(w + WEAPON_DATA)) {                  // our copy of that glider has no gun there (#7)
        // fire its first loaded gun instead, so the shot is still seen (#15); none -> skip
        int s, guns = 0, alt = -1;
        for (s = 0; s < 4; s++)
            if (*(void **)(g + WEAPON0 + s * WEAPON_SIZE + WEAPON_DATA)) { guns |= 1 << s; if (alt < 0) alt = s; }
        static DWORD last;                               // every time, at most once a second (finds the cause)
        static int nskip;
        nskip++;
        if (GetTickCount() - last >= 1000) {
            last = GetTickCount();
            mlog("mirror: shot by %s: slot %d is empty here, %s (glider %p, %s, guns %x, %d so far)",
                 f->shooter, f->slot, alt < 0 ? "skipped" : "fired the first loaded slot instead", g,
                 f->shooter[0] == '@' ? (coop_is_remote(g) ? "player puppet" : "player, NOT a puppet")
                                      : (find_glider(f->shooter) == g ? "NPC by pilot" : "NPC roster"),
                 guns, nskip);
        }
        if (alt < 0) return;
        w = g + WEAPON0 + alt * WEAPON_SIZE;
    }
    float m0[16];
    memcpy(m0, f->m0, sizeof m0);
    fire_tail t = f->t;
    t.target = resolve_ref(f->target);
    g_replaying = 1;
    o_fire(w, m0, t);
    g_replaying = 0;
    static int nlog;
    if (nlog++ < 5) mlog("mirror: shot by %s (slot %d, target '%s')", f->shooter, f->slot, f->target);
}

static int protected_glider(uint8_t *g)
{
    if (net_mode() == NET_HOST) return coop_is_remote(g);         // a joining player's glider
    if (net_mode() == NET_CLIENT) return g != G_PLAYER_GLIDER && !coop_is_remote(g) &&
                                         *(int *)(g + OBJ_TYPE) == TYPE_GLIDER;   // host's NPCs
    return 0;
}

static void THISCALL h_damage(void *self, void *a1, float a2, float a3, float a4, int a5, void *a6, int a7, float a8)
{
    if (net_mode() != NET_OFF && protected_glider(self)) return;
    if (net_mode() != NET_OFF && self == G_PLAYER_GLIDER && a6) {
        uint8_t *att = (uint8_t *)a6;
        if (*(int *)(att + OBJ_TYPE) == 0x10 && *(uint8_t **)(att + 0x10)) att = *(uint8_t **)(att + 0x10);   // projectile -> shooter
        if (coop_glider_same_clan(att)) return;      // no friendly fire
    }
    o_damage(self, a1, a2, a3, a4, a5, a6, a7, a8);
}

static void THISCALL h_boom(uint8_t *self)
{
    if (net_mode() == NET_CLIENT && !g_replaying && protected_glider(self)) return;
    if (net_mode() == NET_HOST) {
        int id = npc_id_of(self);
        if (id >= 0) {
            uint16_t i16 = (uint16_t)id;
            net_send(NET_ALL, MSG_NPC_BOOM, &i16, 2, 1);
        }
    }
    o_boom(self);
}

static void replay_boom(int id)
{
    if (id >= MAXNPC || !g_npc[id].used) return;
    uint8_t *g = g_npc[id].glider;
    if (!g || g_npc[id].gen != coop_world_gen() || !g[OBJ_ALIVE]) return;
    g_replaying = 1;
    o_boom(g);
    g_replaying = 0;
    g_npc[id].glider = NULL;
}

int mirror_handle_combat(int type, const uint8_t *data, int len)
{
    if (type == MSG_FIRE && len == sizeof(fire_msg)) {
        if (coop_in_flight() && !g_coop_menu) replay_fire((const fire_msg *)data);
        return 1;
    }
    if (type == MSG_NPC_BOOM && len >= 2) {
        if (coop_in_flight() && !g_coop_menu) replay_boom(*(const uint16_t *)data);
        return 1;
    }
    return 0;
}

// ---- NPC simulation off on joining players ------------------------------------------------------
typedef void (THISCALL *run0_fn)(void *);
typedef void (THISCALL *run1_fn)(void *, int);
static run0_fn o_ai_run;
static run1_fn o_group_run, o_world_run;

static void THISCALL h_ai_run(void *self) { if (!g_client) o_ai_run(self); }
static void THISCALL h_group_run(void *self, int a) { if (!g_client) o_group_run(self, a); }
static void THISCALL h_world_run(void *self, int a) { if (!g_client) o_world_run(self, a); }

void mirror_install(void)
{
    static const uint8_t pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};   // push ebp; mov ebp, esp; push -1
    o_ai_run = (run0_fn)hook_jmp(A_AI_RUN, pro, 5, h_ai_run);        // CBasicAI::Run
    o_group_run = (run1_fn)hook_jmp(A_GROUP_RUN, pro, 5, h_group_run);  // CMechGroup2::Run
    o_world_run = (run1_fn)hook_jmp(A_WORLD_RUN, pro, 5, h_world_run);  // CWorld::RunWorld
    o_fire = (fire_fn)hook_jmp(A_WEAPON_FIRE, pro, 5, h_fire);            // weapon fire
    o_damage = (damage_fn)hook_jmp(A_GLIDER_DAMAGE, pro, 5, h_damage);      // CGliderObject::Damage
    o_boom = (boom_fn)hook_jmp(A_GLIDER_BOOM, pro, 5, h_boom);            // CGliderObject::Boom
    mlog("mirror: installed (%p %p %p)", (void *)o_ai_run, (void *)o_group_run, (void *)o_world_run);
}
