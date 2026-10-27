// A.I.M. co-op: host-authoritative NPC gliders, shots, deaths and world time
// (port of the A.I.M. 2 module).
//
// The host sends every NPC glider of its location: a roster entry (pilot
// id, group header, mechanoid) when it first appears, its state ten times a
// second, "gone" when it disappears and "boom" when it is destroyed. A
// joining player stops its own NPC simulation (group AI, glider AI, the
// hourly organisation update that spawns groups) and shows the host's NPCs,
// matched by pilot id (the mechanoid's name, e.g. "MINVA-1_2").
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define MAXNPC 512
#define TYPE_GLIDER 0x0e
#define GLIDER_DYING 0x10d0        // set by CGlider::Run when the pilot is dead
#define GROUP_HDR 0x0c             // display name, group name, type, organisation
#define GROUP_HDR_LEN 0x80

enum { MSG_NPC_SPAWN = 32, MSG_NPC_GONE = 33, MSG_NPC_STATES = 34, MSG_NPC_ROSTER_END = 35,
       MSG_FIRE = 36, MSG_NPC_BOOM = 37, MSG_WORLD_TIME = 38, MSG_PLAYER_DOWN = 39, MSG_NPC_HEALTH = 42,
       MSG_PLAYER_HEALTH = 47 /* char name[32], float v[5] */ };

#pragma pack(push, 1)
typedef struct { uint16_t id; char name[32]; uint8_t grp[GROUP_HDR_LEN]; } spawn_hdr;
typedef struct { uint16_t id; float pos[3]; float r[9]; float vel[3]; } npc_state;
typedef struct { uint16_t id; float v[5]; } npc_health;
#pragma pack(pop)

// A pilot's health (#22): shield +0x15c, armour +0x160 (both used up first by
// CMechanoid's damage 0x4ad8a0), then the damage (+0x24) of its three parts - reactor +0x48,
// drive +0x80, glider body +0x128 (vf 8 adds damage, vf 0xc = damage / maximum, +0x2d "damaged";
// +0x2c "destroyed" is the death byte, left to MSG_NPC_BOOM).
static const int k_health_off[5] = {0x15c, 0x160, 0x48 + 0x24, 0x80 + 0x24, 0x128 + 0x24};

typedef struct {
    int used;
    char name[32];
    uint8_t grp[GROUP_HDR_LEN];
    uint8_t *glider;
    int gen;
    // host
    int present, boomed;
    float hp[5];          // health as last sent
    int hp_sent;
    // client
    uint8_t *blob;
    int blob_len;
    snapshot snap[4];
    int nsnap;
    DWORD next_lookup;
    int gone;             // remove at the next frame (received in a menu)
    int spawned;          // glider is our puppet (spawn_npc), not one of the world's own NPCs
} npc;

static npc g_npc[MAXNPC];
typedef struct { char name[32]; float v[5]; int valid; } player_hp;   // the other players' health
static player_hp g_php[NET_MAX_PEERS];
static int g_roster_done;
static DWORD g_last_scan, g_last_cleanup;
static int g_client;       // NPC simulation is off

static uint8_t *glider_mechanoid(uint8_t *g) { return *(uint8_t **)(g + GLIDER_MECHANOID); }

// The pilot id, or NULL when the glider has no (valid) mechanoid: some
// gliders of a loaded world keep a stale pointer, so the mechanoid must point
// back at its glider.
static const char *glider_pilot(uint8_t *g)
{
    uint8_t *mo = glider_mechanoid(g);
    if (!mo || IsBadReadPtr(mo, 0x48) || *(uint8_t **)(mo + MECHANOID_GLIDER) != g || !mo[0]) return NULL;
    for (int i = 0; i < 32 && mo[i]; i++)
        if (mo[i] < 0x20 || mo[i] > 0x7e) return NULL;
    return (const char *)mo;
}

// its pilot is dead: CGlider::Run explodes it (or already did)
static int pilot_dead(uint8_t *g)
{
    uint8_t *mo = glider_mechanoid(g);
    return g[GLIDER_DYING] || (mo && !IsBadReadPtr(mo, 0x160) && (mo[0x74] || mo[0xac] || mo[0x154]));
}

static int is_npc_glider(uint8_t *o)
{
    return o[OBJ_ALIVE] && *(int *)(o + OBJ_TYPE) == TYPE_GLIDER && o != G_PLAYER_GLIDER &&
           !coop_is_remote(o) && glider_pilot(o);
}

#define FOR_EACH_OBJECT(o) \
    for (void **node_ = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); node_; node_ = (void **)node_[1]) \
        for (uint8_t *o = (uint8_t *)node_[0]; o; o = NULL)

// Also runs when a guest loses the host and keeps flying: our NPC puppets have no AI, and with
// the session over nothing protects them any more - the game's own CGliderObject::Damage throws
// on the first hit (#15). Remove them while the world is still the host's.
static void fire_reset(void);

void mirror_reset(void)
{
    int removed = 0;
    for (int i = 0; i < MAXNPC; i++) {
        npc *e = &g_npc[i];
        if (g_client && e->used && e->spawned && e->glider && e->gen == coop_world_gen() && coop_in_flight() &&
            e->glider[OBJ_ALIVE]) {
            coop_remove_glider(e->glider);
            removed++;
        }
    }
    if (removed) mlog("mirror: session over, removed %d NPC puppets", removed);
    for (int i = 0; i < MAXNPC; i++) free(g_npc[i].blob);
    memset(g_npc, 0, sizeof g_npc);
    memset(g_php, 0, sizeof g_php);
    fire_reset();
    g_roster_done = 0;
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
    uint8_t *mo = glider_mechanoid(n->glider);
    if (!mo) return;
    int len;
    uint8_t *blob = coop_mechanoid_save(mo, &len);
    uint8_t *buf = malloc(sizeof(spawn_hdr) + len);
    spawn_hdr *h = (spawn_hdr *)buf;
    memset(h, 0, sizeof *h);
    h->id = (uint16_t)id;
    lstrcpynA(h->name, n->name, 32);
    memcpy(h->grp, n->grp, GROUP_HDR_LEN);
    memcpy(buf + sizeof *h, blob, len);
    net_send(to, MSG_NPC_SPAWN, buf, sizeof *h + len, 1);
    free(buf);
    free(blob);
}

