// A.I.M. co-op: the team's cargo (#83).
//
// A task's script judges the hold of whoever runs it. Base 1's first task gives the player who takes it a
// container (_ADDOBJECT) and fails for anyone who then walks into that base without one (_PLAYERHAS): a
// teammate's visit failed the task for everybody, and the host's failures count (#12). So when a script
// asks, the team's holds count as one:
// - _PLAYERHAS (0x439200) is also true when teammates carry the goods;
// - _REMOVEOBJECT (0x4390e0) takes what is missing here from a teammate's hold.
// Any teammate can deliver. Shared: goods named TOV* and mechminds (kind 2). Equipment, gliders and clans
// stay each player's own.
//
// What each player carries travels in the quest log (questlog.c), so it arrives together with the flags
// of the tasks: a player's new task never shows up without its cargo, and a delivered container never
// disappears before the flags saying it was delivered. A guest's log has its own hold, the host's has all.
// A take goes out after our own log, for the same reason, and is applied in flight after the merges.
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MSG_CARGO_TAKE = 57 };

#define A_PLAYER_HAS   0x00439200   // CMMScriptManager: thiscall(sm, name, count as text or NULL), al
#define A_REMOVE_OBJ   0x004390e0   // CMMScriptManager: thiscall(sm, name, count)
#define A_ADD_OBJ      0x0043a7b0   // CMMScriptManager: thiscall(sm, name, count)
#define A_HOLD_TAKE    0x00438870   // CGarbage: thiscall(hold, name32, count), by name, any kind
#define A_PILOT_STATS  0x004ae820   // thiscall(mechanoid): weight etc. after a hold change
#define G_PILOT        (*(uint8_t **)0x005b7b38)   // the player's mechanoid, as the scripts read it
#define G_SCRIPTS      (*(uint8_t **)A_TEXT_DB_PTR)
#define PILOT_HOLD     0x34
#define HOLD_N         4
#define HOLD_P         8
#define ENTRY_SIZE     0x2c
#define ENTRY_NAME     4
#define ENTRY_COUNT    0x24
#define KIND_MECHMIND  2

// a thing lying in the world (CTheThing): what it holds
#define TYPE_THING     0x15
#define THING_ACTIVE   0x268        // byte: can be picked up
#define THING_KIND     0x278
#define THING_MECHMIND 0x101
#define THING_GOODS    0x27c        // entries of 0x28, the name at +4
#define THING_NGOODS   0x40c

#define MAX_ITEMS 48
#define MAX_TAKES 16

typedef struct { char name[32]; int count; } item;
typedef struct { char who[32]; int peer; int n; item it[MAX_ITEMS]; } member;
typedef struct { char who[32], name[32], by[32]; int count; } take;   // also the message

static member g_team[NET_MAX_PEERS];    // by player name, kept when they leave: they may come back with it
static member g_own;                    // our hold as last seen (not while dying: the hold drops then)
static take g_out[MAX_TAKES], g_in[MAX_TAKES];
static int g_nout, g_nin;

typedef char (THISCALL *has_fn)(void *, const char *, const char *);
typedef void (THISCALL *remove_fn)(void *, const char *, int);
typedef void (THISCALL *hold_take_fn)(void *, name32, int);
typedef void (THISCALL *pilot_fn)(void *);
static has_fn o_has;
static remove_fn o_remove;

// the n-th entry of our hold
static uint8_t *hold_entry(int i)
{
    uint8_t *mo = G_PILOT;
    if (!mo) return NULL;
    int n = *(int *)(mo + PILOT_HOLD + HOLD_N);
    uint8_t *a = *(uint8_t **)(mo + PILOT_HOLD + HOLD_P);
    if (i < 0 || i >= n || n > 500 || !a) return NULL;
    return a + i * ENTRY_SIZE;
}

static int shared(const uint8_t *e)
{
    return *(const int *)e == KIND_MECHMIND || !strncmp((const char *)e + ENTRY_NAME, "TOV", 3);
}

