// LAN transport for co-op (see net.h).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HDR 5   // u16 len, u8 type, u8 from, u8 to
static const char UDP_MAGIC[4] = {'R', 'R', 'C', '1'};
static const char PROBE[4] = {'R', 'R', 'Q', '?'};
static const char ANSWER[4] = {'R', 'R', 'A', '!'};

typedef struct {
    SOCKET tcp;
    struct sockaddr_in udp;
    int have_udp;
    uint8_t *rbuf;
    int rlen;
    CRITICAL_SECTION wcs;
} peer;

typedef struct qmsg { struct qmsg *next; netmsg m; } qmsg;

static int g_mode, g_my_id;
static int g_port;
static SOCKET g_listen = INVALID_SOCKET, g_udp = INVALID_SOCKET;
static peer g_peers[NET_MAX_PEERS];
static struct sockaddr_in g_host_udp;
static HANDLE g_thread;
static volatile LONG g_quit;
static CRITICAL_SECTION g_qcs;
static qmsg *g_qhead, *g_qtail;
static uint8_t *g_last;          // data handed out by the previous net_poll
static char g_session[64] = "A.I.M. 2 co-op";
static int g_session_players = 1;
static DWORD g_session_id;       // tells one host apart from another (it answers on every network)
static int g_inited;

static void init_once(void)
{
    if (g_inited) return;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    InitializeCriticalSection(&g_qcs);
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        g_peers[i].tcp = INVALID_SOCKET;
        InitializeCriticalSection(&g_peers[i].wcs);
    }
    g_inited = 1;
}

// Winsock loads its provider DLLs on the first socket(); doing that later on
// the game thread can deadlock on the loader lock (seen in Clan Wars).
void net_preload(void)
{
    init_once();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) closesocket(s);
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) closesocket(s);
}

int net_mode(void) { return g_mode; }
int net_my_id(void) { return g_my_id; }
int net_peer_connected(int id)
{
    if (id == g_my_id) return g_mode != NET_OFF;
    if (g_mode == NET_HOST) return id > 0 && id < NET_MAX_PEERS && g_peers[id].tcp != INVALID_SOCKET;
    return g_mode == NET_CLIENT && (id == 0 || g_peers[id].have_udp);   // clients only know ids
}

void net_set_session_name(const char *name, int players)
{
    lstrcpynA(g_session, name, sizeof g_session);
    g_session_players = players;
}

// ---- receive queue (network thread -> game thread) -------------------------------------
static void enqueue(uint8_t type, uint8_t from, const uint8_t *data, int len)
{
    qmsg *q = malloc(sizeof *q + len);
    q->next = NULL;
    q->m.type = type;
    q->m.from = from;
    q->m.len = (uint16_t)len;
    q->m.data = (uint8_t *)(q + 1);
    if (len) memcpy(q->m.data, data, len);
    EnterCriticalSection(&g_qcs);
    if (g_qtail) g_qtail->next = q; else g_qhead = q;
    g_qtail = q;
    LeaveCriticalSection(&g_qcs);
}

int net_poll(netmsg *m)
{
    if (g_last) { free(g_last); g_last = NULL; }
    if (!g_inited) return 0;
    EnterCriticalSection(&g_qcs);
    qmsg *q = g_qhead;
    if (q) { g_qhead = q->next; if (!g_qhead) g_qtail = NULL; }
    LeaveCriticalSection(&g_qcs);
    if (!q) return 0;
    *m = q->m;
    g_last = (uint8_t *)q;
    return 1;
}

// ---- sending -------------------------------------------------------------------------------
static void build(uint8_t *buf, uint8_t type, uint8_t from, uint8_t to, const void *data, int len)
{
    buf[0] = (uint8_t)len;
    buf[1] = (uint8_t)(len >> 8);
    buf[2] = type;
    buf[3] = from;
    buf[4] = to;
    if (len) memcpy(buf + HDR, data, len);
}

static void tcp_send_raw(int pi, const uint8_t *buf, int n)
{
    peer *p = &g_peers[pi];
    EnterCriticalSection(&p->wcs);
    while (n > 0 && p->tcp != INVALID_SOCKET) {
        int r = send(p->tcp, (const char *)buf, n, 0);
        if (r <= 0) break;
        buf += r;
        n -= r;
    }
    LeaveCriticalSection(&p->wcs);
}

static void udp_send_raw(const struct sockaddr_in *to, const uint8_t *buf, int n)
{
    uint8_t pkt[NET_MAXMSG + 16];
    if (n + 4 > (int)sizeof pkt) return;
    memcpy(pkt, UDP_MAGIC, 4);
    memcpy(pkt + 4, buf, n);
    sendto(g_udp, (const char *)pkt, n + 4, 0, (const struct sockaddr *)to, sizeof *to);
}

