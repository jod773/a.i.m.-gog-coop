// A.I.M. co-op: things lying around (loot) are the host's (#27).
//
// A CTheThing (object type 0x15, 0x418 bytes) is what a destroyed glider drops - the pilot's
// mechmind (kind +0x278 = 0x101) and containers with its cargo (0x201..0x203) - and what a save
// keeps lying on the ground. Its goods are up to 10 entries at +0x27c (0x28 each: int kind,
// name32, int count; count of entries +0x40c). +0x268 = 1 while it can be picked up; a pickup
// (CTheThing::Run 0x4fc560: the glider's hold gets the goods) clears it, the thing rises for 0.5 s
// and deletes itself.
//
// Each world used to drop its own loot: a guest saw mechminds the host didn't have (a guest's copy
// of an NPC exploded there too, its "pilot" named after garbage - the mechmind "."), and none of
// the host's. Now the host sends its things (MSG_THING_ADD / _MOVE / _GONE); a guest shows copies,
// removes the things of its own world, and its copies of exploding gliders drop nothing (mirror.c).
// A guest's pickup goes to the guest's own hold and tells the host (MSG_THING_TAKEN), which takes
// the thing away there - and so from everyone.
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

enum { MSG_THING_ADD = 48, MSG_THING_GONE = 49, MSG_THING_TAKEN = 50, MSG_THING_MOVE = 51, MSG_THING_END = 52 };

#define TYPE_THING     0x15
#define SZ_THING       0x418
#define THING_ACTIVE   0x268      // byte: can be picked up
#define THING_KIND     0x278
#define THING_GOODS    0x27c      // 10 x 0x28
#define GOODS_ENTRY    0x28
#define THING_NGOODS   0x40c
#define THING_PLAYER_ONLY 0x410   // byte: only the player picks it up
#define MAXGOODS       10
#define MAXTHINGS      256
#define A_THING_CTOR   0x004edeb0 // thiscall(thing)
#define A_THING_INIT   0x004edf40 // thiscall(thing, float m[16], int kind)
#define A_THING_ADD_GOODS 0x00403ff0  // thiscall(thing, int kind, name32 (by value), int count)
#define A_THING_RUN    0x004fc560
#define A_OBJLIST_ADD  0x00507f30 // thiscall(list, obj)
#define A_OBJLIST_DEL  0x0041f450 // thiscall(list, obj): deferred delete
#define DEATH_CAM_TARGET (*(uint8_t **)0x00677e00)
#define GLIDER_TARGET  0xb98

#pragma pack(push, 1)
typedef struct { uint16_t id; int32_t kind; float pos[3]; uint8_t player_only, n; } thing_hdr;   // + n * 0x28 goods
typedef struct { uint16_t id; float pos[3]; } thing_move;
#pragma pack(pop)

typedef struct {
    int used, gen;
    uint8_t *obj;              // host: the thing; guest: our copy
    float pos[3];
    // guest
    int kind, player_only, n, taken;
    DWORD retry;               // no copy before this time
    uint8_t goods[MAXGOODS * GOODS_ENTRY];
} slot;

static slot g_t[MAXTHINGS];
static int g_end;              // guest: the host's list is complete (our own things can go)
static DWORD g_last_scan;

static int in_list(const uint8_t *o)
{
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1])
        if (nd[0] == o) return 1;
    return 0;
}

static int is_thing(const uint8_t *o)
{
    return o && o[OBJ_ALIVE] && *(const int *)(o + OBJ_TYPE) == TYPE_THING;
}

static int ours(const uint8_t *o)       // guest: one of our copies
{
    for (int i = 0; i < MAXTHINGS; i++)
        if (g_t[i].used && g_t[i].obj == o && g_t[i].gen == coop_world_gen()) return 1;
    return 0;
}

static void delete_thing(uint8_t *o)
{
    if (G_PLAYER_GLIDER && *(uint8_t **)(G_PLAYER_GLIDER + GLIDER_TARGET) == o)
        *(uint8_t **)(G_PLAYER_GLIDER + GLIDER_TARGET) = NULL;
    typedef void (THISCALL *del_fn)(void *, void *);
    CALL(A_OBJLIST_DEL, del_fn)((void *)A_OBJECT_LIST, o);
}

