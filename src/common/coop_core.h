// Co-op session core shared by the A.I.M. and A.I.M. 2 mods: players, their
// pilots and glider states, puppets, world transfer (host -> joining player),
// personal pilot profiles. The engine work is done by the game's adapter
// through the cg_* functions below.
#pragma once
#include "util.h"
#include "net.h"

#define COOP_SYNC_SAVE "coop_sync.sav"   // world snapshot the host sends to joining players

// message types of the core (game modules use their own from 23 up)
enum { MSG_HELLO = 16, MSG_MECH = 17, MSG_STATE = 18, MSG_WORLD_BEGIN = 19, MSG_WORLD_FILE = 20,
       MSG_WORLD_END = 21, MSG_WORLD_REQ = 22, MSG_SAY = 45,
       MSG_PROFILE_UP = 53, MSG_PROFILE_BACK = 54 /* a joining player's profile file: backup on the host (#42) */ };

typedef struct { DWORD t; float m[16]; float vel[3]; } snapshot;   // received glider state

extern int g_coop_menu;               // messages are being handled inside a menu loop: don't touch objects

// ---- core API (coop_core.c) ----------------------------------------------------------------
void coop_core_install(void);         // reads [Coop] settings, hooks the harness command queue
void coop_frame(void);                // every frame of the flight loop, before rendering
void coop_menu_poll(void);            // from menu loops: keep the session alive
void coop_show_version(int on);       // the mod's version in the main menu's corner
void coop_menu_keepalive(void);       // from a panel opened in flight (hold, journal, ESC): keep our glider shown
void coop_world_tick(void);           // the world runs outside the flight loop: drive the other players' gliders
void coop_poll(void);
int coop_host(void);
int coop_join(const char *addr);
void coop_leave(void);
void coop_set_clan(int own);          // before joining: 0 = the host's clan, 1 = own clan
int coop_my_clan(void);
int coop_peer_same_clan(int peer);
int coop_glider_same_clan(const uint8_t *glider);
const char *coop_my_name(void);
void coop_set_name(const char *name); // the name other players see (saved to the ini)
int coop_world_gen(void);             // changes whenever the world is rebuilt
int coop_world_receiving(int *percent);
int coop_is_remote(const uint8_t *glider);
int coop_remote_peer(const uint8_t *glider);   // the player flying a puppet, -1 if none
const char *coop_remote_name(const uint8_t *glider);
uint8_t *coop_remote_glider(const char *name);
uint8_t *coop_remote_by_index(int n, const char **name);   // the n-th remote player's puppet in this world
void coop_remote_down(int peer, int game_owns);  // that player was destroyed: drop / remove the puppet until new pilot data
int coop_teammates_up(void);          // other players not destroyed (flying, in a base or a menu)
int coop_player_up(const char *name); // a connected player of that name, not destroyed
void coop_say_to(const uint8_t *glider, const char *text); // a notice for the player flying that puppet
uint32_t coop_player_color(void);     // radar / map / name colour of other players
int coop_sample(const snapshot *snap, int nsnap, float out[16], float vel[3]);
void coop_set_host_intent(int on);    // host as soon as the next game starts
// the adapter's InitMainMech hook: a new world is being built. Returns the
// player's own pilot (malloc'ed) when it replaces the host's, else NULL.
uint8_t *coop_new_world(int *len);
void coop_profile_save(void);
void coop_profile_keep(void);         // test: don't overwrite the profile file
void coop_profile_path(char *out, int n);

// ---- adapter (one per game) ------------------------------------------------------------------
int cg_in_flight(void);
uint8_t *cg_pilot_save(int *len);
int cg_pilot_name(char *out, int n);                  // the local pilot's name (1 if known)                     // the local pilot, serialized (malloc'ed)
int cg_pilot_saveable(void);                          // not dying etc.
uint8_t *cg_spawn_player(const uint8_t *blob, int len, const float m[16], const char *name, int same_clan);
void cg_drive(uint8_t *glider, const float m[16], const float vel[3]);
void cg_remove_glider(uint8_t *glider);
void cg_player_state(float m[16], float vel[3]);      // the local glider
void cg_player_setpos(float x, float y, float z);
void cg_save_world(const char *file);                 // SAVES\<file>
void cg_load_world(const char *file);                 // from the lobby or in flight
void cg_world_progress(int percent);                  // a world is being received
const char *cg_text(const char *key);                 // "joined", "left", "lost", "host_failed"
void coop_notify(const char *text);                   // a HUD notice (queued while not in flight)
// game modules: messages (1 = handled), per-frame work, session events
int cg_handle(int type, int from, const uint8_t *data, int len, const char *name);
void cg_frame(void);
void cg_session_started(void);
void cg_reset(void);
void cg_peer_joined(int peer);                        // host: the world went out to this player
int cg_command(const char *cmd, const char *arg, const char *line);   // harness, 1 = handled
// extra data kept in the player's profile (the A.I.M. 2 quest log)
uint8_t *cg_profile_extra(int *len);
void cg_profile_extra_restore(const uint8_t *data, int len);