static int in_hold(const char *name)
{
    int n = 0;
    uint8_t *e;
    for (int i = 0; (e = hold_entry(i)) != NULL; i++)
        if (shared(e) && !strcmp((const char *)e + ENTRY_NAME, name)) n += *(int *)(e + ENTRY_COUNT);
    return n;
}

static void read_own_hold(void)
{
    lstrcpynA(g_own.who, coop_my_name(), sizeof g_own.who);
    if (!G_PILOT || (coop_in_flight() && !cg_pilot_saveable())) return;
    g_own.n = 0;
    uint8_t *e;
    for (int i = 0; (e = hold_entry(i)) != NULL && g_own.n < MAX_ITEMS; i++) {
        if (!shared(e)) continue;
        item *t = &g_own.it[g_own.n++];
        lstrcpynA(t->name, (const char *)e + ENTRY_NAME, sizeof t->name);
        t->count = *(int *)(e + ENTRY_COUNT);
    }
}

static int team_count(const char *name)
{
    int n = 0;
    for (int m = 0; m < NET_MAX_PEERS; m++)
        for (int i = 0; g_team[m].who[0] && i < g_team[m].n; i++)
            if (!strcmp(g_team[m].it[i].name, name)) n += g_team[m].it[i].count;
    return n;
}

static member *team_member(const char *who, int create)
{
    member *free_slot = NULL;
    for (int m = 0; m < NET_MAX_PEERS; m++) {
        if (!g_team[m].who[0]) { if (!free_slot) free_slot = &g_team[m]; continue; }
        if (!lstrcmpiA(g_team[m].who, who)) return &g_team[m];
    }
    if (free_slot && create) {
        memset(free_slot, 0, sizeof *free_slot);
        lstrcpynA(free_slot->who, who, sizeof free_slot->who);
        free_slot->peer = -1;
    }
    return create ? free_slot : NULL;
}

// ---- the log's section ---------------------------------------------------------------------------
// u16 members, each: who 32, u16 items, items * (name 32, i32 count)
static int save_member(uint8_t *out, const member *m)
{
    int pos = 0;
    memcpy(out, m->who, 32); pos += 32;
    *(uint16_t *)(out + pos) = (uint16_t)m->n; pos += 2;
    for (int i = 0; i < m->n; i++) {
        memcpy(out + pos, m->it[i].name, 32); pos += 32;
        memcpy(out + pos, &m->it[i].count, 4); pos += 4;
    }
    return pos;
}

int cargo_save_size(void) { return 2 + (NET_MAX_PEERS + 1) * (34 + MAX_ITEMS * 36); }

int cargo_save(uint8_t *out)
{
    read_own_hold();
    int pos = 2, n = 1;
    pos += save_member(out + pos, &g_own);
    if (net_mode() == NET_HOST)
        for (int m = 0; m < NET_MAX_PEERS; m++)
            if (g_team[m].who[0]) { pos += save_member(out + pos, &g_team[m]); n++; }
    *(uint16_t *)out = (uint16_t)n;
    return pos;
}

void cargo_load(const uint8_t *b, int len, int from_guest, int from)
{
    if (len < 2) return;
    int n = *(const uint16_t *)b, pos = 2;
    for (int k = 0; k < n && pos + 34 <= len; k++) {
        char who[32];
        memcpy(who, b + pos, 32);
        who[31] = 0;
        pos += 32;
        int items = *(const uint16_t *)(b + pos);
        pos += 2;
        if (pos + items * 36 > len) return;
        const uint8_t *list = b + pos;
        pos += items * 36;
        if (!who[0] || !lstrcmpiA(who, coop_my_name())) continue;
        member *m = team_member(who, 1);
        if (!m) continue;
        if (from_guest) m->peer = from;
        m->n = items < MAX_ITEMS ? items : MAX_ITEMS;
        for (int i = 0; i < m->n; i++) {
            memcpy(m->it[i].name, list + i * 36, 32);
            m->it[i].name[31] = 0;
            memcpy(&m->it[i].count, list + i * 36 + 32, 4);
        }
    }
}