// Joining players saw only an NPC's death, never its damage: the host owns NPCs and the guests'
// copies are protected (#22). Changed health goes out 4 times a second.
static void send_health(void)
{
    static DWORD last;
    if (GetTickCount() - last < 250) return;
    last = GetTickCount();
    uint8_t pkt[2 + 64 * sizeof(npc_health)];
    int n = 0;
    for (int i = 0; i < MAXNPC; i++) {
        npc *e = &g_npc[i];
        if (!e->used || !e->present || !e->glider || e->boomed) continue;
        uint8_t *mo = glider_mechanoid(e->glider);
        if (!mo) continue;
        float v[5];
        int changed = !e->hp_sent;
        for (int k = 0; k < 5; k++) {
            v[k] = *(float *)(mo + k_health_off[k]);
            float d = v[k] - e->hp[k];
            if (d > 0.5f || d < -0.5f) changed = 1;
        }
        if (!changed) continue;
        memcpy(e->hp, v, sizeof v);
        e->hp_sent = 1;
        npc_health *h = (npc_health *)(pkt + 2) + n;
        h->id = (uint16_t)i;
        memcpy(h->v, v, sizeof v);
        if (++n == 64) {
            *(uint16_t *)pkt = (uint16_t)n;
            net_send(NET_ALL, MSG_NPC_HEALTH, pkt, 2 + n * sizeof(npc_health), 1);
            n = 0;
        }
    }
    if (n) {
        *(uint16_t *)pkt = (uint16_t)n;
        net_send(NET_ALL, MSG_NPC_HEALTH, pkt, 2 + n * sizeof(npc_health), 1);
    }
}

// NPCs and co-op players (#22). NPC AI (CBasicGLAI, glider +0x10d8) attacks in state 3 (+0x3c)
// the glider at +0x44, set by vf 0x14 Attack(glider). Its group orders that (0x501a50) from a
// threat list of other groups by their index in the pilot list - our puppets have none, so no
// group ever went for them. Hits by co-op players count as the team's (h_damage), so a group
// turns on the team (group 0) as on the host's own shots; which glider of a group it attacks is
// "the member nearest to us" (0x4feb10, thiscall(group, pos)): for the host player's group that
// is now the nearest co-op player - the host or a puppet.
#define GLIDER_AI   0x10d8
#define AI_STATE    0x3c
#define AI_TARGET   0x44
#define AI_ATTACK   0x14

static int alive_object(const uint8_t *g);

static float dist2(const uint8_t *a, const uint8_t *b)
{
    const float *p = (const float *)(a + OBJ_POS), *q = (const float *)(b + OBJ_POS);
    return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
}

typedef uint8_t *(THISCALL *nearest_fn)(uint8_t *, const float *);
static nearest_fn o_nearest_member;

static uint8_t *THISCALL h_nearest_member(uint8_t *grp, const float *pos)
{
    uint8_t *g = o_nearest_member(grp, pos);
    if (net_mode() != NET_HOST || grp != G_PLAYER_MECH || !pos || g_coop_menu || (!coop_in_flight() && !g_base_world)) return g;
    float bd = 1e30f;
    uint8_t *game_pick = g;
    if (g == G_PLAYER_GLIDER && g_base_world) g = NULL;       // the host's glider is parked at the base: the others first
    if (g) {
        const float *q = (const float *)(g + OBJ_POS);
        bd = (q[0] - pos[0]) * (q[0] - pos[0]) + (q[1] - pos[1]) * (q[1] - pos[1]) + (q[2] - pos[2]) * (q[2] - pos[2]);
    }
    uint8_t *p, *best = g;
    for (int i = 0; (p = coop_remote_by_index(i, NULL)) != NULL; i++) {
        if (!p[OBJ_ALIVE] || p[GLIDER_DYING]) continue;
        const float *q = (const float *)(p + OBJ_POS);
        float d = (q[0] - pos[0]) * (q[0] - pos[0]) + (q[1] - pos[1]) * (q[1] - pos[1]) + (q[2] - pos[2]) * (q[2] - pos[2]);
        if (d < bd) { bd = d; best = p; }
    }
    static int nlog;
    if (best && best != g && nlog++ < 20) mlog("mirror: an NPC group goes for %s (nearest of the team)", coop_remote_name(best) ? coop_remote_name(best) : "?");
    return best ? best : game_pick;
}

// test: "provoke" - the NPC nearest a co-op player's puppet: its group attacks the host's group (it
// should go for the puppet when that is nearer)
void mirror_test_provoke(void)
{
    uint8_t *pup = coop_remote_by_index(0, NULL), *best = NULL;
    if (!pup) { mlog("provoke: no player puppet"); return; }
    float bd = 1e30f, margin = -1e30f;              // the NPC where the puppet is most clearly nearer than the host
    FOR_EACH_OBJECT(o) {
        if (!is_npc_glider(o) || o[GLIDER_DYING] || !*(uint8_t **)(o + GLIDER_AI)) continue;
        float m = sqrtf(dist2(o, G_PLAYER_GLIDER)) - sqrtf(dist2(o, pup));
        if (m > margin) { margin = m; best = o; bd = dist2(o, pup); }
    }
    if (!best) { mlog("provoke: no NPC"); return; }
    uint8_t *grp = *(uint8_t **)(glider_mechanoid(best) + MECHANOID_MECH);
    typedef void (THISCALL *group_attack_fn)(uint8_t *, uint8_t *, int);
    CALL(0x00501a50, group_attack_fn)(grp, G_PLAYER_MECH, 1);       // CMechGroup: attack that group
    uint8_t *t = *(uint8_t **)(*(uint8_t **)(best + GLIDER_AI) + AI_TARGET);
    mlog("provoke: %s's group attacks the host's (host %.0f away, puppet %.0f away) -> it goes for %s",
         glider_pilot(best), sqrtf(dist2(best, G_PLAYER_GLIDER)), sqrtf(bd),
         t == G_PLAYER_GLIDER ? "the host" : t == pup ? "the puppet" : "someone else");
}

// test: "npcai" - NPCs in attack mode and their target
void mirror_test_ai(void)
{
    int n = 0;
    FOR_EACH_OBJECT(o) {
        if (!is_npc_glider(o)) continue;
        uint8_t *ai = *(uint8_t **)(o + GLIDER_AI);
        if (!ai || *(int *)(ai + AI_STATE) != 3 || n >= 30) continue;
        uint8_t *t = *(uint8_t **)(ai + AI_TARGET);
        const char *tn = !t ? "-" : t == G_PLAYER_GLIDER ? "the host" : coop_remote_name(t) ? coop_remote_name(t) :
                         (glider_pilot(t) ? glider_pilot(t) : "?");
        mlog("npcai: %-14.32s attacks %-14.32s at %.0f", glider_pilot(o), tn, t ? sqrtf(dist2(o, t)) : 0.0f);
        n++;
    }
    if (!n) mlog("npcai: no NPC attacking");
}

// CGlider::Run 0x4f9b80 explodes a glider whose pilot is dead (EXP_GLIDER_2, wreck, loot) and the
// glider leaves the object list within the frame - the 100 ms scan never saw an NPC dying, so the
// guests only got "gone" and their copies vanished (#27). The boom goes out here, before that.
typedef void (THISCALL *glider_run_fn)(uint8_t *);
static glider_run_fn o_glider_run;

