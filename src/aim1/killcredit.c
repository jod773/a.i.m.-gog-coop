// A.I.M. co-op: a guest's kills count for the guest (#42).
//
// CGliderObject::Damage (0x4fb380) credits a kill when the victim's pilot is dead and the attacker is
// the player's glider (0x4fb933): it calls the game's event object (*0x6769fc) vf 0x10 =
// CMMAppEvent::MechWasKilledByPlayer (0x41e2e0) with the victim's mechanoid - kill counts by rating
// and organisation, experience, organisation reactions, script events. On the host a co-op player's
// hit counts as the host player's (#22), so every guest kill raised the host's rating and statistics.
// Now, when the killing hit was a guest's (mirror.c g_kill_peer), the event still runs on the host
// (the world's reactions and task scripts stay the host's), the host's own rating / statistics are put
// back as they were, and MSG_KILL_CREDIT tells the guest, whose game runs the same event on its copy
// of that NPC: the kill counts in the guest's statistics, which its profile keeps (stats.c).
#include "coop.h"
#include "../common/net.h"
#include <stdlib.h>
#include <string.h>

#define A_MECH_KILLED 0x0041e2e0      // CMMAppEvent::MechWasKilledByPlayer(victim's mechanoid)
#define G_APP_EVENT   (*(void **)0x006769fc)

enum { MSG_KILL_CREDIT = 55 };        // char pilot[32]: you killed the NPC flown by this pilot

typedef void (THISCALL *killed_fn)(void *, void *);
static killed_fn o_killed;

extern int g_kill_peer;
uint8_t *mirror_copy_of(const char *pilot);
int stats_put_back(const uint8_t *data, int len);

static void THISCALL h_killed(void *ev, uint8_t *victim)
{
    int peer = g_kill_peer;
    if (net_mode() != NET_HOST || peer <= 0 || !victim) { o_killed(ev, victim); return; }
    int len;
    uint8_t *ours = cg_profile_extra(&len);       // the host's own rating / statistics
    o_killed(ev, victim);
    if (ours) stats_put_back(ours, len);
    free(ours);
    char pilot[32] = {0};
    lstrcpynA(pilot, (const char *)victim, sizeof pilot);   // the mechanoid's name comes first
    net_send(peer, MSG_KILL_CREDIT, pilot, sizeof pilot, 1);
    static int nlog;
    if (nlog++ < 10) mlog("killcredit: %s killed by player %d - their kill", pilot, peer);
}

int killcredit_handle(int type, int from, const uint8_t *data, int len)
{
    (void)from;
    if (type != MSG_KILL_CREDIT) return 0;
    if (len != 32 || data[31] || net_mode() != NET_CLIENT || !coop_in_flight() || g_coop_menu) return 1;
    uint8_t *g = mirror_copy_of((const char *)data);
    uint8_t *mo = g ? *(uint8_t **)(g + GLIDER_MECHANOID) : NULL;
    if (mo && G_APP_EVENT) {
        o_killed(G_APP_EVENT, mo);
        mlog("killcredit: our kill of %s counted", (const char *)data);
    } else {
        mlog("killcredit: our kill of %s - no copy of it here", (const char *)data);
    }
    return 1;
}

void killcredit_install(void)
{
    static const uint8_t pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};   // push ebp; mov ebp, esp; push -1
    o_killed = (killed_fn)hook_jmp(A_MECH_KILLED, pro, sizeof pro, (void *)h_killed);
    mlog("killcredit: %s", o_killed ? "guests' kills are theirs" : "NOT installed (unexpected code)");
}