// A job's target was killed and its mechmind lies there: "killed, and nobody has it" fails the job in the
// base that gave it (T_L1_B1_3, T_L2_B2_3, T_L2_B2_4, T_L5_CARR_3). One player flying into that base while the
// other is on the way to pick it up must not fail it.
static int mind_lying(const char *name)
{
    typedef int (THISCALL *flag_find_fn)(void *, const char *);
    char killed[64];
    snprintf(killed, sizeof killed, "%.40s.KILLED", name);     // only a killed pilot's: a job's own "go and fetch
    if (!G_SCRIPTS || CALL(0x00404620, flag_find_fn)(G_SCRIPTS + 0xbd4, killed) == -1) return 0;   // these" stays a job
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (o && o[OBJ_ALIVE] && *(int *)(o + OBJ_TYPE) == TYPE_THING && *(int *)(o + THING_KIND) == THING_MECHMIND &&
            o[THING_ACTIVE] && *(int *)(o + THING_NGOODS) > 0 && !strcmp((const char *)o + THING_GOODS + 4, name))
            return 1;
    }
    return 0;
}

// ---- the scripts' two commands -------------------------------------------------------------------
static char THISCALL h_has(void *sm, const char *name, const char *count)
{
    char r = o_has(sm, name, count);
    if (r || net_mode() == NET_OFF || !name) return r;
    if (mind_lying(name)) {
        static int nlog;
        if (nlog++ < 20) mlog("cargo: %s's mechmind lies in the world - it counts", name);
        return 1;
    }
    int theirs = team_count(name);
    if (!theirs) return r;
    int need = count ? atoi(count) : 1;
    if (in_hold(name) + theirs < (need > 0 ? need : 1)) return r;
    static int nlog;
    if (nlog++ < 30) mlog("cargo: %s counts for us, a teammate carries it", name);
    return 1;
}

static void THISCALL h_remove(void *sm, const char *name, int count)
{
    int missing = net_mode() != NET_OFF && name ? count - in_hold(name) : 0;
    o_remove(sm, name, count);
    if (missing <= 0) return;
    for (int m = 0; m < NET_MAX_PEERS && missing > 0; m++) {
        member *t = &g_team[m];
        for (int i = 0; t->who[0] && i < t->n && missing > 0; i++) {
            if (strcmp(t->it[i].name, name) || t->it[i].count <= 0 || g_nout >= MAX_TAKES) continue;
            int k = t->it[i].count < missing ? t->it[i].count : missing;
            take *o = &g_out[g_nout++];
            memset(o, 0, sizeof *o);
            lstrcpynA(o->who, t->who, sizeof o->who);
            lstrcpynA(o->name, name, sizeof o->name);
            lstrcpynA(o->by, coop_my_name(), sizeof o->by);
            o->count = k;
            t->it[i].count -= k;
            missing -= k;
            mlog("cargo: %d x %s comes out of %s's hold", k, name, t->who);
        }
    }
}

// ---- takes -----------------------------------------------------------------------------------------
int cargo_takes_waiting(void) { return g_nout; }

static void send_take(const take *t)
{
    if (net_mode() == NET_CLIENT) {
        net_send(0, MSG_CARGO_TAKE, t, sizeof *t, 1);
        return;
    }
    member *m = team_member(t->who, 0);
    if (m && m->peer >= 0) net_send(m->peer, MSG_CARGO_TAKE, t, sizeof *t, 1);
}

static void apply_take(const take *t)
{
    int have = in_hold(t->name), k = t->count < have ? t->count : have;
    if (k <= 0 || !G_PILOT) {
        mlog("cargo: %s delivered %s, but it is not in our hold any more", t->by, t->name);
        return;
    }
    name32 nm;
    memset(&nm, 0, sizeof nm);
    lstrcpynA(nm.s, t->name, sizeof nm.s);
    CALL(A_HOLD_TAKE, hold_take_fn)(G_PILOT + PILOT_HOLD, nm, k);
    CALL(A_PILOT_STATS, pilot_fn)(G_PILOT);
    mlog("cargo: %s took %d x %s from our hold", t->by, k, t->name);
    char msg[160];
    snprintf(msg, sizeof msg, aim1_ui("cargo_taken"), t->by);
    coop_notify(msg);
}