static void THISCALL h_glider_run(uint8_t *g)
{
    if (net_mode() == NET_HOST && !g_coop_menu && g != G_PLAYER_GLIDER) {
        uint8_t *mo = glider_mechanoid(g);
        if (mo && !g[0x434] && (mo[0x74] || mo[0xac] || mo[0x154]) && !coop_is_remote(g)) {
            int gen = coop_world_gen();
            for (int i = 0; i < MAXNPC; i++) {
                npc *e = &g_npc[i];
                if (!e->used || e->glider != g || e->gen != gen || e->boomed) continue;
                e->boomed = 1;
                uint16_t id = (uint16_t)i;
                net_send(NET_ALL, MSG_NPC_BOOM, &id, 2, 1);
                static int nlog;
                if (nlog++ < 20) mlog("mirror: %s destroyed - told the guests", e->name);
                break;
            }
        }
    } else if (net_mode() == NET_CLIENT && !g_coop_menu && g != G_PLAYER_GLIDER) {
        uint8_t *mo = glider_mechanoid(g);
        static int nlog;
        if (mo && !g[0x434] && mo[0x74] && nlog++ < 20) mlog("mirror: exploding %s here", glider_pilot(g) ? glider_pilot(g) : (char *)g + OBJ_MODEL);
    }
    o_glider_run(g);
}

// g is still in the object list (alive or dying; dead gliders are freed when the list is packed)
static int in_object_list(const uint8_t *g)
{
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1])
        if (nd[0] == g) return 1;
    return 0;
}

static void host_scan(void)
{
    DWORD now = GetTickCount();
    if (now - g_last_scan < 100) return;
    g_last_scan = now;
    int gen = coop_world_gen();
    for (int i = 0; i < MAXNPC; i++) g_npc[i].present = 0;
    uint8_t pkt[2 + 20 * sizeof(npc_state)];
    int n = 0, nseen = 0, ngone = 0;
    FOR_EACH_OBJECT(o) {
        if (!is_npc_glider(o)) continue;
        nseen++;
        const char *name = glider_pilot(o);
        int id = host_id(name);
        if (id < 0) continue;
        npc *e = &g_npc[id];
        int fresh = !e->used || e->glider != o || e->gen != gen;
        if (!e->used) {
            e->used = 1;
            lstrcpynA(e->name, name, 32);
        }
        if (fresh) e->boomed = 0, e->hp_sent = 0;    // a new spawn is repaired: its health again
        e->glider = o;
        e->gen = gen;
        e->present = 1;
        if (fresh) {
            uint8_t *grp = *(uint8_t **)(glider_mechanoid(o) + MECHANOID_MECH);
            if (grp) memcpy(e->grp, grp + GROUP_HDR, GROUP_HDR_LEN);
            send_spawn(NET_ALL, id);
        }
        if (o[GLIDER_DYING] && !e->boomed) {
            e->boomed = 1;
            uint16_t i16 = (uint16_t)id;
            net_send(NET_ALL, MSG_NPC_BOOM, &i16, 2, 1);
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
        npc *e = &g_npc[i];
        if (!e->used || e->present) continue;
        uint16_t id = (uint16_t)i;
        // A dying NPC can fail is_npc_glider (its pilot lets go of the glider) and so was never
        // boomed - the guests' copies just vanished (#27). Still in the list and dying: boom, keep it.
        if (e->glider && e->gen == gen && in_object_list(e->glider) && e->glider[GLIDER_DYING]) {
            if (!e->boomed) {
                e->boomed = 1;
                net_send(NET_ALL, MSG_NPC_BOOM, &id, 2, 1);
            }
            continue;
        }
        net_send(NET_ALL, MSG_NPC_GONE, &id, 2, 1);
        memset(e, 0, sizeof *e);
        ngone++;
    }
    static int nlog;
    if (ngone >= 3 && nlog++ < 20) mlog("mirror: %d NPCs gone at once (%d seen, base world %d, in flight %d, hour %.3f)", ngone, nseen, g_base_world, coop_in_flight(), *(float *)0x00676614);
    send_health();
}

// the object list still holds g (saving the game packs the list: dead gliders are freed)
static int alive_object(const uint8_t *g)
{
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1])
        if (nd[0] == g) return g[OBJ_ALIVE];
    return 0;
}

void mirror_peer_joined(int peer)
{
    if (!coop_in_flight()) return;
    for (int i = 0; i < MAXNPC; i++)
        if (g_npc[i].used && g_npc[i].glider && g_npc[i].gen == coop_world_gen() && alive_object(g_npc[i].glider)) {
            send_spawn(peer, i);
            g_npc[i].hp_sent = 0;              // the new player's copies start repaired
        }
    net_send(peer, MSG_NPC_ROSTER_END, NULL, 0, 1);
}

// ---- client ------------------------------------------------------------------------------
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

static uint8_t *spawn_npc(npc *e, const float m[16])
{
    uint8_t *g = coop_spawn_glider(e->blob, e->blob_len, m);
    if (!g) return NULL;
    uint8_t *mo = glider_mechanoid(g);
    uint8_t *grp = *(uint8_t **)(mo + MECHANOID_MECH);
    if (grp) memcpy(grp + GROUP_HDR, e->grp, GROUP_HDR_LEN);     // organisation (radar colour, relations)
    // the pilot's name (shown when targeted) isn't in the saved mechanoid: the copy had garbage there
    memset(mo, 0, 32);
    lstrcpynA((char *)mo, e->name, 32);
    return g;
}

static void apply_health(uint8_t *g, const float *v)
{
    uint8_t *mo = glider_mechanoid(g);
    if (!mo || *(uint8_t **)(mo + MECHANOID_GLIDER) != g) return;
    for (int k = 0; k < 5; k++) *(float *)(mo + k_health_off[k]) = v[k];
    for (int k = 2; k < 5; k++) if (v[k] > 0) mo[k_health_off[k] - 0x24 + 0x2d] = 1;   // part "damaged"
}

