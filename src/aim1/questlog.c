// A.I.M. co-op: shared quest log.
//
// The log lives in the script manager (*0x576328): the quest list (0x54-byte
// entries, +0x1cdc count / +0x1ce0 array), the journal (32-byte ids, +0x1ce4 /
// +0x1ce8) and script flags such as "<quest>.COMPLETE" / ".FAIL" (names in
// the flag table at +0xbd4). Logs are merged by union: whatever one player
// has, the others get, through the game's own functions (so a teammate's
// completed quest plays the usual "quest complete" feedback). Joining players
// send their log up, the host merges and sends the result to everyone.
#include "coop.h"
#include "../common/net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum { MSG_LOG = 40, MSG_LOG_UP = 41, MSG_SAY_ = 45 /* coop_core's notice */ };

#define G_SCRIPTS      (*(uint8_t **)A_TEXT_DB_PTR)
#define SM_FLAGS       0xbd4
#define SM_QUEST_N     0x1cdc
#define SM_QUEST_P     0x1ce0
#define SM_JOURNAL_N   0x1ce4
#define SM_JOURNAL_P   0x1ce8
#define QUEST_SIZE     0x54
#define JOURNAL_SIZE   0x20


#define A_SET_FLAG     0x0043ac60   // CMMScriptManager: set a script flag; "<quest>.FAIL" fails the quest

typedef int (THISCALL *flag_find_fn)(void *, const char *);
typedef void (THISCALL *str_fn)(void *, const char *);

static str_fn o_set_flag;
static int g_from_host;              // log_merge is applying the host's log

static uint8_t *g_pending;
static int g_pending_len, g_pending_from;
static uint32_t g_known;
static DWORD g_last_tick;

static int has_flag(const char *name)
{
    return CALL(0x00404620, flag_find_fn)(G_SCRIPTS + SM_FLAGS, name) != -1;
}

// The flag table (script manager +0xbd4, as CMMScriptManager's set-flag 0x43ac60 / find 0x404620
// use it): +4 text, +8 text used, +0xc count, +0x14 offsets of the names in the text.
static const char *flag_name(int i, int *n)
{
    uint8_t *t = G_SCRIPTS + SM_FLAGS;
    const char *text = *(const char **)(t + 4);
    const int *offs = *(const int **)(t + 0x14);
    *n = *(int *)(t + 0xc);
    if (i < 0 || i >= *n || *n > 100000 || !text || !offs) return NULL;
    return text + offs[i];
}

