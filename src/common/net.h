// LAN transport for co-op: star topology around the host.
//   TCP: reliable messages (lobby, files, events)   UDP: frequent state
// Every message is [u16 len][u8 type][u8 from][payload], len = payload size.
// The host relays messages addressed to everyone. A network thread does the
// socket work; the game thread polls received messages with net_poll().
#pragma once
#include "util.h"

#define NET_PORT      28620
#define NET_MAX_PEERS 32           // including the host
#define NET_ALL       0xff
#define NET_MAXMSG    60000

typedef struct {
    uint8_t type, from;
    uint16_t len;
    uint8_t *data;                 // valid until the next net_poll()
} netmsg;

enum { NET_OFF, NET_HOST, NET_CLIENT };
enum {                              // transport-level message types (game types start at 16)
    NM_PEER_JOIN = 1,              // host -> all: [u8 id]
    NM_PEER_LEAVE = 2,             // host -> all: [u8 id]
    NM_YOUR_ID = 3,                // host -> new client: [u8 id]
    NM_DISCONNECTED = 4,           // local only: lost the host
};

void net_preload(void);                         // call at startup (loads the Winsock providers)
int net_host(int port, int loopback);           // returns 1 on success; loopback: 127.0.0.1 only
int net_join(const char *host, int port);       // connects (blocking, ~3 s timeout)
void net_stop(void);
int net_mode(void);
int net_my_id(void);                            // host = 0
int net_peer_connected(int id);
// Send to one peer (or NET_ALL). Clients send everything to the host, which
// forwards messages for others. reliable=0 uses UDP.
void net_send(int to, uint8_t type, const void *data, int len, int reliable);
// Returns 1 and fills m while messages are pending.
int net_poll(netmsg *m);

// LAN discovery: host answers broadcasts with its session name.
typedef struct { char addr[64]; char name[64]; int players; } net_found;
int net_discover(net_found *out, int max, int wait_ms);
void net_set_session_name(const char *name, int players);
