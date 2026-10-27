// A.I.M. 2: Clan Wars engine bindings (GOG AIMII.exe; addresses in addr_cw.h).
#pragma once
#include "../common/util.h"

// ---- engine string class (12 bytes, owns a heap buffer) --------------------
typedef struct {
    void *vt;
    char flag;
    char pad[3];
    char *buf;
} gstr;

typedef struct { int l, t, r, b; } grect;

// Virtual call helper: VFN(obj, offset, type)
#define VFN(obj, off, T) ((T)(*(void ***)(obj))[(off) / 4])

// ---- addresses: per executable ---------------------------------------------------
#include "addr.h"



void aim2_options_install(void);
void aim2_vfs_install(void);
void aim2_apply_language(void);
const char *aim2_ui(const char *key);   // small built-in UI strings per language

// settings.c
int aim2_display_mode_ini(void);
void aim2_display_install(void);
void aim2_display_apply(void *dlg, int mode);
void aim2_display_quality(void *dlg, int msaa, int vsync);
int aim2_save_anywhere(void);
void aim2_set_save_anywhere(int on);
void aim2_saves_install(void);
int aim2_intro_videos(void);
void aim2_set_intro_videos(int on);
void aim2_intro_install(void);
void aim2_modopts_install(void);
void aim2_shadows_install(void);
int aim2_shadow_distance(void);
void aim2_set_shadow_distance(int v);
extern void *g_reset_ui;

// coop.c
void aim2_coop_install(void);
void aim2_economy_install(void);
void aim2_debugmenu_install(void);
int names_enabled(void);
void names_set_enabled(int on);
void economy_dump(void);
void economy_run_hour(void);
int economy_auto_supply(void);
void economy_set_auto_supply(int on);