// ---- host -------------------------------------------------------------------------------------
static void send_add(int to, int id)
{
    slot *s = &g_t[id];
    uint8_t *o = s->obj;
    uint8_t buf[sizeof(thing_hdr) + MAXGOODS * GOODS_ENTRY];
    thing_hdr *h = (thing_hdr *)buf;
    int n = *(int *)(o + THING_NGOODS);
    if (n < 0) n = 0;
    if (n > MAXGOODS) n = MAXGOODS;
    h->id = (uint16_t)id;
    h->kind = *(int *)(o + THING_KIND);
    memcpy(h->pos, o + OBJ_POS, sizeof h->pos);
    h->player_only = o[THING_PLAYER_ONLY];
    h->n = (uint8_t)n;
    memcpy(buf + sizeof *h, o + THING_GOODS, n * GOODS_ENTRY);
    net_send(to, MSG_THING_ADD, buf, sizeof *h + n * GOODS_ENTRY, 1);
}

static void host_scan(void)
{
    DWORD now = GetTickCount();
    if (now - g_last_scan < 200) return;
    g_last_scan = now;
    int gen = coop_world_gen();
    static int seen[MAXTHINGS];
    memset(seen, 0, sizeof seen);
    uint8_t mv[2 + 32 * sizeof(thing_move)];
    int nmv = 0;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!is_thing(o) || !o[THING_ACTIVE]) continue;
        int id = -1, fr = -1;
        for (int i = 0; i < MAXTHINGS && id < 0; i++) {
            if (g_t[i].used && g_t[i].obj == o && g_t[i].gen == gen) id = i;
            else if (!g_t[i].used && fr < 0) fr = i;
        }
        if (id < 0) {
            if (fr < 0) continue;
            id = fr;
            memset(&g_t[id], 0, sizeof g_t[id]);
            g_t[id].used = 1;
            g_t[id].obj = o;
            g_t[id].gen = gen;
            memcpy(g_t[id].pos, o + OBJ_POS, sizeof g_t[id].pos);
            send_add(NET_ALL, id);
        } else {
            const float *p = (const float *)(o + OBJ_POS);
            float *q = g_t[id].pos;
            if (fabsf(p[0] - q[0]) + fabsf(p[1] - q[1]) + fabsf(p[2] - q[2]) > 2.0f && nmv < 32) {
                memcpy(q, p, sizeof g_t[id].pos);
                thing_move *m = (thing_move *)(mv + 2) + nmv++;
                m->id = (uint16_t)id;
                memcpy(m->pos, p, sizeof m->pos);
            }
        }
        seen[id] = 1;
    }
    if (nmv) {
        *(uint16_t *)mv = (uint16_t)nmv;
        net_send(NET_ALL, MSG_THING_MOVE, mv, 2 + nmv * sizeof(thing_move), 0);
    }
    for (int i = 0; i < MAXTHINGS; i++) {
        if (!g_t[i].used || seen[i]) continue;
        // picked up (still here, rising, +0x268 cleared): the guests show the pickup too - the thing
        // rising with its pickup effect and the sound - instead of it just vanishing
        uint8_t *o = g_t[i].obj;
        uint8_t msg[3];
        *(uint16_t *)msg = (uint16_t)i;
        msg[2] = g_t[i].gen == gen && in_list(o) && *(int *)(o + OBJ_TYPE) == TYPE_THING && o[OBJ_ALIVE] && !o[THING_ACTIVE];
        net_send(NET_ALL, MSG_THING_GONE, msg, 3, 1);
        memset(&g_t[i], 0, sizeof g_t[i]);
    }
}

void loot_peer_joined(int peer)
{
    if (net_mode() != NET_HOST) return;
    if (coop_in_flight()) {
        g_last_scan = 0;
        host_scan();                      // the list is current
        for (int i = 0; i < MAXTHINGS; i++)
            if (g_t[i].used && g_t[i].gen == coop_world_gen() && in_list(g_t[i].obj)) send_add(peer, i);
    }
    net_send(peer, MSG_THING_END, NULL, 0, 1);
}

// a guest picked up our thing number id: it goes away here (and so for everybody)
static void host_taken(int id, int from)
{
    if (id < 0 || id >= MAXTHINGS || !g_t[id].used || g_t[id].gen != coop_world_gen()) return;
    uint8_t *o = g_t[id].obj;
    if (!in_list(o) || !is_thing(o) || !o[THING_ACTIVE]) return;
    o[THING_ACTIVE] = 0;                  // as a pickup: rises and deletes itself
    *(float *)(o + 0x26c) = 0;
    static int nlog;
    if (nlog++ < 20) mlog("loot: thing %d (kind %x) picked up by player %d", id, *(int *)(o + THING_KIND), from);
}

