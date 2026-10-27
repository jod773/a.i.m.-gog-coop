// A.I.M. (GOG AIM.exe, timestamp 0x43e99b7c): engine addresses and layouts.
// Same engine family as A.I.M. 2.
#pragma once
#include "../common/util.h"

#define VFN(obj, off, T) ((T)(*(void ***)(obj))[(off) / 4])
#define CALL(addr, T) ((T)(addr))

typedef struct { char s[32]; } name32;
typedef struct { void *vt; char flag; char *buf; } gstr;               // engine string
typedef struct { char owned; int cap; int pos; uint8_t *buf; } gstream; // memory stream

// ---- globals ----
#define A_APP_PTR        0x005b7498   // CMMApp*
#define G_APP            (*(uint8_t **)A_APP_PTR)
#define APP_MODE         0x250        // 0 start menu, 1 new game, 2 loaded game, 3 network (PvP)
#define G_PLAYER_GLIDER  (*(uint8_t **)0x00677df8)
#define G_PLAYER_MECHANOID ((uint8_t *)0x00678fec)
#define G_PLAYER_MECH    ((uint8_t *)0x00678f60)
#define A_GLIDER_MGR     0x00676be8
#define A_OBJECT_LIST    0x00685928   // CObjectList; +0x971c: list of all objects {obj, next}
#define OBJLIST_HEAD     0x971c
#define A_TEXT_DB_PTR    0x00576328
#define A_MESSAGER_PTR   0x00576438

// ---- functions ----
#define A_NEW            0x00514cc5   // operator new (cdecl)
#define A_DELETE         0x00514cf6   // operator delete (cdecl)
#define A_GSTR_CTOR_CSTR 0x00446fa0   // thiscall(gstr*, const char*)
#define A_GSTR_DTOR      0x00446d60   // thiscall(gstr*)
#define A_TEXT_LOOKUP    0x004373f0   // thiscall(textdb, key, gstr *out)
#define A_ADD_MESSAGE    0x00440e00   // thiscall(messager, gstr*, 1, 0): HUD line
#define A_RUN_MENU       0x0041a270   // thiscall(app, menu*, 1) -> result (6 = OK)
#define A_SAVE_GAME      0x0041d060   // thiscall(app, file, gstr *title, 1, name32 base (by value), 1)
#define A_LOAD_GAME      0x0041de60   // thiscall(app, file, char header_only)
#define A_CREATE_GLIDER  0x004ca5f0   // thiscall(mgr, name32 type (by value), float *m, const char *name)
#define A_ASSIGN_MECHANOID 0x004f8910 // thiscall(glider, mechanoid)
#define A_MECH_CTOR      0x00503be0   // pilot object (SZ_MECH), mechanoid embedded at +0x8c
#define A_MECHANOID_SAVE 0x004aefd0   // thiscall(mechanoid, gstream*, char full)
#define A_MECHANOID_LOAD 0x004b0d00   // thiscall(mechanoid, gstream*, char full)
#define A_MECHANOID_REPAIR 0x004acfc0 // thiscall(mechanoid): clears damage and destroyed flags
#define A_RENDER       0x00423510   // thiscall(app, int first): CMMApp::Render
#define A_DEVICE_CHECK   0x00497b40   // thiscall(app, int) -> char; called by every loop

// ---- layouts ----
#define SZ_MECH          0x64c8
#define MECH_MECHANOID   0x8c         // embedded CMechanoid
#define MECHANOID_GLIDER 0x40
#define MECHANOID_MECH   0x44
#define MECHANOID_TYPE   0x3ac        // 32 bytes passed to CreateGlider (model at +4)
#define OBJ_NAME         0x14
#define OBJ_MODEL        0x38
#define OBJ_TYPE         0x54         // 0x0e = glider
#define OBJ_ALIVE        0x5e         // 0 = CObjectList::Pack deletes it
#define OBJ_NET          0x6a
#define OBJ_LOCAL        0x7b
#define VF_OBJ_SETPOS    4            // thiscall(obj, x, y, z): moves it, keeping the map cells right
#define OBJ_MATRIX       0x150        // 4x4, translation at +0x180
#define OBJ_POS          0x180
#define OBJ_CELL_POS     0x190        // physics position; the map cell is looked up from it
#define GLIDER_VELOCITY  0x294
#define GLIDER_MECHANOID 0x10d4

// ---- start menu (CMMStartMenu) / list menus ----
#define A_START_MENU_CMD 0x0042e800   // thiscall(menu, cmd): ProcessCommand
#define VT_START_MENU    0x00522f74
#define A_NET_SEARCH_CALL 0x0042ea45  // call to the PvP server search dialog (Network button)
#define A_NET_SEARCH     0x0042c540
#define MENU_RESULT      0x40         // 2 new game, 3 network, 7 load (file at +0xdc)
#define MENU_DONE        0x3e
#define MENU_FILE        0xdc
#define A_LISTMENU_CTOR  0x0042abc0   // CLoadSaveMenu, network-location flavour (type 2)
#define A_LISTMENU_DTOR  0x00404580
#define VT_LISTMENU_NET  0x00523ba0   // 14 slots; 0x30 ProcessCommand, 0x34 GetGamesData
#define LISTREC_SIZE     0x230        // +0 key (file name), +0x84 caption, +0x12c autosave flag