// deliver one framed message to peer pi (host side) or to the host (client side)
static void send_to_peer(int pi, const uint8_t *buf, int n, int reliable)
{
    if (g_mode == NET_CLIENT) {
        if (reliable) tcp_send_raw(0, buf, n);
        else udp_send_raw(&g_host_udp, buf, n);
        return;
    }
    if (pi <= 0 || pi >= NET_MAX_PEERS || g_peers[pi].tcp == INVALID_SOCKET) return;
    if (reliable || !g_peers[pi].have_udp) tcp_send_raw(pi, buf, n);
    else udp_send_raw(&g_peers[pi].udp, buf, n);
}

void net_send(int to, uint8_t type, const void *data, int len, int reliable)
{
    if (g_mode == NET_OFF || len > NET_MAXMSG) return;
    uint8_t *buf = malloc(HDR + len);
    build(buf, type, (uint8_t)g_my_id, (uint8_t)to, data, len);
    if (g_mode == NET_CLIENT) {
        send_to_peer(0, buf, HDR + len, reliable);
    } else if (to == NET_ALL) {
        for (int i = 1; i < NET_MAX_PEERS; i++) send_to_peer(i, buf, HDR + len, reliable);
    } else {
        send_to_peer(to, buf, HDR + len, reliable);
    }
    free(buf);
}

// ---- incoming ------------------------------------------------------------------------------
// A complete message arrived from peer pi (host) / from the host (client).
static void on_message(int pi, const uint8_t *buf, int n, int reliable)
{
    int len = buf[0] | (buf[1] << 8);
    uint8_t type = buf[2], from = buf[3], to = buf[4];
    if (len + HDR != n) return;
    if (g_mode == NET_HOST) {
        from = (uint8_t)pi;                       // never trust the sender's claim
        uint8_t *fwd = (uint8_t *)buf;
        fwd[3] = from;
        if (to == NET_ALL) {
            for (int i = 1; i < NET_MAX_PEERS; i++)
                if (i != pi) send_to_peer(i, fwd, n, reliable);
        } else if (to != 0) {
            send_to_peer(to, fwd, n, reliable);
            return;
        }
    }
    enqueue(type, from, buf + HDR, len);
}

static void drop_peer(int pi)
{
    peer *p = &g_peers[pi];
    EnterCriticalSection(&p->wcs);
    if (p->tcp != INVALID_SOCKET) closesocket(p->tcp);
    p->tcp = INVALID_SOCKET;
    LeaveCriticalSection(&p->wcs);
    p->have_udp = 0;
    free(p->rbuf);
    p->rbuf = NULL;
    p->rlen = 0;
    if (g_mode == NET_HOST) {
        uint8_t id = (uint8_t)pi;
        mlog("net: peer %d left", pi);
        net_send(NET_ALL, NM_PEER_LEAVE, &id, 1, 1);
        enqueue(NM_PEER_LEAVE, 0, &id, 1);
    } else {
        mlog("net: lost the host");
        enqueue(NM_DISCONNECTED, 0, NULL, 0);
        g_mode = NET_OFF;
    }
}

static void read_tcp(int pi)
{
    peer *p = &g_peers[pi];
    if (!p->rbuf) p->rbuf = malloc(HDR + NET_MAXMSG);
    int r = recv(p->tcp, (char *)p->rbuf + p->rlen, HDR + NET_MAXMSG - p->rlen, 0);
    if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return;    // nothing yet (a freshly accepted socket)
    if (r <= 0) { drop_peer(pi); return; }
    p->rlen += r;
    for (;;) {
        if (p->rlen < HDR) break;
        int len = p->rbuf[0] | (p->rbuf[1] << 8);
        if (p->rlen < HDR + len) break;
        on_message(pi, p->rbuf, HDR + len, 1);
        memmove(p->rbuf, p->rbuf + HDR + len, p->rlen - HDR - len);
        p->rlen -= HDR + len;
    }
}