// in flight, after the merges and after our own log went out
void cargo_frame(void)
{
    for (int i = 0; i < g_nout; i++) send_take(&g_out[i]);
    g_nout = 0;
    for (int i = 0; i < g_nin; i++) apply_take(&g_in[i]);
    g_nin = 0;
}

int cargo_handle(int type, int from, const uint8_t *data, int len)
{
    (void)from;
    if (type != MSG_CARGO_TAKE) return 0;
    if (len < (int)sizeof(take)) return 1;
    const take *t = (const take *)data;
    if (!lstrcmpiA(t->who, coop_my_name())) {
        if (g_nin < MAX_TAKES) g_in[g_nin++] = *t;
    } else if (net_mode() == NET_HOST) {                 // for another guest: our table follows it too
        member *m = team_member(t->who, 0);
        for (int i = 0; m && i < m->n; i++)
            if (!strcmp(m->it[i].name, t->name)) m->it[i].count -= m->it[i].count < t->count ? m->it[i].count : t->count;
        send_take(t);
    }
    return 1;
}

void cargo_reset(void)
{
    memset(g_team, 0, sizeof g_team);
    memset(&g_own, 0, sizeof g_own);
    g_nout = g_nin = 0;
}

void cargo_install(void)
{
    static const uint8_t has_pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};                     // push ebp; mov ebp, esp; push -1
    static const uint8_t rem_pro[7] = {0x8b, 0x44, 0x24, 0x08, 0x83, 0xec, 0x20};         // mov eax, [esp+8]; sub esp, 0x20
    o_has = (has_fn)hook_jmp(A_PLAYER_HAS, has_pro, sizeof has_pro, (void *)h_has);
    o_remove = (remove_fn)hook_jmp(A_REMOVE_OBJ, rem_pro, sizeof rem_pro, (void *)h_remove);
    mlog("cargo: team cargo %s", o_has && o_remove ? "on" : "NOT on (unexpected code)");
}

// harness: "cargostat", "cargoadd <name> [count]", as a script's _ADDOBJECT (in flight)
void cargo_test(const char *cmd, const char *arg)
{
    if (!strcmp(cmd, "cargoadd") && arg[0] && G_SCRIPTS) {
        char name[64];
        int n = 1;
        if (sscanf(arg, "%63s %d", name, &n) >= 1) {
            typedef void (THISCALL *add_fn)(void *, const char *, int);
            CALL(A_ADD_OBJ, add_fn)(G_SCRIPTS, name, n);
        }
        return;
    }
    uint8_t *e;
    char line[400];
    int pos = snprintf(line, sizeof line, "cargo: %.0f crystals, glider %.32s, our hold:", G_PILOT ? (double)*(float *)(G_PILOT + PILOT_HOLD) : 0.0,
                       (const char *)G_PLAYER_MECHANOID + MECHANOID_TYPE + 4);
    for (int i = 0; (e = hold_entry(i)) != NULL && pos < 340; i++)
        pos += snprintf(line + pos, sizeof line - pos, " %.32s x%d%s", (const char *)e + ENTRY_NAME, *(int *)(e + ENTRY_COUNT),
                        *(int *)e == KIND_MECHMIND ? " (mechmind)" : "");
    mlog("%s", line);
    for (int m = 0; m < NET_MAX_PEERS; m++) {
        if (!g_team[m].who[0]) continue;
        pos = snprintf(line, sizeof line, "cargo: %s carries:", g_team[m].who);
        for (int i = 0; i < g_team[m].n && pos < 340; i++)
            pos += snprintf(line + pos, sizeof line - pos, " %s x%d", g_team[m].it[i].name, g_team[m].it[i].count);
        mlog("%s", line);
    }
}