static void client_frame(void)
{
    int gen = coop_world_gen();
    DWORD now = GetTickCount();
    // NPCs of our own (from the loaded snapshot or our scripts) that the host doesn't have
    if (g_roster_done && now - g_last_cleanup > 2000) {
        g_last_cleanup = now;
        int removed = 0;
        FOR_EACH_OBJECT(o) {
            if (is_npc_glider(o) && !pilot_dead(o) && !in_roster(glider_pilot(o))) {
                if (removed < 8) mlog("mirror: removing local NPC %s (%s)", glider_pilot(o), (char *)o + OBJ_MODEL);
                coop_remove_glider(o);
                removed++;
            }
        }
        if (removed) mlog("mirror: removed %d local NPC gliders not on the host", removed);
    }
    for (int i = 0; i < MAXNPC; i++) {
        npc *e = &g_npc[i];
        if (!e->used) continue;
        if (e->glider && e->gen != gen) e->glider = NULL;
        if (e->gone) {
            if (e->glider && e->glider[OBJ_ALIVE] && !pilot_dead(e->glider)) coop_remove_glider(e->glider);
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
            e->spawned = 0;
            if (!e->glider && e->blob) e->spawned = (e->glider = spawn_npc(e, m)) != NULL;
            e->gen = gen;
        }
        if (e->glider && !e->glider[GLIDER_DYING]) {
            coop_drive(e->glider, m, vel);
            if (e->hp_sent) apply_health(e->glider, e->hp);   // the host's values, every frame (shields regenerate)
        }
    }
}

// ---- world time ---------------------------------------------------------------------------
// The clock (days and hours) advances on every machine; the host's wins.
#define WORLD_DAYS  ((float *)0x0067661c)
#define WORLD_HOURS ((float *)0x00676614)

static void handle_time(const uint8_t *data, int len)
{
    if (len != 8 || !coop_in_flight() || g_coop_menu) return;
    *WORLD_DAYS = ((const float *)data)[0];
    *WORLD_HOURS = ((const float *)data)[1];
}

// ---- combat -------------------------------------------------------------------------------
// Every shot is a call of a gun's fire function (made only when the gun is
// ready). Shots by the local player (and, on the host, by NPCs) are sent and
// replayed by the same glider on the other machines; on the host a joining
// player's replayed shots do the real damage to NPCs. NPCs on joining players
// and joining players' gliders on the host take no local damage.
typedef struct { float mtx[16]; float x, y, z; void *target; float k; } fire_tail;
#pragma pack(push, 1)
typedef struct { char shooter[32]; char target[32]; uint8_t slot; float m0[16]; fire_tail t; } fire_msg;
#pragma pack(pop)

#define WEAPON0      0x0ba4
#define WEAPON_SIZE  0x148
#define WEAPON_OWNER 0x118
#define WEAPON_DATA  0x114      // the gun's weapon data; NULL = empty slot (CGun::FIRE reads it unchecked)

typedef void (THISCALL *fire_fn)(void *, float *, fire_tail);
typedef void (THISCALL *damage_fn)(void *, float, float, float, float, float, void *, int);
static fire_fn o_fire;
static damage_fn o_damage;
static int g_replaying;

// "@player" for players, the pilot id for NPCs
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

// Continuous guns (base guns such as SHUN) fire on every frame, not at a reload rate: one reliable
// message per call was ~450 a second per gun at an uncapped frame rate, more than the players' link
// carried - every send then waited and the game ran at a few frames a second. A gun that fired less
// than HOLD_GAP_MS ago goes out at most every HOLD_SEND_MS, as an unreliable "keep firing" (slot |
// FIRE_HOLD); the receiver fires it on each of its own frames until HOLD_KEEP_MS pass without news,
// as its own game would.
#define FIRE_HOLD    0x80
#define HOLD_GAP_MS  25.0
#define HOLD_SEND_MS 33.0
#define HOLD_KEEP_MS 100.0
#define MAXFSRC      64
#define MAXHOLD      32
typedef struct { uint8_t *g; int slot; double last_call, last_sent; } fire_src;
typedef struct { fire_msg f; double until; } fire_hold;
static fire_src g_fsrc[MAXFSRC];
static fire_hold g_fhold[MAXHOLD];