static void read_udp(void)
{
    uint8_t pkt[NET_MAXMSG + 16];
    struct sockaddr_in from;
    int fl = sizeof from;
    int n = recvfrom(g_udp, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
    if (n < 4) return;
    if (!memcmp(pkt, PROBE, 4)) {
        if (g_mode != NET_HOST) return;
        char ans[128];
        int m = snprintf(ans + 4, sizeof ans - 4, "%d|#%08lx|%s", g_session_players, (unsigned long)g_session_id, g_session);
        memcpy(ans, ANSWER, 4);
        sendto(g_udp, ans, 4 + m, 0, (struct sockaddr *)&from, fl);
        return;
    }
    if (memcmp(pkt, UDP_MAGIC, 4) || n < 4 + HDR) return;
    uint8_t *buf = pkt + 4;
    n -= 4;
    if (g_mode == NET_HOST) {
        int pi = buf[3];
        if (pi <= 0 || pi >= NET_MAX_PEERS || g_peers[pi].tcp == INVALID_SOCKET) return;
        // learn / refresh the peer's UDP address (same host as its TCP connection)
        g_peers[pi].udp = from;
        g_peers[pi].have_udp = 1;
        on_message(pi, buf, n, 0);
    } else {
        on_message(0, buf, n, 0);
    }
}

static void accept_peer(void)
{
    struct sockaddr_in a;
    int al = sizeof a;
    SOCKET s = accept(g_listen, (struct sockaddr *)&a, &al);
    if (s == INVALID_SOCKET) return;
    int id = -1;
    for (int i = 1; i < NET_MAX_PEERS; i++)
        if (g_peers[i].tcp == INVALID_SOCKET) { id = i; break; }
    if (id < 0) { closesocket(s); return; }
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
    g_peers[id].tcp = s;
    g_peers[id].rlen = 0;
    g_peers[id].have_udp = 0;
    mlog("net: peer %d connected from %s", id, inet_ntoa(a.sin_addr));
    uint8_t b = (uint8_t)id;
    uint8_t buf[HDR + 1];
    build(buf, NM_YOUR_ID, 0, (uint8_t)id, &b, 1);
    tcp_send_raw(id, buf, sizeof buf);
    for (int i = 1; i < NET_MAX_PEERS; i++) {        // tell the newcomer who is here
        if (i == id || g_peers[i].tcp == INVALID_SOCKET) continue;
        uint8_t o = (uint8_t)i;
        build(buf, NM_PEER_JOIN, 0, (uint8_t)id, &o, 1);
        tcp_send_raw(id, buf, sizeof buf);
    }
    for (int i = 1; i < NET_MAX_PEERS; i++) {        // and everyone else about the newcomer
        if (i == id || g_peers[i].tcp == INVALID_SOCKET) continue;
        build(buf, NM_PEER_JOIN, 0, (uint8_t)i, &b, 1);
        tcp_send_raw(i, buf, sizeof buf);
    }
    enqueue(NM_PEER_JOIN, 0, &b, 1);
}

static DWORD WINAPI net_thread(void *arg)
{
    (void)arg;
    while (!g_quit) {
        fd_set rs;
        FD_ZERO(&rs);
        if (g_udp != INVALID_SOCKET) FD_SET(g_udp, &rs);
        if (g_listen != INVALID_SOCKET) FD_SET(g_listen, &rs);
        for (int i = 0; i < NET_MAX_PEERS; i++)
            if (g_peers[i].tcp != INVALID_SOCKET) FD_SET(g_peers[i].tcp, &rs);
        struct timeval tv = {0, 20000};
        if (rs.fd_count == 0) { Sleep(20); continue; }
        if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        if (g_udp != INVALID_SOCKET && FD_ISSET(g_udp, &rs)) read_udp();
        if (g_listen != INVALID_SOCKET && FD_ISSET(g_listen, &rs)) accept_peer();
        for (int i = 0; i < NET_MAX_PEERS; i++)
            if (g_peers[i].tcp != INVALID_SOCKET && FD_ISSET(g_peers[i].tcp, &rs)) read_tcp(i);
    }
    return 0;
}

static void start_thread(void)
{
    g_quit = 0;
    g_thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
}

// ---- session control -------------------------------------------------------------------
int net_host(int port, int loopback)
{
    init_once();
    net_stop();
    g_port = port ? port : NET_PORT;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons((u_short)g_port);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(g_udp, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof a) || listen(g_listen, 4) ||
        bind(g_udp, (struct sockaddr *)&a, sizeof a)) {
        mlog("net: cannot host on port %d (error %d)", g_port, WSAGetLastError());
        net_stop();
        return 0;
    }
    g_mode = NET_HOST;
    g_my_id = 0;
    g_session_id = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (DWORD)(uintptr_t)&a;
    start_thread();
    mlog("net: hosting on port %d", g_port);
    return 1;
}

