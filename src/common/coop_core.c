// Co-op session core (see coop_core.h): players exchange their pilot
// (mechanoid) data and glider state; every remote player is shown as a puppet
// glider. A joining player receives the host's world (a save plus the
// per-location state files) and keeps their own pilot.
#include "coop_core.h"
#include "version.h"          // build/version.h, written by src/build.ps1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

extern char g_ini[MAX_PATH];

typedef struct {
    uint32_t seq;
    float m[16];
    float vel[3];
} state_msg;

typedef struct {
    int used;
    char name[32];
    uint8_t *blob;            // serialized pilot
    int blob_len, blob_dirty;
    snapshot snap[4];         // newest first
    int nsnap;
    uint32_t last_seq;
    int clan;                 // 0 = the host's clan, 1 = own clan
    uint8_t *glider;          // puppet
    int world_gen;            // world the puppet belongs to
    int down;                 // destroyed: no puppet until new pilot data (respawn / rejoin)
} remote;

static remote g_rem[NET_MAX_PEERS];
static int g_world_gen;       // bumped whenever the player's glider is (re)created
static char g_name[32] = "Player";
static int g_name_set;                       // [Coop] Name given (typed in the lobby): never replaced
static uint32_t g_seq;
static DWORD g_last_state;
static int g_mech_gen;                       // world generation our pilot data was last sent for
static int g_world_pending[NET_MAX_PEERS];   // host: players waiting for the world
static int g_loading_host_world;
static int g_host_intent;
static DWORD g_session_t0;
static DWORD g_profile_saved;
static uint32_t g_profile_hash;                  // of the profile last written (#42)
static int g_profile_test;
static int g_my_clan;
static uint8_t *g_profile_extra;             // restored once the host's world is up
static int g_profile_extra_len;
static uint32_t g_player_color = 0xffffff00;
int g_coop_menu;

// ---- commands from the test harness, run on the game thread -------------------------------
static CRITICAL_SECTION g_qcs;
static char g_queue[8][128];
static int g_qn;

static void queue_cmd(const char *line)
{
    EnterCriticalSection(&g_qcs);
    if (g_qn < 8) lstrcpynA(g_queue[g_qn++], line, 128);
    LeaveCriticalSection(&g_qcs);
}

// ---- players ---------------------------------------------------------------------------------
int coop_world_gen(void) { return g_world_gen; }
const char *coop_my_name(void) { return g_name; }
uint32_t coop_player_color(void) { return g_player_color; }
void coop_set_host_intent(int on) { g_host_intent = on; }
void coop_set_clan(int own) { g_my_clan = own ? 1 : 0; }
int coop_my_clan(void) { return net_mode() == NET_HOST ? 0 : g_my_clan; }

void coop_set_name(const char *name)
{
    if (!name || !name[0]) return;
    lstrcpynA(g_name, name, sizeof g_name);
    g_name_set = 1;
    WritePrivateProfileStringA("Coop", "Name", g_name, g_ini);
}

int coop_peer_same_clan(int peer)
{
    if (peer < 0 || peer >= NET_MAX_PEERS) return 0;
    int theirs = peer == 0 ? 0 : g_rem[peer].clan;   // the host is always in the host's clan
    return !coop_my_clan() && !theirs;                 // own clans: never shared
}

int coop_remote_peer(const uint8_t *g)
{
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g && g_rem[i].glider == g && g_rem[i].world_gen == g_world_gen) return i;
    return -1;
}

int coop_glider_same_clan(const uint8_t *g)
{
    int i = coop_remote_peer(g);
    return i >= 0 && coop_peer_same_clan(i);
}

const char *coop_remote_name(const uint8_t *g)
{
    int i = coop_remote_peer(g);
    return i >= 0 ? g_rem[i].name : NULL;
}

uint8_t *coop_remote_glider(const char *name)
{
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_rem[i].used && g_rem[i].glider && g_rem[i].world_gen == g_world_gen && !strcmp(g_rem[i].name, name))
            return g_rem[i].glider;
    return NULL;
}

uint8_t *coop_remote_by_index(int n, const char **name)
{
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_rem[i].used && g_rem[i].glider && g_rem[i].world_gen == g_world_gen && n-- == 0) {
            if (name) *name = g_rem[i].name;
            return g_rem[i].glider;
        }
    return NULL;
}

int coop_is_remote(const uint8_t *g)
{
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g && g_rem[i].glider == g) return 1;
    return 0;
}