// ---- guest ------------------------------------------------------------------------------------
static uint8_t *make_copy(slot *s)
{
    typedef void *(__cdecl *new_fn)(size_t);
    typedef void (THISCALL *ctor_fn)(void *);
    typedef void (THISCALL *init_fn)(void *, float *, int);
    typedef void (THISCALL *goods_fn)(void *, int, name32, int);
    typedef void (THISCALL *add_fn)(void *, void *);
    uint8_t *t = CALL(A_NEW, new_fn)(SZ_THING);
    if (!t) return NULL;
    CALL(A_THING_CTOR, ctor_fn)(t);
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(m + 12, s->pos, 12);
    CALL(A_THING_INIT, init_fn)(t, m, s->kind);
    for (int k = 0; k < s->n; k++) {
        const uint8_t *g = s->goods + k * GOODS_ENTRY;
        name32 nm;
        memcpy(&nm, g + 4, sizeof nm);
        CALL(A_THING_ADD_GOODS, goods_fn)(t, *(const int *)g, nm, *(const int *)(g + 0x24));
    }
    t[THING_PLAYER_ONLY] = (uint8_t)s->player_only;
    t[OBJ_NET] = 0;
    t[OBJ_LOCAL] = 0;
    CALL(A_OBJLIST_ADD, add_fn)((void *)A_OBJECT_LIST, t);
    return t;
}

static void guest_frame(void)
{
    int gen = coop_world_gen();
    for (int i = 0; i < MAXTHINGS; i++) {
        slot *s = &g_t[i];
        if (!s->used || s->taken) continue;
        if (s->obj && (s->gen != gen || !in_list(s->obj) || !is_thing(s->obj))) s->obj = NULL;
        if (!s->obj && (!s->retry || (int)(GetTickCount() - s->retry) >= 0)) {
            s->obj = make_copy(s);
            s->gen = gen;
            if (!s->obj) s->retry = GetTickCount() + 1000;
        }
    }
    static DWORD last;
    if (!g_end || GetTickCount() - last < 500) return;
    last = GetTickCount();
    // things of our own world: dropped here by our copies of gliders, or lying in our save
    int removed = 0;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!is_thing(o) || !o[THING_ACTIVE] || ours(o) || o == DEATH_CAM_TARGET) continue;
        delete_thing(o);
        removed++;
    }
    static int nlog;
    if (removed && nlog++ < 20) mlog("loot: removed %d things of our own world (the host's are shown)", removed);
}

// picked: someone picked it up on the host - it rises and deletes itself here as for a pickup, with
// the pickup sound (CTheThing::Run's end: Play3DSound(sound manager, SND_PICK_UP_THING, position))
#define A_PLAY3DSOUND  0x004a71c0
#define G_SOUND_MGR    (*(void **)0x005b747c)
#define SND_PICK_UP    (*(int *)0x0067727c)

static void guest_gone(int id, int picked)
{
    if (id < 0 || id >= MAXTHINGS || !g_t[id].used) return;
    slot *s = &g_t[id];
    if (!s->taken && !g_coop_menu && coop_in_flight() && s->obj && s->gen == coop_world_gen() && in_list(s->obj) &&
        is_thing(s->obj) && s->obj[THING_ACTIVE]) {
        if (picked) {
            s->obj[THING_ACTIVE] = 0;
            *(float *)(s->obj + 0x26c) = 0;
            typedef void (THISCALL *snd_fn)(void *, int, float *);
            if (G_SOUND_MGR) CALL(A_PLAY3DSOUND, snd_fn)(G_SOUND_MGR, SND_PICK_UP, (float *)(s->obj + OBJ_POS));
            static int nlog;
            if (nlog++ < 20) mlog("loot: thing %d picked up on the host - shown here", id);
        } else {
            delete_thing(s->obj);
        }
    }
    memset(s, 0, sizeof *s);
}

// CTheThing::Run: a pickup on a guest - ours (the thing is ours to take: tell the host) or a puppet's
typedef void (THISCALL *run_fn)(uint8_t *);
static run_fn o_thing_run;

static void THISCALL h_thing_run(uint8_t *t)
{
    int was = t[THING_ACTIVE];
    o_thing_run(t);
    if (!was || t[THING_ACTIVE] || net_mode() != NET_CLIENT || g_coop_menu || !G_PLAYER_GLIDER) return;
    for (int i = 0; i < MAXTHINGS; i++) {
        slot *s = &g_t[i];
        if (!s->used || s->obj != t || s->gen != coop_world_gen()) continue;
        const float *p = (const float *)(G_PLAYER_GLIDER + OBJ_POS), *q = (const float *)(t + OBJ_POS);
        float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
        if (d < 100.0f) {
            s->taken = 1;
            uint16_t id = (uint16_t)i;
            net_send(0, MSG_THING_TAKEN, &id, 2, 1);
            mlog("loot: we picked up thing %d (kind %x)", i, s->kind);
        } else {
            // one of our NPC copies took it here (they still pick things up): the host's NPC may not
            // have - it is back in a while, unless the host says it is gone
            s->obj = NULL;
            s->retry = GetTickCount() + 3000;
        }
        break;
    }
}