int net_join(const char *host, int port)
{
    init_once();
    net_stop();
    g_port = port ? port : NET_PORT;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", g_port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) {
        mlog("net: cannot resolve %s", host);
        return 0;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, (int)res->ai_addrlen);
    fd_set ws, es;
    FD_ZERO(&ws); FD_SET(s, &ws);
    FD_ZERO(&es); FD_SET(s, &es);
    struct timeval tv = {3, 0};
    int ok = select(0, NULL, &ws, &es, &tv) > 0 && FD_ISSET(s, &ws);
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (!ok) {
        mlog("net: cannot connect to %s:%d", host, g_port);
        closesocket(s);
        freeaddrinfo(res);
        return 0;
    }
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
    memcpy(&g_host_udp, res->ai_addr, sizeof g_host_udp);
    freeaddrinfo(res);
    g_peers[0].tcp = s;
    g_peers[0].rlen = 0;
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    bind(g_udp, (struct sockaddr *)&any, sizeof any);
    // wait for our id
    uint8_t hdr[HDR + 1];
    int got = 0;
    DWORD t0 = GetTickCount();
    while (got < (int)sizeof hdr && GetTickCount() - t0 < 3000) {
        int r = recv(s, (char *)hdr + got, sizeof hdr - got, 0);
        if (r <= 0) break;
        got += r;
    }
    if (got < (int)sizeof hdr || hdr[2] != NM_YOUR_ID) {
        mlog("net: no answer from %s", host);
        net_stop();
        return 0;
    }
    g_my_id = hdr[HDR];
    g_mode = NET_CLIENT;
    start_thread();
    net_send(0, NM_YOUR_ID, NULL, 0, 0);     // registers our UDP address with the host
    mlog("net: joined %s:%d as player %d", host, g_port, g_my_id);
    return 1;
}

void net_stop(void)
{
    if (!g_inited) return;
    g_quit = 1;
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (g_peers[i].tcp != INVALID_SOCKET) closesocket(g_peers[i].tcp);
        g_peers[i].tcp = INVALID_SOCKET;
        g_peers[i].have_udp = 0;
        free(g_peers[i].rbuf);
        g_peers[i].rbuf = NULL;
        g_peers[i].rlen = 0;
    }
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    if (g_udp != INVALID_SOCKET) closesocket(g_udp);
    g_listen = g_udp = INVALID_SOCKET;
    g_mode = NET_OFF;
    g_my_id = 0;
}

int net_discover(net_found *out, int max, int wait_ms)
{
    init_once();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&any, sizeof any);
    struct sockaddr_in b = {0};
    b.sin_family = AF_INET;
    b.sin_port = htons(NET_PORT);
    b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s, PROBE, 4, 0, (struct sockaddr *)&b, sizeof b);
    b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);          // a host on this PC
    sendto(s, PROBE, 4, 0, (struct sockaddr *)&b, sizeof b);
    // every network's own broadcast address: the plain broadcast only leaves by one of them
    // (virtual LANs such as Radmin VPN or Hamachi are usually not that one)
    INTERFACE_INFO ii[32];
    DWORD got = 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, ii, sizeof ii, &got, NULL, NULL) == 0) {
        for (DWORD i = 0; i < got / sizeof ii[0]; i++) {
            if (!(ii[i].iiFlags & IFF_UP) || (ii[i].iiFlags & IFF_LOOPBACK)) continue;
            u_long ad = ii[i].iiAddress.AddressIn.sin_addr.s_addr, mask = ii[i].iiNetmask.AddressIn.sin_addr.s_addr;
            b.sin_addr.s_addr = ad | ~mask;
            sendto(s, PROBE, 4, 0, (struct sockaddr *)&b, sizeof b);
        }
    }
    char ids[16][12];
    int n = 0;
    DWORD t0 = GetTickCount();
    while (n < max && (int)(GetTickCount() - t0) < wait_ms) {
        fd_set rs;
        FD_ZERO(&rs); FD_SET(s, &rs);
        struct timeval tv = {0, 50000};
        if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        char pkt[256];
        struct sockaddr_in from;
        int fl = sizeof from;
        int r = recvfrom(s, pkt, sizeof pkt - 1, 0, (struct sockaddr *)&from, &fl);
        if (r < 5 || memcmp(pkt, ANSWER, 4)) continue;
        pkt[r] = 0;
        const char *addr = inet_ntoa(from.sin_addr);
        const char *bar = strchr(pkt + 4, '|');
        char id[12] = "";
        if (bar && bar[1] == '#') {                     // "players|#session id|name"
            lstrcpynA(id, bar + 2, 9);
            bar = strchr(bar + 1, '|');
        }
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].addr, addr) || (id[0] && i < 16 && !strcmp(ids[i], id))) dup = 1;   // the same host on another network
        if (dup) continue;
        lstrcpynA(out[n].addr, addr, sizeof out[n].addr);
        out[n].players = atoi(pkt + 4);
        lstrcpynA(out[n].name, bar ? bar + 1 : "", sizeof out[n].name);
        if (n < 16) lstrcpyA(ids[n], id);
        n++;
    }
    closesocket(s);
    return n;
}