void coop_say_to(const uint8_t *glider, const char *text)
{
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (glider && g_rem[i].glider == glider) { net_send(i, MSG_SAY, text, (int)strlen(text) + 1, 1); return; }
}

static void despawn(remote *r)
{
    if (r->glider && r->world_gen == g_world_gen && !g_coop_menu) cg_remove_glider(r->glider);
    r->glider = NULL;
}

// A remote player was destroyed. game_owns: the game is exploding the puppet and frees it like any
// dead glider - only drop our pointer; else remove it ourselves.
void coop_remote_down(int peer, int game_owns)
{
    if (peer < 0 || peer >= NET_MAX_PEERS) return;
    remote *r = &g_rem[peer];
    if (game_owns) r->glider = NULL;
    else despawn(r);
    r->down = 1;
}

// a connected player of that name who is not destroyed (case-insensitive: the game shows names in capitals)
int coop_player_up(const char *name)
{
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_rem[i].used && g_rem[i].name[0] && i != net_my_id() && !g_rem[i].down && !lstrcmpiA(g_rem[i].name, name))
            return 1;
    return 0;
}

// the other players still up (flying, or in a base / menu), for a dead player's spectator mode
int coop_teammates_up(void)
{
    int n = 0;
    for (int i = 0; i < NET_MAX_PEERS; i++)
        if (g_rem[i].used && g_rem[i].name[0] && i != net_my_id() && !g_rem[i].down) n++;
    return n;
}

static void forget(int id)
{
    remote *r = &g_rem[id];
    despawn(r);
    free(r->blob);
    memset(r, 0, sizeof *r);
}

static void normalize_rows(float *m)
{
    for (int r = 0; r < 3; r++) {
        float *v = m + r * 4;
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    }
}

// state shown now: 100 ms behind the newest snapshot, interpolated
int coop_sample(const snapshot *snap, int nsnap, float out[16], float vel[3])
{
    if (!nsnap) return 0;
    DWORD t = GetTickCount() - 100;
    const snapshot *a = &snap[0], *b = NULL;
    for (int i = 0; i < nsnap - 1 && (int)(t - snap[0].t) < 0; i++) {
        if ((int)(t - snap[i + 1].t) >= 0) { a = &snap[i + 1]; b = &snap[i]; break; }
    }
    if (!b) {                                   // newest is older than t: extrapolate a little
        float dt = (int)(t - a->t) / 1000.0f;
        if (dt > 0.25f) dt = 0.25f;
        if (dt < 0) dt = 0;
        memcpy(out, a->m, 16 * sizeof(float));
        for (int k = 0; k < 3; k++) out[12 + k] += a->vel[k] * dt;
        memcpy(vel, a->vel, 3 * sizeof(float));
        return 1;
    }
    float f = (b->t == a->t) ? 1.0f : (float)(int)(t - a->t) / (float)(int)(b->t - a->t);
    for (int k = 0; k < 16; k++) out[k] = a->m[k] + (b->m[k] - a->m[k]) * f;
    for (int k = 0; k < 3; k++) vel[k] = a->vel[k] + (b->vel[k] - a->vel[k]) * f;
    normalize_rows(out);
    return 1;
}

static void drive_remotes(void)
{
    for (int id = 0; id < NET_MAX_PEERS; id++) {
        remote *r = &g_rem[id];
        if (!r->used || id == net_my_id()) continue;
        if (r->glider && r->world_gen != g_world_gen) r->glider = NULL;   // world was rebuilt
        if (r->down) continue;                                             // destroyed, waiting
        if (r->blob_dirty && r->glider) despawn(r);                        // new glider/equipment
        float m[16], vel[3];
        if (r->nsnap && GetTickCount() - r->snap[0].t > 2000) {            // in a base or a menu
            if (r->glider) despawn(r);
            continue;
        }
        if (!coop_sample(r->snap, r->nsnap, m, vel)) continue;
        if (!r->glider && r->blob) {
            r->glider = cg_spawn_player(r->blob, r->blob_len, m, r->name, coop_peer_same_clan(id));
            r->world_gen = g_world_gen;
            r->blob_dirty = 0;
        }
        if (r->glider) cg_drive(r->glider, m, vel);
    }
}

static void update_remotes(void)
{
    if (cg_in_flight()) drive_remotes();
}