static uint32_t hash(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// ---- the host's removed flags ---------------------------------------------------------------
// Scripts also take flags away (REMOVEEVENT -> 0x43a660); a merge by union never did, so a guest
// kept e.g. a task's "not complete yet" flag after the host's script had removed it. The host
// remembers what it had at its last look; names gone since are "removed" (until they come back).
#define A_REMOVE_FLAG  0x0043a660   // thiscall(script manager, name)
static char **g_had, **g_removed;
static int g_nhad, g_nremoved;

static int in_list_of(char **l, int n, const char *s)
{
    for (int i = 0; i < n; i++) if (!strcmp(l[i], s)) return i;
    return -1;
}

static void host_track_removed(void)
{
    int n;
    const char *f;
    for (int i = 0; i < g_nhad; i++) {
        if (has_flag(g_had[i]) || in_list_of(g_removed, g_nremoved, g_had[i]) >= 0 || strlen(g_had[i]) > 255) continue;
        g_removed = realloc(g_removed, (g_nremoved + 1) * sizeof *g_removed);
        g_removed[g_nremoved++] = _strdup(g_had[i]);
    }
    for (int i = 0; i < g_nremoved; i++)
        if (has_flag(g_removed[i])) { free(g_removed[i]); g_removed[i--] = g_removed[--g_nremoved]; }
    for (int i = 0; i < g_nhad; i++) free(g_had[i]);
    free(g_had);
    g_had = NULL;
    g_nhad = 0;
    flag_name(0, &n);
    if (n <= 0 || n > 100000) return;
    g_had = malloc(n * sizeof *g_had);
    for (int i = 0; (f = flag_name(i, &n)) != NULL && g_nhad < n; i++) g_had[g_nhad++] = _strdup(f);
}

static void removed_reset(void)
{
    for (int i = 0; i < g_nhad; i++) free(g_had[i]);
    for (int i = 0; i < g_nremoved; i++) free(g_removed[i]);
    free(g_had); free(g_removed);
    g_had = g_removed = NULL;
    g_nhad = g_nremoved = 0;
}

// ---- task timers ----------------------------------------------------------------------------
// A timed task's countdown is a timer of the same name in the script manager's timer list (+0xe98
// count, +0xe9c array of 0x4c: name 0x20, +0x20 length, +0x24 deadline in world days, +0x28 the
// script it was started from 0x20, +0x48). STARTTIMER (0x439c70) also writes the deadline into the
// task's entry (+0x50), which the HUD (0x441fe0) counts down while it lies ahead of the world clock;
// when the timer runs out the game runs "<name>_TIMEOUT" from that script. A task merged in by name
// (0x43adf0) starts with no deadline, so the teammate saw no timer. The clock is the host's
// everywhere (mirror.c), so deadlines go across as they are; the host's deadline wins.
#define SM_TIMER_N     0xe98
#define SM_TIMER_P     0xe9c
#define TIMER_SIZE     0x4c
#define TIMER_DEADLINE 0x24
#define QUEST_DEADLINE 0x50
#define A_START_TIMER  0x00439c70   // thiscall(script manager, name, hours text)
#define A_NEXT_TIMER   0x00438530   // thiscall(script manager): picks the timer that runs out first
#define WORLD_DAYS     (*(float *)0x0067661c)

typedef void (THISCALL *start_timer_fn)(void *, const char *, const char *);
typedef void (THISCALL *void_fn)(void *);

static uint8_t *quest_entry(const char *name)
{
    uint8_t *sm = G_SCRIPTS;
    int n = *(int *)(sm + SM_QUEST_N);
    uint8_t *l = *(uint8_t **)(sm + SM_QUEST_P);
    if (n <= 0 || n > 2000 || !l) return NULL;
    for (int i = 0; i < n; i++) if (!strncmp((const char *)l + i * QUEST_SIZE, name, QUEST_SIZE - 4)) return l + i * QUEST_SIZE;
    return NULL;
}

static uint8_t *timer_entry(int i)
{
    uint8_t *sm = G_SCRIPTS;
    int n = *(int *)(sm + SM_TIMER_N);
    uint8_t *l = *(uint8_t **)(sm + SM_TIMER_P);
    if (i < 0 || i >= n || n > 1000 || !l) return NULL;
    return l + i * TIMER_SIZE;
}

static uint8_t *timer_named(const char *name)
{
    uint8_t *t;
    for (int i = 0; (t = timer_entry(i)) != NULL; i++) if (!strncmp((const char *)t, name, 0x20)) return t;
    return NULL;
}

static int task_over(const char *q)
{
    char f[0x60];
    snprintf(f, sizeof f, "%s.COMPLETE", q);
    if (has_flag(f)) return 1;
    snprintf(f, sizeof f, "%s.FAIL", q);
    return has_flag(f);
}

// a timer that belongs to a task in our log (other script timers stay each world's own)
static int task_timer(const uint8_t *t)
{
    char name[0x21];
    memcpy(name, t, 0x20);
    name[0x20] = 0;
    return name[0] && quest_entry(name) && !task_over(name);
}

// blob: "QL1\0" u16 nq, nq * 0x54 | u16 nj, nj * 0x20 | u16 nf, nf * (u8 len, name) | u16 nr, removed names
//       | u16 nt, nt * 0x4c task timers
static uint8_t *log_save(int *len)
{
    uint8_t *sm = G_SCRIPTS;
    if (!sm) return NULL;
    int nq = *(int *)(sm + SM_QUEST_N), nj = *(int *)(sm + SM_JOURNAL_N);
    const uint8_t *q = *(uint8_t **)(sm + SM_QUEST_P), *j = *(uint8_t **)(sm + SM_JOURNAL_P);
    if (nq < 0 || nq > 2000 || nj < 0 || nj > 4000 || (nq && !q) || (nj && !j)) return NULL;
    uint8_t *out = malloc(4 + 2 + nq * QUEST_SIZE + 2 + nj * JOURNAL_SIZE + 2 + (nq + nj) * 3 * (QUEST_SIZE + 16));
    int pos = 0;
    memcpy(out, "QL1", 4);
    pos = 4;
    *(uint16_t *)(out + pos) = (uint16_t)nq; pos += 2;
    memcpy(out + pos, q, nq * QUEST_SIZE); pos += nq * QUEST_SIZE;
    *(uint16_t *)(out + pos) = (uint16_t)nj; pos += 2;
    memcpy(out + pos, j, nj * JOURNAL_SIZE); pos += nj * JOURNAL_SIZE;
    int fpos = pos;
    pos += 2;
    int nf = 0;
    // All script flags, from every player: the story is everyone's. The first "quest"
    // (TS_L1_FIND_BASE, "Get to the base") is a journal entry, not a task, and the story events that
    // come with a task (a base's dialogue pages) are flags of their own - a guest's own completion
    // didn't reach the host with only the tasks' .GET / .COMPLETE.
    int n, need = 0;
    const char *f;
    for (int i = 0; (f = flag_name(i, &n)) != NULL; i++) need += (int)strlen(f) + 1;
    if (net_mode() == NET_HOST) {
        host_track_removed();
        for (int i = 0; i < g_nremoved; i++) need += (int)strlen(g_removed[i]) + 1;
    }
    out = realloc(out, pos + need + 16);
    for (int i = 0; (f = flag_name(i, &n)) != NULL; i++) {
        int l = (int)strlen(f);
        if (l <= 0 || l > 255) continue;
        out[pos++] = (uint8_t)l;
        memcpy(out + pos, f, l);
        pos += l;
        if (++nf == 0xffff) break;
    }
    *(uint16_t *)(out + fpos) = (uint16_t)nf;
    // the host: flags its scripts removed (REMOVEEVENT) - a guest that still has them drops them
    int rpos = pos, nr = 0;
    pos += 2;
    if (net_mode() == NET_HOST)
        for (int i = 0; i < g_nremoved && nr < 0xffff; i++) {
            int l = (int)strlen(g_removed[i]);
            out[pos++] = (uint8_t)l;
            memcpy(out + pos, g_removed[i], l);
            pos += l;
            nr++;
        }
    *(uint16_t *)(out + rpos) = (uint16_t)nr;
    // the running task timers
    int nt = 0, tpos;
    const uint8_t *t;
    for (int i = 0; (t = timer_entry(i)) != NULL; i++) nt += task_timer(t);
    out = realloc(out, pos + 2 + nt * TIMER_SIZE);
    tpos = pos;
    pos += 2;
    nt = 0;
    for (int i = 0; (t = timer_entry(i)) != NULL; i++)
        if (task_timer(t)) { memcpy(out + pos, t, TIMER_SIZE); pos += TIMER_SIZE; nt++; }
    *(uint16_t *)(out + tpos) = (uint16_t)nt;
    *len = pos;
    return out;
}

// Adds whatever the blob has and we don't. Returns the number of additions.
static int log_merge(const uint8_t *b, int len, int from_guest)
{
    uint8_t *sm = G_SCRIPTS;
    if (!sm || len < 10 || memcmp(b, "QL1", 4)) return 0;
    int pos = 4, added = 0;
    int nq = *(const uint16_t *)(b + pos); pos += 2;
    if (pos + nq * QUEST_SIZE + 2 > len) return 0;
    const uint8_t *q = b + pos; pos += nq * QUEST_SIZE;
    int nj = *(const uint16_t *)(b + pos); pos += 2;
    if (pos + nj * JOURNAL_SIZE + 2 > len) return 0;
    const uint8_t *j = b + pos; pos += nj * JOURNAL_SIZE;
    int nf = *(const uint16_t *)(b + pos); pos += 2;

    for (int i = 0; i < nq; i++) {
        char name[QUEST_SIZE];
        memcpy(name, q + i * QUEST_SIZE, QUEST_SIZE - 4);
        name[QUEST_SIZE - 4] = 0;
        if (!name[0]) continue;
        int have = 0, n = *(int *)(sm + SM_QUEST_N);
        const uint8_t *mine = *(uint8_t **)(sm + SM_QUEST_P);
        for (int k = 0; k < n && !have; k++) have = !strncmp((const char *)mine + k * QUEST_SIZE, name, QUEST_SIZE - 4);
        if (have) continue;
        CALL(0x0043adf0, str_fn)(sm, name);           // new quest: list entry, ".GET", sound
        added++;
    }
    for (int i = 0; i < nj; i++) {
        char id[JOURNAL_SIZE + 1];
        memcpy(id, j + i * JOURNAL_SIZE, JOURNAL_SIZE);
        id[JOURNAL_SIZE] = 0;
        if (!id[0]) continue;
        int have = 0, n = *(int *)(sm + SM_JOURNAL_N);
        const uint8_t *mine = *(uint8_t **)(sm + SM_JOURNAL_P);
        for (int k = 0; k < n && !have; k++) have = !strncmp((const char *)mine + k * JOURNAL_SIZE, id, JOURNAL_SIZE);
        if (have) continue;
        CALL(0x00437ea0, str_fn)(sm, id);             // journal entry
        added++;
    }
    for (int i = 0; i < nf && pos < len; i++) {
        int l = b[pos++];
        if (pos + l > len) break;
        char name[256];
        memcpy(name, b + pos, l);
        name[l] = 0;
        pos += l;
        if (has_flag(name)) continue;
        // a failed task comes from the host only: a joining player's copy of the world can run out a
        // task's time on its own (it was in a base, it rejoined) and must not fail it for everybody
        if (from_guest && strstr(name, ".FAIL")) continue;
        g_from_host = !from_guest;
        CALL(A_SET_FLAG, str_fn)(sm, name);           // flag (".COMPLETE" / ".FAIL" show the result)
        g_from_host = 0;
        added++;
    }
    // the host's removed flags (a guest's log has none)
    int nr = !from_guest && pos + 2 <= len ? *(const uint16_t *)(b + pos) : 0;
    pos += 2;
    for (int i = 0; i < nr && pos < len; i++) {
        int l = b[pos++];
        if (pos + l > len) break;
        char name[256];
        memcpy(name, b + pos, l);
        name[l] = 0;
        pos += l;
        if (!has_flag(name)) continue;
        CALL(A_REMOVE_FLAG, str_fn)(sm, name);
        static int nlog;
        if (nlog++ < 20) mlog("questlog: '%s' removed (as on the host)", name);
        added++;
    }
    // task timers (after the tasks and flags: a timer goes with a task we have and that still runs)
    int nt = pos + 2 <= len ? *(const uint16_t *)(b + pos) : 0, timers = 0;
    pos += 2;
    for (int i = 0; i < nt && pos + TIMER_SIZE <= len; i++, pos += TIMER_SIZE) {
        const uint8_t *t = b + pos;
        float deadline = *(const float *)(t + TIMER_DEADLINE);
        char name[0x21];
        memcpy(name, t, 0x20);
        name[0x20] = 0;
        if (!name[0] || !quest_entry(name) || task_over(name)) continue;
        uint8_t *mine = timer_named(name);
        if (!mine) {
            if (deadline <= WORLD_DAYS) continue;
            CALL(A_START_TIMER, start_timer_fn)(sm, name, "0");
            if (!(mine = timer_named(name))) continue;
            memcpy(mine + 0x20, t + 0x20, TIMER_SIZE - 0x20);    // the starter's length, deadline, script
        } else if (from_guest || *(float *)(mine + TIMER_DEADLINE) == deadline) {
            continue;                                             // the host's own timer wins
        } else {
            *(float *)(mine + TIMER_DEADLINE) = deadline;
        }
        *(float *)(quest_entry(name) + QUEST_DEADLINE) = deadline;   // what the HUD counts down
        static int nlog;
        if (nlog++ < 20) mlog("questlog: timer '%s' %.1f game min left (as on player %d)", name,
                              (deadline - WORLD_DAYS) * 1440.0f, from_guest ? g_pending_from : 0);
        timers++;
    }
    if (timers) CALL(A_NEXT_TIMER, void_fn)(sm);
    return added + timers;
}

static void remember(void)
{
    int l;
    uint8_t *b = log_save(&l);
    if (b) { g_known = hash(b, l); free(b); }
}

// the log as last seen here (after our own changes and the merges): what is new in ours since
// then was done by this player and is announced to the others
static uint8_t *g_prev;
static int g_prev_len;

static void set_prev(uint8_t *b, int len) { free(g_prev); g_prev = b; g_prev_len = len; }

// the flags / journal ids of a blob, one by one
static int blob_items(const uint8_t *b, int len, int journal, int idx, char *out, int n)
{
    if (!b || len < 10) return 0;
    int pos = 4, nq = *(const uint16_t *)(b + pos);
    pos += 2 + nq * QUEST_SIZE;
    if (pos + 2 > len) return 0;
    int nj = *(const uint16_t *)(b + pos);
    pos += 2;
    if (journal) {
        if (idx >= nj || pos + (idx + 1) * JOURNAL_SIZE > len) return 0;
        lstrcpynA(out, (const char *)b + pos + idx * JOURNAL_SIZE, n < JOURNAL_SIZE + 1 ? n : JOURNAL_SIZE + 1);
        return 1;
    }
    pos += nj * JOURNAL_SIZE;
    if (pos + 2 > len) return 0;
    int nf = *(const uint16_t *)(b + pos);
    pos += 2;
    for (int i = 0; i < nf && pos < len; i++) {
        int l = b[pos++];
        if (pos + l > len) return 0;
        if (i == idx) { int k = l < n - 1 ? l : n - 1; memcpy(out, b + pos, k); out[k] = 0; return 1; }
        pos += l;
    }
    return 0;
}

static int blob_has(const uint8_t *b, int len, int journal, const char *item)
{
    char x[256];
    for (int i = 0; blob_items(b, len, journal, i, x, sizeof x); i++) if (!strcmp(x, item)) return 1;
    return 0;
}

// this player's own progress since the last look: a notice for everybody
static void announce(const uint8_t *b, int len)
{
    static int gen = -1;
    int g = coop_world_gen();
    if (g != gen) { gen = g; return; }       // a new world (joined, loaded): nothing of it was done just now
    if (!g_prev) return;
    char x[256], msg[160];
    int quests = 0, done = 0, failed = 0, journal = 0;
    for (int i = 0; blob_items(b, len, 0, i, x, sizeof x); i++) {
        if (blob_has(g_prev, g_prev_len, 0, x)) continue;
        if (strstr(x, ".COMPLETE")) done++;
        else if (strstr(x, ".FAIL")) failed++;
        else if (strstr(x, ".GET")) quests++;
    }
    for (int i = 0; blob_items(b, len, 1, i, x, sizeof x); i++)
        if (!blob_has(g_prev, g_prev_len, 1, x)) journal++;
    const char *me = coop_my_name();
    if (done) snprintf(msg, sizeof msg, aim1_ui("ql_done"), me);
    else if (failed) snprintf(msg, sizeof msg, aim1_ui("ql_failed"), me);
    else if (quests) snprintf(msg, sizeof msg, aim1_ui("ql_new"), me);
    else if (journal) snprintf(msg, sizeof msg, aim1_ui("ql_journal"), me);
    else return;
    net_send(NET_ALL, MSG_SAY_, msg, (int)strlen(msg) + 1, 1);
}

// our changes go out at once, in flight, in bases and in dialogues
static void send_changes(void)
{
    DWORD now = GetTickCount();
    if (now - g_last_tick < 500) return;
    g_last_tick = now;
    int len;
    uint8_t *b = log_save(&len);
    if (!b) return;
    uint32_t h = hash(b, len);
    if (h != g_known) {
        g_known = h;
        announce(b, len);
        if (net_mode() == NET_HOST) net_send(NET_ALL, MSG_LOG, b, len, 1);
        else net_send(0, MSG_LOG_UP, b, len, 1);
        set_prev(b, len);
        return;
    }
    if (!g_prev) set_prev(b, len);
    else free(b);
}

void questlog_frame(void)
{
    if (net_mode() == NET_OFF || !coop_in_flight()) return;
    if (g_pending) {                                  // others' progress: merged in flight only
        int n = log_merge(g_pending, g_pending_len, net_mode() == NET_HOST);
        if (n) mlog("questlog: %d entries from player %d", n, g_pending_from);
        free(g_pending);
        g_pending = NULL;
        if (n) {
            int len;
            uint8_t *b = log_save(&len);
            if (b) {
                if (net_mode() == NET_CLIENT) g_known = hash(b, len);   // the host already has them
                set_prev(b, len);                                         // not ours to announce
            }
        }
    }
    send_changes();
}

// in bases and dialogues (the menu loops): send our changes
void questlog_menu_tick(void)
{
    if (net_mode() == NET_OFF || !G_SCRIPTS || !G_APP || *(int *)(G_APP + APP_MODE) == 0) return;
    send_changes();
}

void questlog_peer_joined(int peer)
{
    int len;
    uint8_t *b = log_save(&len);
    if (b) { net_send(peer, MSG_LOG, b, len, 1); free(b); }
}

int questlog_handle(int type, int from, const uint8_t *data, int len)
{
    if (type != MSG_LOG && type != MSG_LOG_UP) return 0;
    if ((type == MSG_LOG && net_mode() != NET_CLIENT) || (type == MSG_LOG_UP && net_mode() != NET_HOST)) return 1;
    free(g_pending);
    g_pending = malloc(len);
    memcpy(g_pending, data, len);
    g_pending_len = len;
    g_pending_from = from;
    return 1;
}

void questlog_reset(void)
{
    free(g_pending);
    g_pending = NULL;
    g_known = 0;
    set_prev(NULL, 0);
    removed_reset();
}

// On a joining player, tasks fail only through the host's log (#12). The guest's own copy of the
// world runs the task scripts too (RunFunction / AddEvent -> set flag), but judges them with its own
// cargo and position: e.g. it fails the host's delivery task when the guest enters the target base.
// So a "<quest>.FAIL" set by the guest's scripts is dropped; the host's merge (g_from_host) still
// sets it, and so do the guest's tasks once it plays alone again (NET_OFF).
static void THISCALL set_flag(void *sm, const char *name)
{
    if (net_mode() == NET_CLIENT && !g_from_host && name && strstr(name, ".FAIL") && !has_flag(name)) {
        static int nlog;
        if (nlog++ < 10) mlog("questlog: '%s' failed here only - left to the host", name);
        // the task's script may already have shown its own "failed" text (#20): say it still runs
        coop_notify(aim1_ui("ql_fail_host"));
        return;
    }
    o_set_flag(sm, name);
}

// Dropping the guest's ".FAIL" flag left the rest of the failing script block running there - its
// "failed" text, journal entry, penalties - so the task still looked failed to the guest (#12, after
// the release with the flag fix). The script interpreter (CScriptInter) runs one token per
// RunBlock call (0x425f40: "IF(cond)", "{", "}", "ELSE", or a command such as
// "_SETEVENT(T_L1_B1_1.FAIL)"); a script is a token list (count +0x10, offsets +0x14, text +4), the
// position is interpreter +0x2fc. On a guest, an IF / ELSE block that would set a task's .FAIL
// (directly, not in a nested block) is skipped whole when it would run - nested blocks that complete
// a task are separate and still run.
#define A_RUN_BLOCK    0x00425f40   // CScriptInter::RunBlock(script*)
#define A_CHECK_COND   0x00425b10   // CScriptInter::CheckCondition(const char *token)
#define INTER_POS      0x2fc
#define INTER_DEPTH    0x304        // "{" nesting (0 at a procedure's start)

typedef int (THISCALL *runblock_fn)(uint8_t *, uint8_t **);
typedef char (THISCALL *cond_fn)(uint8_t *, const char *);
static runblock_fn o_run_block;

static const char *script_tok(const uint8_t *l, int i)
{
    if (!l || i < 0 || i >= *(const int *)(l + 0x10)) return NULL;
    return (const char *)(*(const uintptr_t *)(l + 4) + (*(const int **)(l + 0x14))[i]);
}

// the block opened by "{" at index i: returns the index after its "}" (-1 if unbalanced); *fail = a
// command of its own sets a task's .FAIL we don't have yet (its name in failname)
static int block_end(const uint8_t *l, int i, int *fail, char *failname, int n)
{
    *fail = 0;
    const char *t = script_tok(l, i);
    if (!t || strcmp(t, "{")) return -1;
    int depth = 0;
    for (; (t = script_tok(l, i)) != NULL; i++) {
        if (!strcmp(t, "{")) depth++;
        else if (!strcmp(t, "}")) { if (--depth == 0) return i + 1; }
        else if (depth == 1 && !*fail && strstr(t, "SETEVENT(") && strstr(t, ".FAIL")) {
            const char *a = strchr(t, '(') + 1, *b = strchr(a, ')');
            char name[128];
            int k = b ? (int)(b - a) : (int)strlen(a);
            if (k >= (int)sizeof name) k = sizeof name - 1;
            memcpy(name, a, k);
            name[k] = 0;
            while (k > 0 && name[k - 1] == ' ') name[--k] = 0;
            if (!has_flag(name)) {
                *fail = 1;
                if (failname) lstrcpynA(failname, name, n);
            }
        }
    }
    return -1;
}

// The block opened by "{" at index i offers a task ("_QUEST(T_L1_B1_2)" of its own): its name in q.
// Returns the index after its "}", -1 if it is no offer.
static int offer_block(const uint8_t *l, int i, char *q, int n)
{
    const char *t = script_tok(l, i);
    if (!t || strcmp(t, "{")) return -1;
    int depth = 0, found = 0;
    for (; (t = script_tok(l, i)) != NULL; i++) {
        if (!strcmp(t, "{")) depth++;
        else if (!strcmp(t, "}")) { if (--depth == 0) return found ? i + 1 : -1; }
        else if (depth == 1 && !found && !strncmp(t, "_QUEST(", 7)) {
            const char *a = t + 7, *b = strchr(a, ')');
            int k = b ? (int)(b - a) : (int)strlen(a);
            if (k >= n) k = n - 1;
            memcpy(q, a, k);
            q[k] = 0;
            while (k > 0 && q[k - 1] == ' ') q[--k] = 0;
            found = q[0] != 0;
        }
    }
    return -1;
}

// another task of the same giver (same name up to the last '_': T_L1_B1_1 for T_L1_B1_2) is still
// running in our log: its name in busy
static int series_busy(const char *q, char *busy, int n)
{
    const char *u = strrchr(q, '_');
    if (!u) return 0;
    int plen = (int)(u - q) + 1;
    uint8_t *sm = G_SCRIPTS;
    int nq = *(int *)(sm + SM_QUEST_N);
    const char *list = (const char *)*(uint8_t **)(sm + SM_QUEST_P);
    if (nq <= 0 || nq > 2000 || !list) return 0;
    for (int i = 0; i < nq; i++) {
        char name[QUEST_SIZE], f[QUEST_SIZE + 16];
        lstrcpynA(name, list + i * QUEST_SIZE, QUEST_SIZE - 4);
        if (strncmp(name, q, plen) || !strcmp(name, q)) continue;
        snprintf(f, sizeof f, "%s.COMPLETE", name);
        if (has_flag(f)) continue;
        snprintf(f, sizeof f, "%s.FAIL", name);
        if (has_flag(f)) continue;
        lstrcpynA(busy, name, n);
        return 1;
    }
    return 0;
}

static int THISCALL h_run_block(uint8_t *in, uint8_t **script)
{
    if (net_mode() == NET_CLIENT && !g_from_host && script && *script && G_SCRIPTS) {
        const uint8_t *l = *script;
        int pos = *(int *)(in + INTER_POS);
        const char *t = script_tok(l, pos);
        if (t && !strcmp(t, "{") && *(int *)(in + INTER_DEPTH) == 0) {
            // a procedure's body (a task timer's timeout runs one): failing the task by itself
            char name[128] = "";
            int fail, end = block_end(l, pos, &fail, name, sizeof name);
            if (end > 0 && fail) {
                *(int *)(in + INTER_POS) = end;
                static int nlog;
                if (nlog++ < 10) mlog("questlog: skipped a script procedure failing '%s' here - left to the host", name);
                coop_notify(aim1_ui("ql_fail_host"));
                return 1;
            }
        }
        if (t && !strncmp(t, "IF", 2)) {
            // A base offers its tasks in order: IF(!T_L1_B1_1.GET) {_QUEST(T_L1_B1_1) END} IF(!T_L1_B1_2.GET)
            // {...}. The host took task 1 and the guest got it through the shared log, but not the base's
            // own state, so the guest's base went on to offer task 2 while task 1 still runs. On a guest,
            // no offer while another task of the same giver runs.
            char q[64] = "", busy[QUEST_SIZE] = "";
            int oend = offer_block(l, pos + 1, q, sizeof q);
            if (oend > 0 && series_busy(q, busy, sizeof busy) && CALL(A_CHECK_COND, cond_fn)(in, t)) {
                const char *e = script_tok(l, oend);
                if (e && !strcmp(e, "ELSE")) {
                    int dummy, ee = block_end(l, oend + 1, &dummy, NULL, 0);
                    if (ee > 0) oend = ee;
                }
                *(int *)(in + INTER_POS) = oend;
                static int nlog;
                if (nlog++ < 10) mlog("questlog: not offering '%s' here - '%s' still runs", q, busy);
                return 1;
            }
            char name[128] = "";
            int if_fail, else_fail = 0;
            int end = block_end(l, pos + 1, &if_fail, name, sizeof name), after = end;
            const char *e = end > 0 ? script_tok(l, end) : NULL;
            if (e && !strcmp(e, "ELSE")) {
                char en[128] = "";
                int ee = block_end(l, end + 1, &else_fail, en, sizeof en);
                if (ee > 0) after = ee;
                else else_fail = 0;
                if (else_fail && !name[0]) lstrcpynA(name, en, sizeof name);
            }
            if (end > 0 && (if_fail || else_fail)) {
                char cond = CALL(A_CHECK_COND, cond_fn)(in, t);
                if ((cond && if_fail) || (!cond && else_fail)) {
                    *(int *)(in + INTER_POS) = after;          // the whole IF (and ELSE) is done
                    static int nlog;
                    if (nlog++ < 10) mlog("questlog: skipped a script block failing '%s' here - left to the host", name);
                    coop_notify(aim1_ui("ql_fail_host"));
                    return 1;
                }
            }
        }
    }
    return o_run_block(in, script);
}

void questlog_install(void)
{
    static const uint8_t prologue[6] = {0x55, 0x56, 0x8b, 0x74, 0x24, 0x0c};   // push ebp; push esi; mov esi, [esp+0xc]
    o_set_flag = (str_fn)hook_jmp(A_SET_FLAG, prologue, sizeof prologue, (void *)set_flag);
    static const uint8_t pro[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};             // push ebp; mov ebp, esp; push -1
    o_run_block = (runblock_fn)hook_jmp(A_RUN_BLOCK, pro, 5, (void *)h_run_block);
    mlog("questlog: guest task failures %s", o_set_flag && o_run_block ? "left to the host (flag and script block)" :
                                             "NOT handled (unexpected code)");
}

// harness: "qldump", "qladd <quest>"
void questlog_test(const char *cmd, const char *arg)
{
    uint8_t *sm = G_SCRIPTS;
    if (!sm) return;
    if (!strcmp(cmd, "qldump")) {
        int nq = *(int *)(sm + SM_QUEST_N), nj = *(int *)(sm + SM_JOURNAL_N);
        int len;
        uint8_t *b = log_save(&len);
        int nfl = 0;
        const char *fl;
        for (int i = 0; (fl = flag_name(i, &nfl)) != NULL; i++)
            if (!strcmp(arg, "flags")) mlog("  flag %s", fl);
        mlog("questlog: %d quests, %d journal entries, %d flags (hash %08x)", nq, nj, nfl, b ? hash(b, len) : 0);
        free(b);
        for (int i = 0; i < nq; i++) {
            const char *q = (const char *)*(uint8_t **)(sm + SM_QUEST_P) + i * QUEST_SIZE;
            char c[0x60], f[0x60];
            snprintf(c, sizeof c, "%s.COMPLETE", q);
            snprintf(f, sizeof f, "%s.FAIL", q);
            mlog("  quest %s%s%s", q, has_flag(c) ? " (complete)" : "", has_flag(f) ? " (failed)" : "");
        }
        for (int i = 0; i < nj; i++) mlog("  journal %.32s", (const char *)*(uint8_t **)(sm + SM_JOURNAL_P) + i * JOURNAL_SIZE);
        const uint8_t *t;
        for (int i = 0; (t = timer_entry(i)) != NULL; i++) {
            const uint8_t *q = quest_entry((const char *)t);
            mlog("  timer %.32s: %.1f game min left%s, script %.32s", (const char *)t,
                 (*(const float *)(t + TIMER_DEADLINE) - WORLD_DAYS) * 1440.0f,
                 q && *(const float *)(q + QUEST_DEADLINE) > WORLD_DAYS ? " (on the HUD)" : "", (const char *)t + 0x28);
        }
    } else if (!strcmp(cmd, "qltimer") && arg[0]) {    // "<quest> <hours>", as a script's STARTTIMER
        char name[64], hours[32] = "1";
        if (sscanf(arg, "%63s %31s", name, hours) >= 1) CALL(A_START_TIMER, start_timer_fn)(sm, name, hours);
    } else if (!strcmp(cmd, "qladd") && arg[0]) {
        CALL(0x0043adf0, str_fn)(sm, arg);
    } else if (!strcmp(cmd, "qlflag") && arg[0]) {     // e.g. T_L1_B1_1.COMPLETE
        CALL(0x0043ac60, str_fn)(sm, arg);
    } else if (!strcmp(cmd, "qlunflag") && arg[0]) {   // as a script's REMOVEEVENT
        CALL(A_REMOVE_FLAG, str_fn)(sm, arg);
    } else if (!strcmp(cmd, "qljournal") && arg[0]) {
        CALL(0x00437ea0, str_fn)(sm, arg);
    }
}