static void fire_reset(void)
{
    memset(g_fsrc, 0, sizeof g_fsrc);
    memset(g_fhold, 0, sizeof g_fhold);
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

// the sender's record of this gun (the longest unused one is reused)
static fire_src *fire_source(uint8_t *g, int slot, double now)
{
    fire_src *old = &g_fsrc[0];
    for (int i = 0; i < MAXFSRC; i++) {
        if (g_fsrc[i].g == g && g_fsrc[i].slot == slot) return &g_fsrc[i];
        if (g_fsrc[i].last_call < old->last_call) old = &g_fsrc[i];
    }
    memset(old, 0, sizeof *old);
    old->g = g;
    old->slot = slot;
    old->last_call = now - 1000.0;
    return old;
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
    double now = now_ms();
    fire_src *src = fire_source(g, slot, now);
    int hold = now - src->last_call < HOLD_GAP_MS;
    src->last_call = now;
    if (hold && now - src->last_sent < HOLD_SEND_MS) return;
    src->last_sent = now;
    fire_msg f;
    memset(&f, 0, sizeof f);
    if (!glider_ref(g, f.shooter)) return;
    glider_ref((uint8_t *)t.target, f.target);
    f.slot = (uint8_t)(slot | (hold ? FIRE_HOLD : 0));
    memcpy(f.m0, m0, sizeof f.m0);
    f.t = t;
    f.t.target = NULL;
    net_send(NET_ALL, MSG_FIRE, &f, sizeof f, !hold);
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
            mlog("mirror: shot by %s: slot %d is empty here, %s (glider %p %s, %s, guns %x, %d so far)",
                 f->shooter, f->slot, alt < 0 ? "skipped" : "fired the first loaded slot instead",
                 g, (char *)g + OBJ_MODEL,
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

static void fire_received(const fire_msg *f)
{
    if (!(f->slot & FIRE_HOLD)) { replay_fire(f); return; }
    // a continuous gun: (re)start holding it; hold_frame fires it
    fire_hold *h = NULL, *free_h = NULL;
    for (int i = 0; i < MAXHOLD && !h; i++) {
        if (g_fhold[i].until && !strcmp(g_fhold[i].f.shooter, f->shooter) && g_fhold[i].f.slot == (f->slot & 3))
            h = &g_fhold[i];
        else if (!g_fhold[i].until && !free_h)
            free_h = &g_fhold[i];
    }
    if (!h) h = free_h;
    if (!h) return;
    h->f = *f;
    h->f.slot &= 3;
    h->until = now_ms() + HOLD_KEEP_MS;
    static int nlog;
    if (nlog++ < 5) mlog("mirror: %s keeps firing (slot %d)", f->shooter, f->slot & 3);
}

// continuous guns of the others fire on each of our frames while their news is fresh
static void hold_frame(void)
{
    double now = now_ms();
    for (int i = 0; i < MAXHOLD; i++) {
        if (!g_fhold[i].until) continue;
        if (now > g_fhold[i].until) { g_fhold[i].until = 0; continue; }
        replay_fire(&g_fhold[i].f);
    }
}

static int protected_glider(uint8_t *g)
{
    if (net_mode() == NET_HOST) return coop_is_remote(g);                // a joining player's glider
    // on a guest every glider but our own belongs to someone else: the host's NPCs, and the other
    // players' puppets - a puppet "killed" here dropped its pilot's mechmind as loot (#27)
    if (net_mode() == NET_CLIENT) return g != G_PLAYER_GLIDER && *(int *)(g + OBJ_TYPE) == TYPE_GLIDER;
    return 0;
}

// The game's pilot list (count, array of pilot objects). Our puppets' pilots are not in it.
#define PILOTS_N   (*(int *)0x00678f54)
#define PILOTS_P   (*(uint8_t ***)0x00678f58)

static int listed_pilot(uint8_t *g)
{
    uint8_t *mo = glider_mechanoid(g);
    uint8_t *mech = mo ? *(uint8_t **)(mo + MECHANOID_MECH) : NULL;
    for (int i = 0; mech && PILOTS_P && i < PILOTS_N; i++) if (PILOTS_P[i] == mech) return 1;
    return 0;
}

int g_kill_peer = -1;          // host: the guest whose hit Damage is applying now (killcredit.c)

// guest: our copy of the host's NPC flown by that pilot (NULL if there is none in this world)
uint8_t *mirror_copy_of(const char *pilot)
{
    for (int i = 0; i < MAXNPC; i++) {
        npc *e = &g_npc[i];
        if (e->used && e->glider && e->gen == coop_world_gen() && !strcmp(e->name, pilot) && in_object_list(e->glider))
            return e->glider;
    }
    return NULL;
}

static void THISCALL h_damage(uint8_t *self, float dmg, float x, float y, float z, float f5, void *att, int type)
{
    int kill_peer = -1;
    if (net_mode() != NET_OFF) {
        // a projectile or explosion: its shooter (owner at +0x10)
        uint8_t *a = att;
        if (a && *(int *)(a + OBJ_TYPE) != TYPE_GLIDER && *(uint8_t **)(a + 0x10)) att = *(uint8_t **)(a + 0x10);
        // co-op players don't hurt each other
        if (self == G_PLAYER_GLIDER && att && coop_is_remote(att)) return;
        // the host is in a base while its world runs (#37): its glider parked outside is not there
        if (self == G_PLAYER_GLIDER && g_base_world) return;
        if (coop_is_remote(self) && att == G_PLAYER_GLIDER) return;
        if (protected_glider(self)) {
            // Someone else's glider: its owner's machine decides its damage. The hit is still shown -
            // Damage makes the sparks and the shield flash (type -1 = a projectile, 0x4e58f0 /
            // EXP_GL_MINI); skipping it whole left the guests without any impact (#31). A token
            // damage, no attacker (no pilot / group handlers), and the death bytes kept as they were.
            uint8_t *mo = glider_mechanoid(self);
            if (!mo || *(uint8_t **)(mo + MECHANOID_GLIDER) != self || self[GLIDER_DYING] || dmg <= 0) return;
            float hp[5];
            uint8_t dead[3] = {mo[0x74], mo[0xac], mo[0x154]};
            for (int k = 0; k < 5; k++) hp[k] = *(float *)(mo + k_health_off[k]);
            o_damage(self, 0.01f, x, y, z, f5, NULL, type);
            for (int k = 0; k < 5; k++) *(float *)(mo + k_health_off[k]) = hp[k];
            mo[0x74] = dead[0]; mo[0xac] = dead[1]; mo[0x154] = dead[2];
            return;
        }
        // Damage tells the victim's pilot who attacked it by the attacker's index in the pilot list
        // (0x4fb8ef); a puppet's pilot isn't listed, the index is -1 and the pilot's handler (0x5048d0)
        // throws: the host crashed when a guest's shot hit an NPC (#20). On the host, a co-op
        // player's hit counts as the host player's - the team's, group 0 (#22): the NPC's group
        // turns on the team as for the host's own shots, and h_nearest_member sends it at the
        // nearest co-op player. Otherwise the hit counts without an attacker.
        a = att;
        if (net_mode() == NET_HOST && a && coop_is_remote(a)) kill_peer = coop_remote_peer(a);
        if (a && *(int *)(a + OBJ_TYPE) == TYPE_GLIDER && !a[0x6a] && (!listed_pilot(a) || !listed_pilot(self))) {
            int team = net_mode() == NET_HOST && coop_is_remote(a) && listed_pilot(self) && coop_in_flight() &&
                       !G_PLAYER_GLIDER[GLIDER_DYING] && listed_pilot(G_PLAYER_GLIDER);
            static int nlog;
            if (nlog++ < 10) mlog("mirror: hit on %s by %s counted %s", (char *)self + OBJ_MODEL, (char *)a + OBJ_MODEL,
                                  team ? "as the team's (host player)" : "without attacker (puppet pilot)");
            att = team ? G_PLAYER_GLIDER : NULL;
        }
        // Each player's own world decides their own glider's damage (turrets hit there too); the others
        // see it through MSG_PLAYER_HEALTH on the puppet (#24).
        if (self == G_PLAYER_GLIDER) {
            static int nlog;
            if (nlog++ < 60 || dmg >= 1.0f) {
                const char *an = att && *(int *)((uint8_t *)att + OBJ_TYPE) == TYPE_GLIDER ? glider_pilot(att) : NULL;
                mlog("mirror: player damaged %.1f type %d by %p (%s, object type %x)", dmg, type, att, an ? an : "?",
                     att ? *(int *)((uint8_t *)att + OBJ_TYPE) : 0);
            }
        }
    }
    g_kill_peer = kill_peer;
    o_damage(self, dmg, x, y, z, f5, att, type);
    g_kill_peer = -1;
}

// test: "npchp" - health of the NPCs near the player (shield, armour, reactor / drive / body damage)
void mirror_test_hp(int all)
{
    // by our NPC table: the guests' copies are found by it (their pilots may have no name)
    int shown = 0;
    const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    for (int i = 0; i < MAXNPC && shown < 40; i++) {
        npc *e = &g_npc[i];
        uint8_t *o = e->used && e->gen == coop_world_gen() ? e->glider : NULL;
        if (!o || !alive_object(o)) continue;
        const float *p = (const float *)(o + OBJ_POS);
        if ((p[0] - pp[0]) * (p[0] - pp[0]) + (p[1] - pp[1]) * (p[1] - pp[1]) > 3000.0f * 3000.0f && !all) continue;
        uint8_t *mo = glider_mechanoid(o);
        if (!mo) continue;
        shown++;
        float v[5];
        for (int k = 0; k < 5; k++) v[k] = *(float *)(mo + k_health_off[k]);
        mlog("npchp: %-14.32s %8.1f %8.1f %7.1f %7.1f %7.1f", e->name, v[0], v[1], v[2], v[3], v[4]);
    }
}

// test: "tonpc [dist]" - put our glider dist (default 120) in front of the nearest NPC, facing it.
// The glider matrix holds its axes in columns: right, up, back (m[4 * row + axis]).
float g_face = 1.0f;          // test: which way the third axis points (set by "face -1")
char g_test_pilot[32];        // test: "killnpc <pilot>" picks that NPC
float g_test_dmg;             // test: "npchit <damage>"

void mirror_test_tonpc(float dist)
{
    if (dist == 0) dist = 120;
    if (dist < 0) dist = dist > -0.7f ? 0.6f : 0.1f;   // npchit (-0.6) / killnpc (-1)
    uint8_t *best = NULL;
    float bd = 1e30f;
    FOR_EACH_OBJECT(o) {
        if (!o[OBJ_ALIVE] || *(int *)(o + OBJ_TYPE) != TYPE_GLIDER || o[GLIDER_DYING] || o == G_PLAYER_GLIDER ||
            coop_remote_name(o)) continue;
        float d = dist2(o, G_PLAYER_GLIDER);
        if (g_test_pilot[0]) d = glider_pilot(o) && !strcmp(glider_pilot(o), g_test_pilot) ? 0 : 1e30f;
        if (d < bd) { bd = d; best = o; }
    }
    g_test_pilot[0] = 0;
    if (!best) { mlog("tonpc: no NPC"); return; }
    if (dist > 0.5f && dist < 0.7f) {      // "npchit": our projectile hits it (type -1), 50 damage
        uint8_t *mo = glider_mechanoid(best);
        float before = mo ? *(float *)(mo + k_health_off[0]) + *(float *)(mo + k_health_off[1]) : 0;
        const float *p = (const float *)(best + OBJ_POS);
        h_damage(best, g_test_dmg > 0 ? g_test_dmg : 50.0f, p[0], p[1], p[2], 0.0f, G_PLAYER_GLIDER, -1);
        float after = mo ? *(float *)(mo + k_health_off[0]) + *(float *)(mo + k_health_off[1]) : 0;
        mlog("npchit: %s shield+armour %.1f -> %.1f, dying %d", glider_pilot(best) ? glider_pilot(best) : "?", before, after,
             best[GLIDER_DYING]);
        return;
    }
    if (dist < 0.5f) {                     // "killnpc": its pilot dies, as by a shot
        uint8_t *mo = glider_mechanoid(best);
        if (mo) mo[0x74] = 1;
        mlog("killnpc: %s (%s)", glider_pilot(best) ? glider_pilot(best) : "?", (char *)best + OBJ_MODEL);
        return;
    }
    const float *q = (const float *)(best + OBJ_POS), *p = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    float f[3] = {q[0] - p[0], q[1] - p[1], 0};
    float l = sqrtf(f[0] * f[0] + f[1] * f[1]);
    if (l < 1) { f[0] = 1; f[1] = 0; l = 1; }
    f[0] /= l; f[1] /= l;
    float up[3] = {0, 0, 1}, back[3] = {f[0] * g_face, f[1] * g_face, 0};
    float right[3] = {up[1] * back[2] - up[2] * back[1], up[2] * back[0] - up[0] * back[2], up[0] * back[1] - up[1] * back[0]};
    float m[16] = {0}, vel[3] = {0};
    for (int i = 0; i < 3; i++) { m[4 * i] = right[i]; m[4 * i + 1] = up[i]; m[4 * i + 2] = back[i]; }
    m[12] = q[0] - f[0] * dist; m[13] = q[1] - f[1] * dist; m[14] = q[2]; m[15] = 1;
    coop_drive(G_PLAYER_GLIDER, m, vel);
    mlog("tonpc: %.0f in front of %s (%s)", dist, glider_pilot(best) ? glider_pilot(best) : "?", (char *)best + OBJ_MODEL);
}

// test: "mirrorstat" - the NPC table: copy, last state's age, copy vs host position
void mirror_test_stat(void)
{
    int gen = coop_world_gen(), n = 0;
    DWORD now = GetTickCount();
    for (int i = 0; i < MAXNPC; i++) {
        npc *e = &g_npc[i];
        if (!e->used) continue;
        n++;
        uint8_t *g = e->glider && e->gen == gen && alive_object(e->glider) ? e->glider : NULL;
        const float *s = e->nsnap ? e->snap[0].m + 12 : NULL;
        const float *p = g ? (const float *)(g + OBJ_POS) : NULL;
        float d = s && p ? sqrtf((s[0] - p[0]) * (s[0] - p[0]) + (s[1] - p[1]) * (s[1] - p[1]) + (s[2] - p[2]) * (s[2] - p[2])) : -1;
        mlog("mirrorstat: %3d %-14.32s %p %s%s age %5ld off %7.1f at %7.0f %7.0f%s%s", i, e->name, g,
             e->spawned ? "puppet" : "local ", g && g[GLIDER_DYING] ? " dying" : "",
             e->nsnap ? (long)(now - e->snap[0].t) : -1L, d, s ? s[0] : 0, s ? s[1] : 0,
             e->boomed ? " boomed" : "", e->present ? " present" : "");
    }
    mlog("mirrorstat: %d entries", n);
}

// test: "selfhit <dmg>" - our glider takes a hit without attacker; "playerhp" - the other players' gliders here
void mirror_test_player(int hit, float dmg)
{
    if (hit) {
        const float *p = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
        h_damage(G_PLAYER_GLIDER, dmg, p[0], p[1], p[2], 0.0f, NULL, 0);
    }
    uint8_t *g;
    const char *nm;
    for (int i = 0; (g = coop_remote_by_index(i, &nm)) != NULL; i++) {
        uint8_t *mo = glider_mechanoid(g);
        if (mo) mlog("playerhp: %s %8.1f %8.1f %7.1f %7.1f %7.1f", nm, *(float *)(mo + k_health_off[0]), *(float *)(mo + k_health_off[1]),
                     *(float *)(mo + k_health_off[2]), *(float *)(mo + k_health_off[3]), *(float *)(mo + k_health_off[4]));
    }
    uint8_t *mo = glider_mechanoid(G_PLAYER_GLIDER);
    if (mo) mlog("playerhp: (us) %8.1f %8.1f %7.1f %7.1f %7.1f", *(float *)(mo + k_health_off[0]), *(float *)(mo + k_health_off[1]),
                 *(float *)(mo + k_health_off[2]), *(float *)(mo + k_health_off[3]), *(float *)(mo + k_health_off[4]));
}

// test: "puppethit" - a co-op player's puppet hits the nearest NPC (1 damage), as a replayed shot does (#20)
void mirror_test_hit(float dmg)
{
    uint8_t *npc = NULL, *pup = NULL;
    float best = 1e30f;
    const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    FOR_EACH_OBJECT(o) {
        if (!o[OBJ_ALIVE] || *(int *)(o + OBJ_TYPE) != TYPE_GLIDER || o[GLIDER_DYING] || o == G_PLAYER_GLIDER) continue;
        if (coop_is_remote(o)) { pup = o; continue; }
        const float *p = (const float *)(o + OBJ_POS);
        float d = (p[0] - pp[0]) * (p[0] - pp[0]) + (p[1] - pp[1]) * (p[1] - pp[1]);
        if (d < best) { best = d; npc = o; }
    }
    if (!npc || !pup) { mlog("puppethit: no NPC (%p) or no player puppet (%p)", npc, pup); return; }
    const float *p = (const float *)(npc + OBJ_POS);
    mlog("puppethit: %s hit by puppet %p", (char *)npc + OBJ_MODEL, pup);
    h_damage(npc, dmg > 0 ? dmg : 1.0f, p[0], p[1], p[2], 0.0f, pup, 0);
    mlog("puppethit: done, NPC still alive: %d", npc[OBJ_ALIVE]);
    mlog("puppethit: %s (%s) hit by puppet %p", glider_pilot(npc), (char *)npc + OBJ_MODEL, pup);
    h_damage(npc, 1.0f, p[0], p[1], p[2], 0.0f, pup, 0);
    // its group's threat list entry for the player's group (index 0): +0x628c, 0x1c each, count +0x64bc
    uint8_t *mo = glider_mechanoid(npc);
    uint8_t *grp = mo ? *(uint8_t **)(mo + MECHANOID_MECH) : NULL;
    int n = grp ? *(int *)(grp + 0x64bc) : 0, found = 0;
    for (int i = 0; i < n && i < 20; i++) {
        int *e = (int *)(grp + 0x628c + i * 0x1c);
        if (e[0] != 0) continue;
        mlog("puppethit: group's entry for the team: type %d  %.2f %.2f %.2f %.2f", e[1],
             ((float *)e)[2], ((float *)e)[3], ((float *)e)[4], ((float *)e)[5]);
        found = 1;
    }
    mlog("puppethit: done, NPC still alive: %d%s (group %p, %d entries, org %d)", npc[OBJ_ALIVE],
         found ? "" : ", no entry for the team", grp, n, grp ? *(int *)(grp + 0x6c) : -1);
}

// On a guest, an exploding copy drops no loot of its own: CGlider::Run makes the mechmind, the
// cargo containers and the falling wreck only for a local or non-network glider (+0x7b / +0x6a,
// as for PvP copies). The host's loot comes through loot.c (#27).
static void no_local_loot(uint8_t *g)
{
    if (net_mode() != NET_CLIENT) return;
    g[OBJ_NET] = 1;
    g[OBJ_LOCAL] = 0;
}

static void replay_boom(int id)
{
    if (id >= MAXNPC || !g_npc[id].used) return;
    uint8_t *g = g_npc[id].glider;
    if (!g || g_npc[id].gen != coop_world_gen() || !g[OBJ_ALIVE] || g[GLIDER_DYING]) return;
    uint8_t *mo = glider_mechanoid(g);
    if (!mo) return;
    no_local_loot(g);
    mo[0x74] = 1;                      // dead pilot: CGlider::Run explodes the glider next frame
    static int nlog;
    if (nlog++ < 20) mlog("mirror: %s destroyed on the host", g_npc[id].name);
}

// A player was destroyed on their own machine (#22): their glider is theirs, protected here, so
// without this their puppet kept flying. The game explodes the puppet like a dead NPC (pilot's
// "destroyed" byte, as replay_boom); coop_core stops driving it until new pilot data comes.
static void player_down(const char *name)
{
    uint8_t *g = coop_remote_glider(name);
    int peer = g ? coop_remote_peer(g) : -1;
    if (peer < 0) { mlog("mirror: %s destroyed (no puppet here)", name); return; }
    uint8_t *mo = glider_mechanoid(g);
    int boom = coop_in_flight() && !g_coop_menu && mo && g[OBJ_ALIVE] && !g[GLIDER_DYING] && alive_object(g);
    if (boom) {
        no_local_loot(g);              // a guest gets the host's drop (their mechmind) through loot.c
        mo[0x74] = 1;
        respawn_player_down(peer, name, g);      // the game drops their mechmind here: a teammate can bring it to a base
    }
    coop_remote_down(peer, boom);
    mlog("mirror: player %s destroyed%s", name, boom ? "" : " (puppet removed)");
    char msg[96];
    snprintf(msg, sizeof msg, aim1_ui("destroyed"), name);
    coop_notify(msg);
}

// ---- players' health (#24) ----------------------------------------------------------------------
// Each player's own world decides their damage (NPCs, turrets); the others' copy of that glider is
// protected and showed hits without damage, then exploded on MSG_PLAYER_DOWN. Each player sends
// their health; the others apply it to the puppet every frame (like the NPCs' MSG_NPC_HEALTH).
static void send_player_health(void)
{
    static DWORD last, last_full;
    static float sent[5];
    DWORD now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    uint8_t *mo = G_PLAYER_GLIDER ? glider_mechanoid(G_PLAYER_GLIDER) : NULL;
    if (!mo) return;
    struct { char name[32]; float v[5]; } m;
    memset(&m, 0, sizeof m);
    lstrcpynA(m.name, coop_my_name(), sizeof m.name);
    int changed = now - last_full >= 2000;
    for (int k = 0; k < 5; k++) {
        m.v[k] = *(float *)(mo + k_health_off[k]);
        if (fabsf(m.v[k] - sent[k]) > 0.5f) changed = 1;
    }
    if (!changed) return;
    memcpy(sent, m.v, sizeof sent);
    last_full = now;
    net_send(NET_ALL, MSG_PLAYER_HEALTH, &m, sizeof m, 0);
}

static void got_player_health(const uint8_t *data, int len)
{
    if (len != 32 + 5 * (int)sizeof(float) || data[31]) return;
    int slot = -1;
    for (int i = 0; i < NET_MAX_PEERS && slot < 0; i++)
        if (g_php[i].valid && !strcmp(g_php[i].name, (const char *)data)) slot = i;
    for (int i = 0; i < NET_MAX_PEERS && slot < 0; i++)
        if (!g_php[i].valid) slot = i;
    if (slot < 0) return;
    memcpy(g_php[slot].name, data, 32);
    memcpy(g_php[slot].v, data + 32, sizeof g_php[slot].v);
    if (!g_php[slot].valid) mlog("mirror: health of player %s received", g_php[slot].name);
    g_php[slot].valid = 1;
}

static void apply_player_health(void)
{
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (!g_php[i].valid) continue;
        uint8_t *g = coop_remote_glider(g_php[i].name);
        if (g && g[OBJ_ALIVE] && !g[GLIDER_DYING]) apply_health(g, g_php[i].v);
    }
}

// our own death, once per life: tell the others
static void check_own_death(void)
{
    static int down;
    uint8_t *mo = G_PLAYER_GLIDER ? glider_mechanoid(G_PLAYER_GLIDER) : NULL;
    int dead = mo && (mo[0x74] || mo[0xac] || mo[0x154] || G_PLAYER_GLIDER[GLIDER_DYING]);
    if (dead && !down) {
        char n[32] = {0};
        lstrcpynA(n, coop_my_name(), sizeof n);
        net_send(NET_ALL, MSG_PLAYER_DOWN, n, sizeof n, 1);
        mlog("mirror: we were destroyed - told the others");
    }
    down = dead;
}

int mirror_handle(int type, int from, const uint8_t *data, int len)
{
    (void)from;
    switch (type) {
    case MSG_PLAYER_DOWN:
        if (len == 32 && data[31] == 0) player_down((const char *)data);
        return 1;
    case MSG_PLAYER_HEALTH:
        got_player_health(data, len);
        return 1;
    case MSG_FIRE:
        if (len == sizeof(fire_msg) && coop_in_flight() && !g_coop_menu) fire_received((const fire_msg *)data);
        return 1;
    case MSG_NPC_BOOM:
        if (len >= 2 && coop_in_flight() && !g_coop_menu) replay_boom(*(const uint16_t *)data);
        return 1;
    case MSG_WORLD_TIME:
        handle_time(data, len);
        return 1;
    case MSG_NPC_HEALTH: {                     // applied in client_frame (the copy may not exist yet)
        if (len < 2 || net_mode() != NET_CLIENT) return 1;
        int n = *(const uint16_t *)data;
        if (len < 2 + n * (int)sizeof(npc_health)) return 1;
        const npc_health *h = (const npc_health *)(data + 2);
        for (int i = 0; i < n; i++, h++) {
            if (h->id >= MAXNPC || !g_npc[h->id].used) continue;
            memcpy(g_npc[h->id].hp, h->v, sizeof h->v);
            g_npc[h->id].hp_sent = 1;
        }
        static int nlog;
        if (nlog++ < 5) mlog("mirror: NPC health for %d gliders", n);
        return 1;
    }
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
        memcpy(e->grp, h->grp, GROUP_HDR_LEN);
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
                // A killed NPC leaves the host's list within the frame: "gone" follows "boom" by one
                // scan and both often arrive in the same frame here - the copy was removed before
                // CGlider::Run could explode it, so guests saw no explosion (#27). A copy whose pilot
                // is dead is left to the game: it explodes and frees it.
                if (e->glider && e->gen == coop_world_gen() && e->glider[OBJ_ALIVE] && !e->glider[GLIDER_DYING] &&
                    !pilot_dead(e->glider))
                    coop_remove_glider(e->glider);
                free(e->blob);
                memset(e, 0, sizeof *e);
            }
        }
        return 1;
    case MSG_NPC_ROSTER_END:
        g_roster_done = 1;
        g_last_cleanup = 0;
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

static void host_frame(void)
{
    host_scan();
    static DWORD last;
    if (GetTickCount() - last >= 1000) {
        last = GetTickCount();
        float t[2] = {*WORLD_DAYS, *WORLD_HOURS};
        net_send(NET_ALL, MSG_WORLD_TIME, t, sizeof t, 0);
    }
}

// the host's world ran a tick in a base (coop.c, #37): its NPCs go out as in flight
void mirror_base_frame(void)
{
    if (net_mode() != NET_HOST) return;
    host_frame();
    apply_player_health();
}

void mirror_frame(void)
{
    g_client = net_mode() == NET_CLIENT;
    if (!coop_in_flight()) return;
    if (net_mode() != NET_OFF) {
        check_own_death();
        send_player_health();
        apply_player_health();
        if (!g_coop_menu) hold_frame();
    }
    if (net_mode() == NET_HOST) {
        host_frame();
    } else if (g_client) {
        client_frame();
    }
    static DWORD dbg;
    if (GetTickCount() - dbg > 1000) {
        dbg = GetTickCount();
        uint8_t *mo = glider_mechanoid(G_PLAYER_GLIDER);
        if (mo && (mo[0x74] || mo[0xac] || mo[0x154] || G_PLAYER_GLIDER[GLIDER_DYING]))
            mlog("mirror: player dead flags %d %d %d dying %d", mo[0x74], mo[0xac], mo[0x154], G_PLAYER_GLIDER[GLIDER_DYING]);
    }
}

// ---- NPC simulation off on joining players ------------------------------------------------------
typedef void (THISCALL *run0_fn)(void *);
typedef void (THISCALL *run1_fn)(void *, float);
typedef unsigned (THISCALL *grouplist_run_fn)(void *, float, int, int);
static run0_fn o_ai_run;
static run1_fn o_org_run;
static grouplist_run_fn o_groups_run;

static void THISCALL h_ai_run(void *self) { if (!g_client) o_ai_run(self); }
static void THISCALL h_org_run(void *self, float dt) { if (!g_client) o_org_run(self, dt); }

// CMechGroupList::Run: group 0 is the player's; joining players run only that one
static unsigned THISCALL h_groups_run(uint8_t *list, float dt, int a, int b)
{
    if (!g_client) return o_groups_run(list, dt, a, b);
    int n = *(int *)(list + 4);
    if (n > 1) *(int *)(list + 4) = 1;
    unsigned r = o_groups_run(list, dt, a, b);
    if (n > 1) *(int *)(list + 4) = n;
    return r;
}

void mirror_install(void)
{
    static const uint8_t pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};   // push ebp; mov ebp, esp; push -1
    o_ai_run = (run0_fn)hook_jmp(0x005136a0, pro, 5, h_ai_run);               // CBasicGLAI::Run
    o_org_run = (run1_fn)hook_jmp(0x004de550, pro, 5, h_org_run);             // COrgManager::Run (hourly)
    o_groups_run = (grouplist_run_fn)hook_jmp(0x00500760, pro, 5, h_groups_run);   // CMechGroupList::Run
    o_fire = (fire_fn)hook_jmp(0x004daa90, pro, 5, h_fire);                   // CGun::FIRE
    o_damage = (damage_fn)hook_jmp(0x004fb380, pro, 5, h_damage);             // CGliderObject::Damage
    o_glider_run = (glider_run_fn)hook_jmp(0x004f9b80, pro, 5, h_glider_run);   // CGlider::Run
    static const uint8_t pro_nearest[5] = {0x83, 0xec, 0x10, 0x53, 0x55};    // sub esp, 0x10; push ebx; push ebp
    o_nearest_member = (nearest_fn)hook_jmp(0x004feb10, pro_nearest, 5, h_nearest_member);   // group: member nearest to
    mlog("mirror: installed");
}
