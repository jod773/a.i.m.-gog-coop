// A.I.M. co-op: a joining player's own rating and statistics (#42).
//
// The game keeps them with the world, not with the pilot: rating and experience in the player's
// record (*0x5b7b38: +0x20 experience, float; +0x24 rating), the statistics in the script manager
// (*0x576328), one block the game saves as it is (+0xeb0, 0xd4 bytes): profit by robbery / trade /
// total +0xeb0..+0xeb8, losses total / upgrades / trade / ammo +0xebc..+0xec8, experience by tasks /
// trade +0xed0 / +0xed4, mechs +0xed8, mechs killed +0xedc, kills by the victim's rating +0xee0..
// (10), kills by organisation +0xf0c (20). Right after it come engine pointers (+0xf88 the %NAME
// string): never copy past +0xf84 - a first version did (35 organisations) and the guest crashed
// on its next world load.
// A guest playing in the host's world saw - and changed - the host's. They go into the extra part
// of the guest's profile and are put back whenever the host's world loads. A profile without them
// (the first join, older versions) keeps what the world has.
#include "coop.h"
#include <stdlib.h>
#include <string.h>

#define G_PLAYER_REC   (*(uint8_t **)0x005b7b38)
#define REC_EXPERIENCE 0x20
#define REC_RATING     0x24
#define G_SM           (*(uint8_t **)A_TEXT_DB_PTR)
#define SM_STATS       0xeb0
#define SM_STATS_LEN   0xd4           // as the game saves it

// "ST2\0" | i32 rating | f32 experience | 0xd4 bytes statistics
#define EXTRA_LEN (4 + 4 + 4 + SM_STATS_LEN)

uint8_t *cg_profile_extra(int *len)
{
    uint8_t *rec = G_PLAYER_REC, *sm = G_SM;
    *len = 0;
    if (!rec || !sm) return NULL;
    uint8_t *b = malloc(EXTRA_LEN);
    memcpy(b, "ST2", 4);
    memcpy(b + 4, rec + REC_RATING, 4);
    memcpy(b + 8, rec + REC_EXPERIENCE, 4);
    memcpy(b + 12, sm + SM_STATS, SM_STATS_LEN);
    *len = EXTRA_LEN;
    return b;
}

// put back what cg_profile_extra took (1 if it was ours)
int stats_put_back(const uint8_t *data, int len)
{
    uint8_t *rec = G_PLAYER_REC, *sm = G_SM;
    if (!rec || !sm || len != EXTRA_LEN || memcmp(data, "ST2", 4)) return 0;
    memcpy(rec + REC_RATING, data + 4, 4);
    memcpy(rec + REC_EXPERIENCE, data + 8, 4);
    memcpy(sm + SM_STATS, data + 12, SM_STATS_LEN);
    return 1;
}

void cg_profile_extra_restore(const uint8_t *data, int len)
{
    if (!stats_put_back(data, len)) return;
    mlog("stats: our own rating %d (experience %.0f) and statistics restored", *(int *)(G_PLAYER_REC + REC_RATING),
         (double)*(float *)(G_PLAYER_REC + REC_EXPERIENCE));
}

// harness "mystats"
void stats_test(void)
{
    uint8_t *rec = G_PLAYER_REC, *sm = G_SM;
    if (!rec || !sm) return;
    mlog("mystats: rating %d experience %.1f, mechs killed %d, profit %.0f, losses %.0f",
         *(int *)(rec + REC_RATING), (double)*(float *)(rec + REC_EXPERIENCE), *(int *)(sm + 0xedc),
         (double)*(float *)(sm + 0xeb8), (double)*(float *)(sm + 0xebc));
}

// harness "setstats <rating>": change ours (test of the save / restore)
void stats_test_set(int rating)
{
    uint8_t *rec = G_PLAYER_REC, *sm = G_SM;
    if (!rec || !sm) return;
    *(int *)(rec + REC_RATING) = rating;
    *(int *)(sm + 0xedc) += 7;
    *(float *)(sm + 0xeb8) += 1234.0f;
    stats_test();
}
