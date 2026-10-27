// Shared quest log (co-op phase 3).
//
// CQuestLog (*0x613858) holds the missions and journal shown in the LOG
// panel and serializes itself (vtable 16 SaveToStream, 17 LoadFromStream):
//   u32 0x101 | u32 n1, n1 * 0x24 | u32 n2, n2 * 0x68 (missions) | u32 n3, n3 * 0x40 (journal)
// The host's log is sent whenever it changes. A joining player whose log
// changed locally (a mission taken in a base) sends it up; the host merges
// both (union; a mission completed or failed on either side stays so) and
// the merged log goes back to everybody.
#include "engine.h"
#include "coop.h"
#include "../common/net.h"
#include <string.h>
#include <stdlib.h>

#define CALL(addr, T) ((T)(addr))
#define VF_QL_SAVE 0x40
#define VF_QL_LOAD 0x44
#define QL_VERSION 0x101
#define MISSION_FAILED 0x40
#define MISSION_DONE   0x41

enum { MSG_LOG = 40, MSG_LOG_UP = 41 };

typedef void (THISCALL *stream_alloc_fn)(gstream *, int);
typedef void (THISCALL *stream_io_fn)(void *, gstream *);

static void (__cdecl *crt_free)(void *);
static uint32_t g_known;          // hash of the log as last agreed with the host
static uint8_t *g_pending;        // received log to apply at the next frame
static int g_pending_len, g_pending_from, g_pending_type;
static DWORD g_last_tick;