int loot_handle(int type, int from, const uint8_t *data, int len)
{
    switch (type) {
    case MSG_THING_ADD: {
        if (net_mode() != NET_CLIENT || len < (int)sizeof(thing_hdr)) return 1;
        const thing_hdr *h = (const thing_hdr *)data;
        if (h->id >= MAXTHINGS || h->n > MAXGOODS || len < (int)sizeof *h + h->n * GOODS_ENTRY) return 1;
        slot *s = &g_t[h->id];
        if (s->used) guest_gone(h->id, 0);                    // id reused
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->kind = h->kind;
        s->player_only = h->player_only;
        memcpy(s->pos, h->pos, sizeof s->pos);
        s->n = h->n;
        memcpy(s->goods, data + sizeof *h, h->n * GOODS_ENTRY);
        return 1;                                          // the copy is made in flight (loot_frame)
    }
    case MSG_THING_MOVE: {
        if (net_mode() != NET_CLIENT || len < 2) return 1;
        int n = *(const uint16_t *)data;
        if (len < 2 + n * (int)sizeof(thing_move)) return 1;
        const thing_move *m = (const thing_move *)(data + 2);
        for (int i = 0; i < n; i++, m++) {
            if (m->id >= MAXTHINGS || !g_t[m->id].used) continue;
            slot *s = &g_t[m->id];
            memcpy(s->pos, m->pos, sizeof s->pos);
            if (!g_coop_menu && coop_in_flight() && s->obj && !s->taken && s->gen == coop_world_gen() && in_list(s->obj) &&
                is_thing(s->obj)) {
                typedef void (THISCALL *setpos_fn)(void *, float, float, float);
                VFN(s->obj, VF_OBJ_SETPOS, setpos_fn)(s->obj, m->pos[0], m->pos[1], m->pos[2]);
            }
        }
        return 1;
    }
    case MSG_THING_GONE:
        if (net_mode() == NET_CLIENT && len >= 2) guest_gone(*(const uint16_t *)data, len >= 3 && data[2]);
        return 1;
    case MSG_THING_END:
        if (net_mode() == NET_CLIENT) { g_end = 1; mlog("loot: the host's things received"); }
        return 1;
    case MSG_THING_TAKEN:
        if (net_mode() == NET_HOST && len >= 2 && coop_in_flight() && !g_coop_menu) host_taken(*(const uint16_t *)data, from);
        return 1;
    }
    return 0;
}

void loot_frame(void)
{
    if (!coop_in_flight() || g_coop_menu) return;
    if (net_mode() == NET_HOST) host_scan();
    else if (net_mode() == NET_CLIENT) guest_frame();
}

void loot_reset(void)
{
    memset(g_t, 0, sizeof g_t);       // (the objects belong to the world: it frees them)
    g_end = 0;
}

// harness: "lootstat" - the things here and our table
void loot_test(void)
{
    int n = 0;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!o || *(int *)(o + OBJ_TYPE) != TYPE_THING) continue;
        const float *p = (const float *)(o + OBJ_POS);
        int id = -1;
        for (int i = 0; i < MAXTHINGS; i++) if (g_t[i].used && g_t[i].obj == o) id = i;
        int ng = *(int *)(o + THING_NGOODS);
        mlog("lootstat: %p kind %x active %d alive %d at %.0f %.0f %.0f id %d goods %d%s%.32s", o, *(int *)(o + THING_KIND),
             o[THING_ACTIVE], o[OBJ_ALIVE], p[0], p[1], p[2], id, ng, ng > 0 ? " first '" : "",
             ng > 0 ? (const char *)o + THING_GOODS + 4 : "");
        n++;
    }
    int used = 0;
    for (int i = 0; i < MAXTHINGS; i++) used += g_t[i].used;
    mlog("lootstat: %d things here, %d in the table, list end %d", n, used, g_end);
}

void loot_install(void)
{
    static const uint8_t pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};   // push ebp; mov ebp, esp; push -1
    o_thing_run = (run_fn)hook_jmp(A_THING_RUN, pro, 5, (void *)h_thing_run);
    mlog("loot: %s", o_thing_run ? "host's things shared" : "NOT installed (unexpected code)");
}
