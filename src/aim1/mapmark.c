// A.I.M. co-op: the other players as pointers on the map screen.
//
// The map panel's list (FUN_00415130, built when the panel opens) has "Buildings" and "Pointers":
// the player, the task's bearing ("PELENG", radar +0xf05) and the map pointers scripts marked - the
// script manager's list of pointer names (count +0xe90, name32 array +0xe94). Each name is looked up
// in the object list's pointer list (+0xd0, nodes {pointer, next}; CObjectList 0x5084e0 compares the
// name at pointer +0x7c) and drawn where that pointer is. A pointer is a 0xa8-byte object (vtable,
// orientation +0x08, position +0x38 and +0x48, radius +0x78, name +0x7c).
// While the list is built, each teammate gets a pointer of ours (a copy of one of the world's, named
// after them, at their glider - or where it was last seen, e.g. while they are in a base), linked
// into the pointer list and marked in a copy of the marked list; the game's own list and array are
// put back right after. Nothing is added to the world or saved.
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>

#define A_MAP_LIST       0x00415130   // thiscall(menu): builds the map panel's list
#define A_MAP_CLICK      0x0040b730   // thiscall(menu, item): the panel's list click (vf 0x34); map mode: centre on it
#define G_SCRIPTS        (*(uint8_t **)A_TEXT_DB_PTR)
#define SM_MARKS_N       0xe90
#define SM_MARKS_P       0xe94
#define OBJLIST_POINTERS 0xd0         // CObjectList: list of map pointers {pointer, next}
#define PTR_SIZE         0xa8
#define PTR_POS          0x38
#define PTR_POS2         0x48
#define PTR_NAME         0x7c

typedef void (THISCALL *maplist_fn)(void *);
static maplist_fn o_map_list;

typedef struct {
    char name[32];
    float pos[3];
    int seen, gen;
    uint8_t ptr[PTR_SIZE];        // our pointer object (the map keeps its address while the panel is open)
    void *node[2];
} mate;
static mate g_mates[NET_MAX_PEERS];

static mate *mate_slot(const char *name)
{
    for (int i = 0; i < NET_MAX_PEERS; i++) if (g_mates[i].seen && !strcmp(g_mates[i].name, name)) return &g_mates[i];
    for (int i = 0; i < NET_MAX_PEERS; i++) if (!g_mates[i].seen) return &g_mates[i];
    return NULL;
}

// every flight frame: where the teammates are (kept while they are in a base)
void mapmark_frame(void)
{
    static DWORD last;
    if (GetTickCount() - last < 500) return;
    last = GetTickCount();
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_mates[i].seen && g_mates[i].gen != coop_world_gen()) g_mates[i].seen = 0;   // another world
    uint8_t *g;
    const char *nm;
    for (int i = 0; (g = coop_remote_by_index(i, &nm)) != NULL; i++) {
        if (!g[OBJ_ALIVE] || !nm || !nm[0]) continue;
        mate *m = mate_slot(nm);
        if (!m) continue;
        lstrcpynA(m->name, nm, sizeof m->name);
        memcpy(m->pos, g + OBJ_POS, sizeof m->pos);
        m->seen = 1;
        m->gen = coop_world_gen();
    }
}

void mapmark_reset(void) { memset(g_mates, 0, sizeof g_mates); }

// Our pointers go into the world's pointer list only around the map panel's own calls (building
// the list, and a click on an entry, which looks the name up again to centre the map on it).
static void **g_old_head;
static int link_mates(void)
{
    void ***head = (void ***)(A_OBJECT_LIST + OBJLIST_POINTERS);
    if (net_mode() == NET_OFF || !*head || !(*head)[0]) return 0;
    g_old_head = *head;
    const uint8_t *tmpl = (const uint8_t *)g_old_head[0];        // one of the world's pointers
    int added = 0;
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        mate *m = &g_mates[i];
        if (!m->seen || m->gen != coop_world_gen() || !coop_player_up(m->name)) continue;
        memcpy(m->ptr, tmpl, PTR_SIZE);
        memcpy(m->ptr + PTR_POS, m->pos, sizeof m->pos);
        memcpy(m->ptr + PTR_POS2, m->pos, sizeof m->pos);
        memset(m->ptr + PTR_NAME, 0, 32);
        lstrcpynA((char *)m->ptr + PTR_NAME, m->name, 32);
        m->node[0] = m->ptr;
        m->node[1] = *head;
        *head = m->node;
        added++;
    }
    return added;
}