static uint32_t hash(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static uint8_t *log_save(int *len)
{
    if (!G_QUESTLOG) return NULL;
    gstream s = {0};
    CALL(A_STREAM_ALLOC, stream_alloc_fn)(&s, 4096);
    VFN(G_QUESTLOG, VF_QL_SAVE, stream_io_fn)(G_QUESTLOG, &s);
    uint8_t *out = malloc(s.pos);
    memcpy(out, s.buf, s.pos);
    *len = s.pos;
    if (!s.owned && s.buf && crt_free) crt_free(s.buf);
    return out;
}

static void log_load(const uint8_t *blob, int len)
{
    if (!G_QUESTLOG || len < 16 || *(const uint32_t *)blob != QL_VERSION) return;
    gstream s = {1, len, 0, (uint8_t *)blob};
    VFN(G_QUESTLOG, VF_QL_LOAD, stream_io_fn)(G_QUESTLOG, &s);
}

// ---- merge -------------------------------------------------------------------------------
typedef struct { int n; const uint8_t *p; } arr;
static const int ESIZE[3] = {0x24, 0x68, 0x40};

static int parse(const uint8_t *b, int len, arr a[3])
{
    if (len < 4 || *(const uint32_t *)b != QL_VERSION) return 0;
    int pos = 4;
    for (int i = 0; i < 3; i++) {
        if (pos + 4 > len) return 0;
        a[i].n = *(const int *)(b + pos);
        pos += 4;
        if (a[i].n < 0 || pos + a[i].n * ESIZE[i] > len) return 0;
        a[i].p = b + pos;
        pos += a[i].n * ESIZE[i];
    }
    return 1;
}

// key: the leading name (32 bytes) for the first two arrays, the whole entry for the journal
static int same(int k, const uint8_t *x, const uint8_t *y)
{
    return k == 2 ? !memcmp(x, y, ESIZE[2]) : !strncmp((const char *)x, (const char *)y, 32);
}

static uint8_t *merge(const uint8_t *hb, int hl, const uint8_t *gb, int gl, int *outlen)
{
    arr h[3], g[3];
    if (!parse(hb, hl, h)) return NULL;
    if (!parse(gb, gl, g)) return NULL;
    uint8_t *out = malloc(hl + gl);
    int pos = 0;
    *(uint32_t *)out = QL_VERSION;
    pos = 4;
    for (int k = 0; k < 3; k++) {
        int cpos = pos;
        pos += 4;
        int n = 0;
        for (int i = 0; i < h[k].n; i++) {
            uint8_t *e = out + pos;
            memcpy(e, h[k].p + i * ESIZE[k], ESIZE[k]);
            if (k == 1) {                         // keep a mission's final state from either side
                for (int j = 0; j < g[k].n; j++) {
                    const uint8_t *ge = g[k].p + j * ESIZE[k];
                    if (same(k, e, ge)) {
                        if (ge[MISSION_DONE]) e[MISSION_DONE] = 1;
                        if (ge[MISSION_FAILED] && !e[MISSION_DONE]) e[MISSION_FAILED] = 1;
                    }
                }
            }
            pos += ESIZE[k];
            n++;
        }
        for (int j = 0; j < g[k].n; j++) {
            const uint8_t *ge = g[k].p + j * ESIZE[k];
            int found = 0;
            for (int i = 0; i < h[k].n && !found; i++) found = same(k, h[k].p + i * ESIZE[k], ge);
            if (found) continue;
            memcpy(out + pos, ge, ESIZE[k]);
            pos += ESIZE[k];
            n++;
        }
        *(int *)(out + cpos) = n;
    }
    *outlen = pos;
    return out;
}

// ---- sync ----------------------------------------------------------------------------------
static void apply_pending(void)
{
    if (!g_pending) return;
    if (g_pending_type == MSG_LOG) {
        log_load(g_pending, g_pending_len);
        int len;
        uint8_t *b = log_save(&len);
        if (b) { g_known = hash(b, len); free(b); }
    } else {                                      // MSG_LOG_UP on the host
        int hl, ml;
        uint8_t *hb = log_save(&hl);
        uint8_t *m = hb ? merge(hb, hl, g_pending, g_pending_len, &ml) : NULL;
        if (m && (ml != hl || memcmp(m, hb, hl))) {
            log_load(m, ml);
            mlog("questlog: merged the log of player %d", g_pending_from);
        }
        free(m);
        free(hb);
    }
    free(g_pending);
    g_pending = NULL;
}

void questlog_frame(void)
{
    if (net_mode() == NET_OFF || !coop_in_flight()) return;
    apply_pending();
    DWORD now = GetTickCount();
    if (now - g_last_tick < 1000) return;
    g_last_tick = now;
    int len;
    uint8_t *b = log_save(&len);
    if (!b) return;
    uint32_t h = hash(b, len);
    if (h != g_known) {
        g_known = h;
        if (net_mode() == NET_HOST) {
            for (int i = 1; i < NET_MAX_PEERS; i++)
                if (net_peer_connected(i) && coop_peer_same_clan(i)) net_send(i, MSG_LOG, b, len, 1);
        } else if (coop_my_clan() == 0) {
            net_send(0, MSG_LOG_UP, b, len, 1);
        }
    }
    free(b);
}

// a player joined: they get the full log with the world, plus this
void questlog_peer_joined(int peer)
{
    if (!coop_peer_same_clan(peer)) return;
    int len;
    uint8_t *b = log_save(&len);
    if (b) { net_send(peer, MSG_LOG, b, len, 1); free(b); }
}

int questlog_handle(int type, int from, const uint8_t *data, int len)
{
    if (type != MSG_LOG && type != MSG_LOG_UP) return 0;
    if ((type == MSG_LOG && net_mode() != NET_CLIENT) || (type == MSG_LOG_UP && net_mode() != NET_HOST)) return 1;
    if (type == MSG_LOG_UP && !coop_peer_same_clan(from)) return 1;
    if (type == MSG_LOG && coop_my_clan()) return 1;
    free(g_pending);
    g_pending = malloc(len);
    memcpy(g_pending, data, len);
    g_pending_len = len;
    g_pending_from = from;
    g_pending_type = type;
    return 1;
}

// test helpers (harness commands "qldump", "qladd <key>")
void questlog_test(const char *cmd, const char *arg)
{
    int len;
    uint8_t *b = log_save(&len);
    arr a[3];
    if (!b || !parse(b, len, a)) { free(b); return; }
    if (!strcmp(cmd, "qldump")) {
        mlog("questlog: %d info, %d missions, %d journal (hash %08x)", a[0].n, a[1].n, a[2].n, hash(b, len));
        for (int i = 0; i < a[1].n; i++) mlog("  mission %.32s done=%d failed=%d", a[1].p + i * 0x68, a[1].p[i * 0x68 + MISSION_DONE], a[1].p[i * 0x68 + MISSION_FAILED]);
        for (int i = 0; i < a[2].n; i++) mlog("  journal %.64s", a[2].p + i * 0x40);
    } else if (!strcmp(cmd, "qladd") && arg[0]) {
        uint8_t *nb = malloc(len + 0x40);
        int off = (int)(a[2].p - b) + a[2].n * 0x40;    // end of the journal array (= end of blob)
        memcpy(nb, b, off);
        memset(nb + off, 0, 0x40);
        lstrcpynA((char *)nb + off, arg, 0x40);
        *(int *)(nb + (a[2].p - b) - 4) = a[2].n + 1;
        log_load(nb, off + 0x40);
        free(nb);
    }
    free(b);
}

uint8_t *questlog_blob(int *len) { return log_save(len); }

void questlog_apply(const uint8_t *blob, int len)
{
    log_load(blob, len);
    int l;
    uint8_t *b = log_save(&l);
    if (b) { g_known = hash(b, l); free(b); }
}

void questlog_reset(void)
{
    free(g_pending);
    g_pending = NULL;
    g_known = 0;
}

void questlog_install(void)
{
    HMODULE crt = GetModuleHandleA(CRT_DLL);
    crt_free = crt ? (void *)GetProcAddress(crt, "free") : NULL;
}
