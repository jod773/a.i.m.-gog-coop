// Co-op (A.I.M. 2): the shared session core plus this game's modules.
#pragma once
#include <stdint.h>
#include <windows.h>
#include "../common/coop_core.h"

int coop_in_flight(void);
uint8_t *coop_mechanoid_save(uint8_t *mechanoid, int *len);   // malloc'ed
uint8_t *coop_mechanoid_load(const uint8_t *blob, int len);
uint8_t *coop_spawn_glider(uint8_t *mechanoid, const char *name, const char *org, const float m[16]);
void coop_remove_glider(uint8_t *glider);
void coop_drive(uint8_t *glider, const float m[16], const float vel[3]);

// mirror.c: host-authoritative NPC gliders
void mirror_install(void);
void mirror_frame(void);              // every frame while connected and flying
void mirror_peer_joined(int peer);    // host: send the full roster
void mirror_reset(void);              // session ended
int mirror_handle(int type, int from, const uint8_t *data, int len);   // 1 if handled

// notify.c: HUD notices and player status
void notify_install(void);
void notify_frame(void);
void notify_session_started(void);
int notify_handle(int type, int from, const uint8_t *data, int len, const char *name);

// questlog.c: shared quest log
void questlog_install(void);
void questlog_frame(void);
void questlog_peer_joined(int peer);
void questlog_reset(void);
uint8_t *questlog_blob(int *len);      // malloc'ed serialized log
void questlog_apply(const uint8_t *blob, int len);
int questlog_handle(int type, int from, const uint8_t *data, int len);

// lobby.c
void aim2_lobby_install(void);
void lobby_set_status(const char *text);
int lobby_load_world(const char *file);   // starts loading from the main menu; 0 if not possible