static void unlink_mates(void)
{
    *(void ***)(A_OBJECT_LIST + OBJLIST_POINTERS) = g_old_head;
    *(void ***)(A_OBJECT_LIST + OBJLIST_POINTERS + 8) = NULL;   // the lookup's cursor may be on one of ours
}

static void THISCALL h_map_list(void *menu)
{
    uint8_t *sm = G_SCRIPTS;
    if (!sm) { o_map_list(menu); return; }
    int n = *(int *)(sm + SM_MARKS_N);
    name32 *old = *(name32 **)(sm + SM_MARKS_P);
    if (n < 0 || n > 200 || (n && !old)) { o_map_list(menu); return; }
    int added = link_mates();
    if (!added) { o_map_list(menu); return; }
    name32 marks[200 + NET_MAX_PEERS];
    if (n) memcpy(marks, old, n * sizeof(name32));
    int k = n;
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        mate *m = &g_mates[i];
        if (!m->seen || m->gen != coop_world_gen() || !coop_player_up(m->name)) continue;
        memset(&marks[k], 0, sizeof(name32));
        lstrcpynA(marks[k++].s, m->name, 32);
    }
    *(name32 **)(sm + SM_MARKS_P) = marks;
    *(int *)(sm + SM_MARKS_N) = k;
    o_map_list(menu);
    *(name32 **)(sm + SM_MARKS_P) = old;
    *(int *)(sm + SM_MARKS_N) = n;
    unlink_mates();
    static int nlog;
    if (nlog++ < 5) mlog("mapmark: %d player(s) on the map", added);
}

typedef void (THISCALL *mapclick_fn)(void *, int);
static mapclick_fn o_map_click;

static void THISCALL h_map_click(void *menu, int item)
{
    if (!link_mates()) { o_map_click(menu, item); return; }
    o_map_click(menu, item);
    unlink_mates();
}

// harness "mapnames": the map pointers and the marked names
void mapmark_test(void)
{
    int k = 0;
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_POINTERS); nd && k < 8; nd = (void **)nd[1], k++) {
        uint8_t *o = (uint8_t *)nd[0];
        if (o) mlog("mapnames: pointer '%.32s' at %.0f %.0f %.0f", (char *)o + PTR_NAME, *(float *)(o + PTR_POS),
                    *(float *)(o + PTR_POS + 4), *(float *)(o + PTR_POS + 8));
    }
    uint8_t *sm = G_SCRIPTS;
    int n = sm ? *(int *)(sm + SM_MARKS_N) : 0;
    for (int i = 0; i < n && i < 10; i++) mlog("mapnames: marked '%.32s'", (*(name32 **)(sm + SM_MARKS_P))[i].s);
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_mates[i].seen) mlog("mapnames: player %s last at %.0f %.0f", g_mates[i].name, g_mates[i].pos[0], g_mates[i].pos[1]);
}

void mapmark_install(void)
{
    static const uint8_t pro[7] = {0x6a, 0xff, 0x68, 0xe8, 0xb2, 0x51, 0x00};   // push -1; push 0x51b2e8
    o_map_list = (maplist_fn)hook_jmp(A_MAP_LIST, pro, sizeof pro, (void *)h_map_list);
    static const uint8_t pro2[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};             // push ebp; mov ebp, esp; push -1
    o_map_click = (mapclick_fn)hook_jmp(A_MAP_CLICK, pro2, sizeof pro2, (void *)h_map_click);
    mlog("mapmark: %s", o_map_list && o_map_click ? "co-op players on the map" : "NOT installed (unexpected code)");
}