// The game runs its world without the flight loop (A.I.M.: the host in a base, #37): the other
// players' gliders keep flying in it.
void coop_world_tick(void)
{
    if (net_mode() != NET_OFF) drive_remotes();
}

// ---- messages ----------------------------------------------------------------------------------
static void send_hello(int to)
{
    char n[33] = {0};
    lstrcpynA(n, g_name, 32);
    n[32] = (char)coop_my_clan();
    net_send(to, MSG_HELLO, n, 33, 1);
}

static void send_mech(int to)
{
    if (!cg_in_flight()) return;
    int len;
    uint8_t *b = cg_pilot_save(&len);
    net_send(to, MSG_MECH, b, len, 1);
    free(b);
}

// The last state sent while flying. When the game stops running (alt-tab out of fullscreen, a
// hang) the heartbeat thread repeats it, standing still, so the others keep seeing the glider;
// a player who enters a base or a menu stops flying and disappears for them as before.
static state_msg g_hb;
static volatile LONG g_hb_flying;
static volatile DWORD g_hb_frame;

static void send_state(void)
{
    DWORD now = GetTickCount();
    g_hb_frame = now;
    if (!cg_in_flight()) { g_hb_flying = 0; return; }
    if (now - g_last_state < 50) return;
    g_last_state = now;
    state_msg s;
    s.seq = (uint32_t)InterlockedIncrement((volatile LONG *)&g_seq);
    cg_player_state(s.m, s.vel);
    net_send(NET_ALL, MSG_STATE, &s, sizeof s, 0);
    g_hb = s;
    memset(g_hb.vel, 0, sizeof g_hb.vel);
    g_hb_flying = 1;
}

static DWORD WINAPI heartbeat_thread(void *p)
{
    (void)p;
    for (;;) {
        Sleep(250);
        if (net_mode() == NET_OFF || !g_hb_flying || GetTickCount() - g_hb_frame < 400) continue;
        HWND display_window(void);
        HWND w = display_window();
        if (w && GetForegroundWindow() == w && !IsIconic(w)) continue;      // stalled in front: a base, loading
        state_msg s = g_hb;
        s.seq = (uint32_t)InterlockedIncrement((volatile LONG *)&g_seq);
        net_send(NET_ALL, MSG_STATE, &s, sizeof s, 0);
    }
    return 0;
}

// "Co-op mod version ..." in a corner of the main menu (overlay.c); MOD_VERSION is the release tag
void overlay_set_corner(const char *text);

void coop_show_version(int on)
{
    static int shown = -1;
    if (on == shown) return;
    shown = on;
    overlay_set_corner(on ? "Co-op mod version " MOD_VERSION : NULL);
}

// In a panel opened in flight (hold, journal, map, ESC menu) the flight loop stops but the glider
// is still out there: keep it shown to the others, standing still. Called from the menu loops.
void coop_menu_keepalive(void)
{
    static DWORD last;
    if (net_mode() == NET_OFF || !g_hb_flying || GetTickCount() - last < 100) return;
    static int nlog;
    if (GetTickCount() - last > 5000 && nlog++ < 10) mlog("coop: a panel is open in flight - our glider stays shown");
    last = GetTickCount();
    state_msg s = g_hb;
    s.seq = (uint32_t)InterlockedIncrement((volatile LONG *)&g_seq);
    net_send(NET_ALL, MSG_STATE, &s, sizeof s, 0);
}

static void heartbeat_start(void)
{
    static LONG started;
    if (!InterlockedExchange(&started, 1)) CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
}

// ---- world transfer (host -> joining player) ---------------------------------------------
// The host saves a snapshot and sends it with the per-location state files
// (SAVES\LOCS); the joining player writes them to its SAVES folder and loads.
#define CHUNK 48000

typedef struct { char name[64]; uint32_t size, off; } file_hdr;

static void send_file(int peer, const char *rel)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sSAVES\\%s", g_root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sizeof(file_hdr) + CHUNK);
    file_hdr *h = (file_hdr *)buf;
    memset(h, 0, sizeof *h);
    lstrcpynA(h->name, rel, sizeof h->name);
    h->size = size;
    uint32_t off = 0;
    do {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK;
        h->off = off;
        fread(buf + sizeof *h, 1, n, f);
        net_send(peer, MSG_WORLD_FILE, buf, sizeof *h + n, 1);
        off += n;
    } while (off < size);
    free(buf);
    fclose(f);
}

