// A.I.M. co-op: the shared session core plus this game's modules.
#pragma once
#include "engine.h"
#include "../common/coop_core.h"

extern char g_ini[MAX_PATH];

// text.c: UI strings (English defaults, overridable in AIMCoop.ini [Text])
const char *aim1_ui(const char *key);

// coop.c (adapter)
uint8_t *coop_mechanoid_save(uint8_t *mo, int *len);
uint8_t *coop_spawn_glider(const uint8_t *blob, int len, const float m[16]);
uint8_t *coop_spawn_player(const uint8_t *blob, int len, const float m[16], const char *name);
void coop_drive(uint8_t *g, const float m[16], const float vel[3]);
void coop_remove_glider(uint8_t *g);
int coop_in_flight(void);
extern int g_base_world;             // the host's world is running a tick while the host is in a base (#37)
void aim1_coop_install(void);

// mirror.c: host-authoritative NPCs, shots, deaths, world time
void mirror_install(void);
void mirror_frame(void);
void mirror_base_frame(void);
void mirror_reset(void);
void mirror_peer_joined(int peer);
int mirror_handle(int type, int from, const uint8_t *data, int len);

// questlog.c: shared quest log (union of all players' logs)
void questlog_frame(void);
void questlog_peer_joined(int peer);
int questlog_handle(int type, int from, const uint8_t *data, int len);
void questlog_reset(void);
void questlog_install(void);
void questlog_test(const char *cmd, const char *arg);

// respawn.c: spectator mode for a dead player, respawn by the mechmind (#24)
void respawn_install(void);
int respawn_spectating(void);
void respawn_player_down(int peer, const char *name, const uint8_t *puppet);
int respawn_handle(int type, int from, const uint8_t *data, int len);
void respawn_frame(void);
void respawn_reset(void);
void respawn_test(const char *cmd);

// killcredit.c: a guest's kills count for the guest
void killcredit_install(void);
int killcredit_handle(int type, int from, const uint8_t *data, int len);

// mapmark.c: the other players as pointers on the map screen
void mapmark_install(void);
void mapmark_frame(void);
void mapmark_reset(void);
void mapmark_test(void);

// loot.c: the host's things lying around (dropped loot), shown on the guests (#27)
void loot_install(void);
void loot_frame(void);
void loot_reset(void);
void loot_peer_joined(int peer);
int loot_handle(int type, int from, const uint8_t *data, int len);
void loot_test(void);

// lobby.c
void lobby_world_ready(void);        // client: the host's world arrived
int lobby_waiting(void);             // the lobby is waiting for the world
void aim1_lobby_install(void);