static void world_send(int peer)
{
    cg_save_world(COOP_SYNC_SAVE);
    char rels[80][64];
    int n = 0;
    lstrcpyA(rels[n++], COOP_SYNC_SAVE);
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%sSAVES\\LOCS\\*", g_root);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 80)
                snprintf(rels[n++], 64, "LOCS\\%s", fd.cFileName);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        char path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA a;
        snprintf(path, sizeof path, "%sSAVES\\%s", g_root, rels[i]);
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) total += a.nFileSizeLow;
    }
    uint32_t begin[2] = {total, (uint32_t)n};
    net_send(peer, MSG_WORLD_BEGIN, begin, sizeof begin, 1);
    for (int i = 0; i < n; i++) send_file(peer, rels[i]);
    net_send(peer, MSG_WORLD_END, NULL, 0, 1);
    mlog("coop: sent the world to player %d (%d files, %u bytes)", peer, n, total);
}

static uint32_t g_rx_total, g_rx_got;
static int g_rx_active;
static FILE *g_rx_file;
static char g_rx_name[64];

int coop_world_receiving(int *percent)
{
    if (percent) *percent = g_rx_total ? (int)((uint64_t)g_rx_got * 100 / g_rx_total) : 0;
    return g_rx_active;
}

static int safe_rel(const char *s)
{
    return s[0] && !strstr(s, "..") && !strchr(s, ':') && s[0] != '\\' && s[0] != '/';
}

static void world_receive(const netmsg *m)
{
    if (m->type == MSG_WORLD_BEGIN && m->len >= 8) {
        g_rx_total = ((const uint32_t *)m->data)[0];
        g_rx_got = 0;
        g_rx_active = 1;
        g_rx_name[0] = 0;
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%sSAVES", g_root);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof dir, "%sSAVES\\LOCS", g_root);
        CreateDirectoryA(dir, NULL);
        mlog("coop: receiving the world (%u bytes)", g_rx_total);
    } else if (m->type == MSG_WORLD_FILE && m->len >= (int)sizeof(file_hdr)) {
        file_hdr h;
        memcpy(&h, m->data, sizeof h);
        h.name[63] = 0;
        if (!safe_rel(h.name)) return;
        if (strcmp(h.name, g_rx_name) || !g_rx_file) {
            if (g_rx_file) fclose(g_rx_file);
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%sSAVES\\%s", g_root, h.name);
            g_rx_file = fopen(path, "wb");
            lstrcpyA(g_rx_name, h.name);
        }
        if (g_rx_file) {
            fseek(g_rx_file, h.off, SEEK_SET);
            fwrite(m->data + sizeof h, 1, m->len - sizeof h, g_rx_file);
        }
        g_rx_got += m->len - sizeof h;
        int pct;
        coop_world_receiving(&pct);
        cg_world_progress(pct);
    } else if (m->type == MSG_WORLD_END) {
        if (g_rx_file) fclose(g_rx_file);
        g_rx_file = NULL;
        g_rx_name[0] = 0;
        g_rx_active = 0;
        mlog("coop: world received (%u bytes)", g_rx_got);
        g_loading_host_world = 1;
        if (cg_in_flight() && !g_profile_saved) coop_profile_save();    // joined from our own game
        cg_load_world(COOP_SYNC_SAVE);
    }
}

static void reset_all(void)
{
    for (int i = 0; i < NET_MAX_PEERS; i++) forget(i);
    cg_reset();
}

static void backup_store(int peer, const uint8_t *data, int len);
static void backup_send(int peer);
static void backup_received(const uint8_t *data, int len);

static void handle(const netmsg *m)
{
    int id = m->from;
    if (id >= NET_MAX_PEERS && m->type >= 16) return;
    if (cg_handle(m->type, id, m->data, m->len, id < NET_MAX_PEERS && g_rem[id].name[0] ? g_rem[id].name : "?")) return;
    remote *r = id < NET_MAX_PEERS ? &g_rem[id] : NULL;
    char nb[128];
    switch (m->type) {
    case NM_PEER_JOIN:
        if (m->len >= 1 && m->data[0] < NET_MAX_PEERS) {
            mlog("coop: player %d joined", m->data[0]);
            send_hello(m->data[0]);
            g_mech_gen = -1;                                 // send our pilot from the next frame
            if (net_mode() == NET_HOST) g_world_pending[m->data[0]] = 1;
        }
        break;
    case MSG_WORLD_BEGIN:
    case MSG_WORLD_FILE:
    case MSG_WORLD_END:
        world_receive(m);
        break;
    case MSG_WORLD_REQ:
        if (net_mode() == NET_HOST && id > 0) g_world_pending[id] = 1;
        break;
    case NM_PEER_LEAVE:
        if (m->len >= 1 && m->data[0] < NET_MAX_PEERS) {
            remote *l = &g_rem[m->data[0]];
            mlog("coop: player %d (%s) left", m->data[0], l->name);
            if (l->name[0]) {
                snprintf(nb, sizeof nb, cg_text("left"), l->name);
                coop_notify(nb);
            }
            forget(m->data[0]);
        }
        break;
    case NM_DISCONNECTED:
        mlog("coop: disconnected from the host");
        coop_notify(cg_text("lost"));
        reset_all();
        break;
    case MSG_HELLO: {
        int first = !r->name[0];
        r->used = 1;
        memcpy(r->name, m->data, m->len < 32 ? m->len : 31);
        r->name[31] = 0;
        r->clan = m->len >= 33 ? m->data[32] : 0;
        mlog("coop: player %d is '%s'", id, r->name);
        if (first && GetTickCount() - g_session_t0 > 3000) {
            snprintf(nb, sizeof nb, cg_text("joined"), r->name);
            coop_notify(nb);
        }
        if (first && net_mode() == NET_HOST && id > 0) backup_send(id);
        break;
    }
    case MSG_PROFILE_UP:
        if (net_mode() == NET_HOST) backup_store(id, m->data, m->len);
        break;
    case MSG_PROFILE_BACK:
        if (net_mode() == NET_CLIENT) backup_received(m->data, m->len);
        break;
    case MSG_MECH:
        r->used = 1;
        free(r->blob);
        r->blob = malloc(m->len);
        memcpy(r->blob, m->data, m->len);
        r->blob_len = m->len;
        r->blob_dirty = 1;
        r->down = 0;                                         // back in flight (respawned / rejoined)
        mlog("coop: pilot data from %d (%d bytes)", id, m->len);
        break;
    case MSG_SAY:
        if (m->len > 1 && m->len < 256 && m->data[m->len - 1] == 0) coop_notify((const char *)m->data);
        break;
    case MSG_STATE:
        if (m->len == sizeof(state_msg)) {
            const state_msg *s = (const state_msg *)m->data;
            if (r->nsnap && (int)(s->seq - r->last_seq) <= 0) break;    // late UDP packet
            r->last_seq = s->seq;
            memmove(&r->snap[1], &r->snap[0], 3 * sizeof(snapshot));
            r->snap[0].t = GetTickCount();
            memcpy(r->snap[0].m, s->m, sizeof s->m);
            memcpy(r->snap[0].vel, s->vel, sizeof s->vel);
            if (r->nsnap < 4) r->nsnap++;
        }
        break;
    }
}

static void session_started(void)
{
    g_session_t0 = GetTickCount();
    g_profile_hash = 0;                                      // the first save of a session always goes out
    heartbeat_start();
    cg_session_started();
    send_hello(NET_ALL);
    g_mech_gen = -1;                                         // our pilot goes out from the next frame
}

int coop_host(void)
{
    if (net_mode() == NET_HOST) return 1;
    if (!net_host(0, GetPrivateProfileIntA("Coop", "Loopback", 0, g_ini))) return 0;
    net_set_session_name(g_name, 1);
    session_started();
    mlog("coop: hosting");
    return 1;
}

int coop_join(const char *addr)
{
    if (!net_join(addr, 0)) return 0;
    session_started();
    return 1;
}

void coop_leave(void)
{
    if (net_mode() == NET_CLIENT) coop_profile_save();     // what changed since the last check
    net_stop();
    reset_all();
}

void coop_poll(void)
{
    netmsg m;
    while (net_poll(&m)) handle(&m);
}

void coop_menu_poll(void)
{
    if (net_mode() == NET_OFF) return;
    g_coop_menu = 1;
    coop_poll();
    g_coop_menu = 0;
}

// ---- personal pilot -----------------------------------------------------------------------
// A joining player loads the host's world, which contains the host's pilot.
// Each player keeps their own (glider, equipment, money) in a profile file
// and gets it back whenever the host's world loads.
// One profile per host and player name (SAVES\coop_<host>_<name>.bin), so a player keeps a
// separate pilot for each friend's campaign and several players can share a PC.
// File: "RRP1" [u32 len][pilot] [u32 len][extra]
static void file_part(char *out, int n, const char *s)
{
    int k = 0;
    for (; *s && k < n - 1; s++)
        out[k++] = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' ? *s : '_';
    out[k] = 0;
}

static void profile_path(char *out, int n)
{
    char host[32], me[32];
    file_part(host, sizeof host, g_rem[0].name[0] ? g_rem[0].name : "host");
    file_part(me, sizeof me, g_name);
    snprintf(out, n, "%sSAVES\\coop_%s_%s.bin", g_root, host, me);
}

void coop_profile_keep(void) { g_profile_test = 1; }   // test: keep the profile file as written

void coop_profile_path(char *out, int n) { profile_path(out, n); }

// Written whenever it changed (checked every 2 s in flight, on leaving the session, when a panel
// opens), so a quit or crash loses at most a few seconds (#42). Every write also goes to the host,
// which keeps a backup and hands it back if the player's own file is missing.
static uint32_t fnv(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

void coop_profile_save(void)
{
    if (!cg_in_flight() || !cg_pilot_saveable()) return;
    int len, xlen = 0;
    uint8_t *b = cg_pilot_save(&len);
    uint8_t *x = cg_profile_extra(&xlen);
    uint32_t ml = (uint32_t)len, xl = x ? (uint32_t)xlen : 0;
    int n = 12 + (int)ml + (int)xl;
    uint8_t *file = malloc(n);
    memcpy(file, "RRP1", 4);
    memcpy(file + 4, &ml, 4);
    memcpy(file + 8, b, ml);
    memcpy(file + 8 + ml, &xl, 4);
    if (xl) memcpy(file + 12 + ml, x, xl);
    free(b);
    free(x);
    g_profile_saved = GetTickCount();
    uint32_t h = fnv(file, n);
    if (h != g_profile_hash) {
        char p[MAX_PATH];
        profile_path(p, sizeof p);
        FILE *f = fopen(p, "wb");
        if (f) {
            fwrite(file, 1, n, f);
            fclose(f);
            g_profile_hash = h;
            static int nlog;
            if (nlog++ < 5) mlog("coop: profile saved (%d bytes)", n);
            if (net_mode() == NET_CLIENT) net_send(0, MSG_PROFILE_UP, file, n, 1);
        }
    }
    free(file);
}

static void backup_path(char *out, int n, const char *player)
{
    char me[32];
    file_part(me, sizeof me, player);
    snprintf(out, n, "%sSAVES\\coop_backup_%s.bin", g_root, me);
}

// host: a joining player's profile as last sent, kept here
static void backup_store(int peer, const uint8_t *data, int len)
{
    if (peer <= 0 || peer >= NET_MAX_PEERS || !g_rem[peer].name[0] || len < 12 || memcmp(data, "RRP1", 4)) return;
    char p[MAX_PATH];
    backup_path(p, sizeof p, g_rem[peer].name);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

// host: a player said hello - their backup goes back to them (used if their own file is missing)
static void backup_send(int peer)
{
    char p[MAX_PATH];
    backup_path(p, sizeof p, g_rem[peer].name);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    int n = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n >= 12 && n < 200000 ? malloc(n) : NULL;
    if (b && fread(b, 1, n, f) == (size_t)n) {
        net_send(peer, MSG_PROFILE_BACK, b, n, 1);
        mlog("coop: sent %s their profile backup (%d bytes)", g_rem[peer].name, n);
    }
    free(b);
    fclose(f);
}

// joining player: the host's backup of our profile - used when we have none for this host
static void backup_received(const uint8_t *data, int len)
{
    if (len < 12 || memcmp(data, "RRP1", 4)) return;
    char p[MAX_PATH];
    profile_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return; }                 // our own copy wins
    f = fopen(p, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
    mlog("coop: no profile here for this host - the host's backup restored (%d bytes)", len);
}

// the pilot part of the profile (malloc'ed); the extra part goes to g_profile_extra
static uint8_t *profile_load(int *len)
{
    char p[MAX_PATH];
    profile_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) {                                // the single profile of earlier versions
        snprintf(p, sizeof p, "%sSAVES\\coop_profile.bin", g_root);
        f = fopen(p, "rb");
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int n = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 8 ? malloc(n) : NULL;
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (!b) return NULL;
    int ml = *(int *)(b + 4);
    if (memcmp(b, "RRP1", 4) || ml <= 0 || ml > n - 8) { free(b); return NULL; }
    free(g_profile_extra);
    g_profile_extra = NULL;
    if (8 + ml + 4 <= n) {
        int xl = *(int *)(b + 8 + ml);
        if (xl > 0 && 12 + ml + xl <= n) {
            g_profile_extra = malloc(xl);
            memcpy(g_profile_extra, b + 12 + ml, xl);
            g_profile_extra_len = xl;
        }
    }
    *len = ml;
    memmove(b, b + 8, ml);
    return b;
}

uint8_t *coop_new_world(int *len)
{
    g_world_gen++;
    if (net_mode() != NET_OFF) g_last_state = 0;
    if (g_host_intent) {
        g_host_intent = 0;
        if (!coop_host()) coop_notify(cg_text("host_failed"));
    }
    if (net_mode() != NET_CLIENT || !g_loading_host_world) return NULL;
    uint8_t *b = profile_load(len);
    if (b) mlog("coop: own pilot restored (%d bytes)", *len);
    else mlog("coop: no profile for this host yet: flying the host's pilot copy");
    return b;
}

// ---- commands ---------------------------------------------------------------------------------
static void run_cmd(const char *line)
{
    char cmd[32] = {0}, arg[64] = {0};
    sscanf(line, "%31s %63s", cmd, arg);
    if (cg_command(cmd, arg, line)) return;
    if (!strcmp(cmd, "host")) {
        coop_host();
    } else if (!strcmp(cmd, "join")) {
        coop_join(arg[0] ? arg : "127.0.0.1");
    } else if (!strcmp(cmd, "joinown")) {
        coop_set_clan(1);
        coop_join(arg[0] ? arg : "127.0.0.1");
    } else if (!strcmp(cmd, "leave")) {
        coop_leave();
    } else if (!strcmp(cmd, "sendmech")) {
        send_mech(NET_ALL);
    } else if (!strcmp(cmd, "notice")) {
        coop_notify(arg[0] ? arg : "test notice");
    } else if (!strcmp(cmd, "pilotname")) {
        char pn[32] = {0};
        int ok = cg_in_flight() && cg_pilot_name(pn, sizeof pn);
        mlog("coop: pilot name %d '%s' (set %d)", ok, pn, g_name_set);
    } else if (!strcmp(cmd, "name") && arg[0]) {
        coop_set_name(arg);
        send_hello(NET_ALL);
    }
}

// Without a typed name, players are called like their pilot (the name given in the game's
// opening dialogue, or its identification code). Only the player's own game counts: a guest
// in the host's world may be flying a copy of the host's pilot.
static void follow_pilot_name(void)
{
    static DWORD last;
    if (g_name_set || net_mode() == NET_CLIENT || GetTickCount() - last < 1000 || !cg_in_flight()) return;
    last = GetTickCount();
    char pn[32] = {0};
    if (!cg_pilot_name(pn, sizeof pn) || !pn[0] || !strcmp(pn, g_name)) return;
    lstrcpynA(g_name, pn, sizeof g_name);
    WritePrivateProfileStringA("Coop", "PilotName", g_name, g_ini);
    mlog("coop: player name '%s' (the pilot's)", g_name);
    if (net_mode() == NET_HOST) {
        net_set_session_name(g_name, 1);
        send_hello(NET_ALL);
    }
}

// ---- chat -----------------------------------------------------------------------------------
// In flight in a co-op game the chat key (T, [Coop] ChatKey) opens a line at the bottom left;
// Enter sends it to everyone (a HUD notice "name: text"), Esc cancels.
void textinput_start_free(const char *initial, int max);
void overlay_set_line(const char *text);
static volatile int g_chat_req;
static int g_chat_typing;

void coop_chat_key(void) { if (!g_chat_typing) g_chat_req = 1; }    // (input thread)

static void chat_frame(void)
{
    if (g_chat_req) {
        g_chat_req = 0;
        if (net_mode() != NET_OFF && cg_in_flight() && !textinput_active()) {
            textinput_start_free("", 90);
            g_chat_typing = 1;
        }
    }
    if (!g_chat_typing) return;
    char t[128], line[160];
    int r = textinput_poll(t, sizeof t);
    if (r == 0 && cg_in_flight() && net_mode() != NET_OFF) {
        snprintf(line, sizeof line, "%s %s_", cg_text("chat_prompt"), t);
        overlay_set_line(line);
        return;
    }
    if (r == 0) textinput_stop();                    // left the flight while typing
    g_chat_typing = 0;
    overlay_set_line(NULL);
    if (r == 1 && t[0] && net_mode() != NET_OFF) {
        snprintf(line, sizeof line, "%s: %s", g_name, t);
        net_send(NET_ALL, MSG_SAY, line, (int)strlen(line) + 1, 1);
        coop_notify(line);
    }
}

// ---- per frame ------------------------------------------------------------------------------
void coop_frame(void)
{
    chat_frame();
    follow_pilot_name();
    coop_poll();
    static int was_fly;
    if (net_mode() != NET_OFF) {
        int fly = cg_in_flight();
        int back_in_flight = fly && !was_fly;
        was_fly = fly;
        if (back_in_flight && g_mech_gen == g_world_gen) {
            // out of a base (same world): its shop may have changed our glider or guns (#15)
            send_mech(NET_ALL);
        }
        if (fly && g_mech_gen != g_world_gen) {    // new world (or new session/player): tell the others our glider
            int was = g_mech_gen;
            g_mech_gen = g_world_gen;
            send_mech(NET_ALL);
            if (net_mode() == NET_CLIENT && !g_loading_host_world && was != -1) {
                net_send(0, MSG_WORLD_REQ, NULL, 0, 1);      // we changed location on our own
            } else if (net_mode() == NET_CLIENT && g_loading_host_world) {
                g_loading_host_world = 0;
                if (g_profile_extra) {
                    cg_profile_extra_restore(g_profile_extra, g_profile_extra_len);
                    free(g_profile_extra);
                    g_profile_extra = NULL;
                }
                // the host's snapshot puts us where the host is: step aside
                float m[16], v[3];
                cg_player_state(m, v);
                float off = 40.0f * net_my_id();
                cg_player_setpos(m[12] + m[0] * off, m[13] + m[1] * off, m[14] + 5.0f);
            }
        }
        if (net_mode() == NET_HOST && fly) {
            static int host_gen = -1;                        // the host changed location: everyone follows
            if (host_gen != g_world_gen) {
                if (host_gen != -1)
                    for (int i = 1; i < NET_MAX_PEERS; i++)
                        if (net_peer_connected(i)) g_world_pending[i] = 1;
                host_gen = g_world_gen;
            }
            for (int i = 1; i < NET_MAX_PEERS; i++) {
                if (!g_world_pending[i]) continue;
                // after their hello (and so their profile backup), or a few seconds without one
                if (!g_rem[i].name[0] && g_world_pending[i]++ < 300) continue;
                g_world_pending[i] = 0;
                world_send(i);
                cg_peer_joined(i);
            }
        }
        update_remotes();
        cg_frame();
        if (net_mode() == NET_CLIENT && fly && !g_loading_host_world && !g_profile_test && GetTickCount() - g_profile_saved > 2000)
            coop_profile_save();
        send_state();
    }
    if (g_qn) {
        char q[8][128];
        int n;
        EnterCriticalSection(&g_qcs);
        n = g_qn;
        memcpy(q, g_queue, sizeof q);
        g_qn = 0;
        LeaveCriticalSection(&g_qcs);
        for (int i = 0; i < n; i++) run_cmd(q[i]);
    }
}

void devtools_set_game_cmd(void (*cb)(const char *line));

void coop_core_install(void)
{
    InitializeCriticalSection(&g_qcs);
    net_preload();
    GetPrivateProfileStringA("Coop", "Name", "", g_name, sizeof g_name, g_ini);
    g_name_set = g_name[0] != 0;
    if (!g_name[0]) GetPrivateProfileStringA("Coop", "PilotName", "", g_name, sizeof g_name, g_ini);
    if (!g_name[0]) {
        DWORD n = sizeof g_name;
        if (!GetComputerNameA(g_name, &n)) lstrcpyA(g_name, "Player");
    }
    char b[32];
    GetPrivateProfileStringA("Coop", "PlayerColor", "ffffff00", b, sizeof b, g_ini);
    g_player_color = (uint32_t)strtoul(b, NULL, 16) | 0xff000000;
    devtools_set_game_cmd(queue_cmd);
    mlog("coop: player name '%s'", g_name);
}
