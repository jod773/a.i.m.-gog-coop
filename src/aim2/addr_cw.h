// A.I.M. 2 Clan Wars GOG AIMII.exe (1.4.30, PE timestamp 0x5371514b, MSVC 10): addresses and layouts.
// 0 = not mapped yet (the feature using it stays off, see FEAT_*). Offsets/sizes marked
// "unverified" were carried over from the A.I.M. 2 layout and are not checked in this executable yet.
#pragma once
#define EXE_TIMESTAMP 0x5371514Bu
#define MOD_NAME               "Clan Wars co-op"
#define MOD_INI                "ClanWarsCoop.ini"
#define MOD_LOG                "ClanWarsCoop.log"
#define CRT_DLL                "msvcr100.dll"

#define A_ALLOC                RA(0x005e6136)   // void* __cdecl operator new(size_t)
#define A_GSTR_CTOR            RA(0x004aa110)   // gstr::gstr()
#define A_GSTR_CTOR_CSTR       RA(0x004ab1a0)   // gstr::gstr(const char*)
#define A_GSTR_DTOR            RA(0x004aa130)   //
#define SZ_COMBO               0x270   // (unverified)
#define A_TEXTDB_PTR           RA(0x0067f8f0)   // ITextDB* (vtbl+0x0c = (re)load, +0x18 = GetText)
#define A_OPT_LOADPARAMS       RA(0x00438ff0)   // void __thiscall(const char* ini)
#define A_OPT_SAVEPARAMS       RA(0x00438060)   // void __thiscall(const char* ini)
#define A_OPT_HIDEALL          RA(0x00437510)   //
#define A_OPT_TAB_GAME         RA(0x00437fd0)   //
#define G_PLAYER_MECH          (*(uint8_t **)RA(0x006db4ec))   // CPlayerMech* (pilot: name +4, mechanoid +0xac)
#define G_PLAYER_GLIDER        (*(uint8_t **)RA(0x006db4f4))   // CGliderObject*
#define G_GLIDER_MGR           ((void *)RA(0x0077fb58))   // CGliderManager
#define G_OBJECT_LIST          ((void *)RA(0x006dbab8))   // CObjectList
#define SZ_MECH                0xbc   // (unverified)
#define A_MECH_CTOR            RA(0x00537220)   // CMech::CMech()
#define SZ_MECHANOID           0xd70   // (unverified)
#define A_MECHANOID_CTOR       RA(0x005325e0)   // CMechanoid::CMechanoid()
#define A_MECHANOID_SAVE       RA(0x00533e30)   // CMechanoid::SaveToStream(stream*)
#define A_MECHANOID_LOAD       RA(0x00534040)   // CMechanoid::LoadFromStream(stream*)
#define A_STREAM_ALLOC         RA(0x0041b480)   // stream::Alloc(size)
#define A_GLIDER_INFO          RA(0x00425980)   // builds glider info (first 32 bytes = type name) from config
#define A_CREATE_GLIDER        RA(0x0056e780)   // CGliderManager::CreateGlider(name32, matrix*, name)
#define A_ASSIGN_MECH          RA(0x00581810)   // CGliderObject::AssignMechanoid(mech*, bool)
#define A_RENDER               RA(0x0041a5e0)   // CMMApp::Render(bool) (simulation + drawing)
#define A_OBJ_SETPOS           RA(0x005388f0)   // CMMObject::SetPosition(x, y, z) (updates the spatial grid)
#define G_WORLD                (*(uint8_t **)RA(0x00672a8c))   // CWorld*
#define A_WORLD_FIND_ORG       RA(0x0058ac60)   // CWorld::FindOrganization(const char*)
#define SZ_GROUP               0xd0   //
#define A_GROUP_CTOR           RA(0x005538d0)   // CMechGroup2::CMechGroup2()
#define VF_GROUP_INIT          0x04   // (unverified)
#define VF_GROUP_ADDMECH       0x10   // (unverified)
#define VF_GROUP_SETAI         0x84   // SetAI(type): 0 people, 1 trans, 2 hunter, other = basic (unverified)
#define VF_AI_SETORG           0x20   // (unverified)
#define GRP_NAME               0x04   // char[32] (unverified)
#define GRP_ORGNAME            0x24   // char[32] (unverified)
#define GRP_ORG                0x44   // COrganization* (unverified)
#define GRP_AI                 0xa4   // (unverified)
#define ORG_E0                 0xe0   // (unverified)
#define MECH_ORGNAME           0x44   // char[32] (unverified)
#define MECH_GROUP             0xb0   // (unverified)
#define MECH_NAME              0x04   // char[32] (unverified)
#define MECH_MECHANOID         0xac   // (unverified)
#define MECHANOID_GLIDER       0x50   // CGliderObject* currently flown (unverified)
#define MECHANOID_CONFIG       0x728   // (unverified)
#define OBJ_ALIVE              0x5f   // byte; 0 = CObjectList::Pack deletes the object (unverified)
#define OBJ_MATRIX             0x150   // float[16], row-major, translation at +0x180 (unverified)
#define OBJ_POS                0x180   // (unverified)
#define OBJ_PREV_POS           0x190   // (unverified)
#define GLIDER_VELOCITY        0x2a0   // float[3] (unverified)
#define GLIDER_MECH            0x1180   // CMech* (unverified)
#define GLIDER_AI              0x1188   // CBasicAI* (0 = not AI driven) (unverified)
#define A_APP_PTR              RA(0x0067d484)   // CMMApp*
#define A_SAVE_GAME            RA(0x00410b20)   // CMMApp::SaveGame(file, title*, 0, key*, 0)
#define A_LOAD_GAME            RA(0x004113e0)   // CMMApp::LoadGame(file, 1)
#define A_INGAME_LOAD_FLAG     RA(0x00671f88)   // byte: reload the world (quick load)
#define A_OBJLIST_HEAD         RA(0x006dbacc)   // CObjectList + 0x14: {obj, next} nodes
#define A_APP_PUMP             RA(0x004dfe70)   // CMMApp message pump
#define A_WNDCLASS             RA(0x0060df18)   // window class name passed to FindWindowA
#define S_FRAME_RENDER         RA(0x0041e882)   // call CMMApp::Render in CMMApp::CycleGame
#define S_INITMAINMECH_CREATE  RA(0x00415c9c)   // call CreateGlider in CMMApp::InitMainMech
#define S_PUMPS                RA(0x004138b9), RA(0x0048ecc8), RA(0x004e0925)   // calls of the message pump in menu loops
#define A_BTN_CREATE           RA(0x004ba2a0)   // CButton::Create(rect, text, 0, cmd)
#define A_BTN_SETCMD           RA(0x00486280)   //
#define A_MENU_BTN_CTOR        RA(0x00478530)   //
#define A_MAINUI_ONCMD         RA(0x00477c60)   //
#define A_MODS_ONCMD           RA(0x0049d250)   //
#define A_MODS_PROC            RA(0x0049d060)   //
#define VT_MODS                RA(0x00617be4)   //
#define A_LIST_CLEAR           RA(0x0049dec0)   //
#define A_LIST_PUSH            RA(0x005d9580)   //
#define A_MODREC_CTOR          RA(0x0049d6c0)   //
#define A_MENU_BTN_RENDER      RA(0x004875a0)   //
#define VT_MENU_BTN            RA(0x00615da4)   //
#define A_PANEL_BTN_RENDER     RA(0x00486d10)   //
#define VT_PANEL_BTN           RA(0x006110a4)   //
#define A_PANEL_GET            RA(0x004a8a50)   //
#define S_MAINMENU_PANEL       RA(0x004764cc)   // main menu: getting the button panel
#define S_MENU_BTN_SITES       RA(0x0047658a), RA(0x00476626), RA(0x004766c2), RA(0x0047676b), RA(0x00476807), RA(0x00476c21)   // main menu button creation calls
#define S_MAINUI_ONCMD_SLOTS   RA(0x0060cc70), RA(0x0060ccd0)   // CMainUI::OnCommand vtable slots
#define A_AI_RUN               RA(0x005a53a0)   // CBasicAI::Run
#define A_GROUP_RUN            RA(0x005510b0)   // CMechGroup2::Run
#define A_WORLD_RUN            RA(0x0058aa30)   // CWorld::RunWorld
#define A_WEAPON_FIRE          RA(0x00599860)   // weapon fire
#define A_GLIDER_DAMAGE        RA(0x0057efd0)   // CGliderObject::Damage
#define A_GLIDER_BOOM          RA(0x00581c60)   // CGliderObject::Boom
#define A_ADD_MESSAGE          RA(0x00433eb0)   // CMessager::AddMessage(gstr*, int, const char*)
#define A_MESSAGER_PTR         RA(0x0067d970)   // CMessager* (HUD messages)
#define A_RUN_BUILDING_MENU    RA(0x004163b0)   // CMMApp::RunBuildingMenu
#define A_MAINMENU_LOOP        RA(0x00416240)   // main menu loop
#define A_TEXKEY               RA(0x0043c000)   // texture key by name
#define S_OPT_INIT_LOADPARAMS  RA(0x0043b9ca)   // COptionDialog::Init -> LoadParams
#define S_OPT_CANCEL_LOADPARAMS RA(0x0043be2a), RA(0x00439398)   // Cancel -> LoadParams
#define S_OPT_GAME_TAB         RA(0x0043bdc6)   // command 0xf081 (Game tab)
#define S_OPT_APPLY_SAVE       RA(0x0043be13)   // command 0xf07e (Apply)
#define S_OPT_SHOW_SLOT        RA(0x00615ed8)   // COptionDialog vtable: Show (shows the pressed tab's controls)
#define VT_EXPLO               RA(0x00628e7c)   // CExploObject (object type 0x0f)
#define VT_MISSILE             RA(0x00628c5c)   // CMissileObject (type 0x10)
#define S_OPT_HIDE_SITES       RA(0x00437d78), RA(0x00437e38), RA(0x00437ee8), RA(0x00437f58), RA(0x00437ff8)   // HideAll calls in the tab handlers
#define G_QUESTLOG             (*(uint8_t **)RA(0x0067f8f8))   //
#define A_OUTSIDE_SAVE         RA(0x00672a84)   // bool, [CONFIG] OutsideSave (saving outside bases, F5/F9)
#define S_INTRO_VIDEOS         RA(0x004153fc)   // CMMApp::Init: play the intro videos
#define S_OUTSIDESAVE_DEFAULT  RA(0x0041582d)   // push 0 = OutsideSave default
#define A_VFS_OPEN             RA(0x004c31d0)   // CFileVFS::Open
#define A_PAK_OPEN             RA(0x0042d6e0)   // CPakManager::OpenFile
#define A_RENDER_APP_PTR       RA(0x006dad40)   // object with OnLostDevice (+0x14) / OnResetDevice (+0x18)
#define A_DEVICE_PTR           RA(0x0065c4d0)   // IDirect3DDevice8*
#define A_PRESENT_PP           RA(0x0065c5ac)   // the game's D3DPRESENT_PARAMETERS

// features enabled for this executable
#define FEAT_OPTIONS           1
#define FEAT_VFS               1
#define FEAT_SAVES             1
#define FEAT_INTRO             1
#define FEAT_DISPLAY_APPLY     1
#define FEAT_COOP              1
#define FEAT_LOBBY             1
#define A_COMBO_CTOR           RA(0x0043c8d0)
#define A_CHECKBOX_CTOR        RA(0x0043c410)
#define OUTSIDESAVE_STR        RA(0x0060e930)   // "OutsideSave" (pushed with its default)
#define MOD_NATIVE_LANG        "en"
#define MOD_NATIVE_LANG_NAME   "English"
#define MAINMENU_LOOP_PRO      0x55, 0x8b, 0xec, 0x6a, 0xff
#define VT_BINK_PLAYER         RA(0x0061d2d0)
#define VF_BINK_PLAY           0x1c
#define A_STREAM_STOP_TT_RET   RA(0x005e410a)   // sound stream stop (0x5e40b0): return address of its TerminateThread call
#define VFS_OPEN_PRO           0x55, 0x8b, 0xec, 0x53, 0x56
#define PAK_OPEN_PRO           0x55, 0x8b, 0xec, 0x51, 0x56
#define A_TRADE_PTR            RA(0x007a0678)
#define VT_TRADE               RA(0x00621074)
#define VF_TRADE_RUNHOUR       0x18
#define FEAT_ECONOMY           1
#define A_OBJLIST_FIND         RA(0x00554a70)
#define A_ENTER_BUILDING       RA(0x00420690)
#define A_FONT_PTR             RA(0x006dad20)
#define A_FONT_HEIGHT          RA(0x004da680)
#define A_FONT_TEXT            RA(0x004dc370)
#define S_CMDMENU_DRAW         RA(0x00408a50)
#define MECHANOID_NAME         0x274
#define A_PLAYER_NAME_PTR      RA(0x0067d16c)   // char *: the name given in the opening dialogue (or the SMT code)
#define MECH_SHOWNAME          0x24
#define GLIDER_TARGET          0xc44
#define A_PASSAGES             RA(0x0067d21c)
#define A_GET_LOCINFO          RA(0x0040a580)
#define A_GAMEDATA             RA(0x0067d140)
#define A_VIDEO_IFACE          RA(0x006dab20)
#define VT_GROUP_AIS           RA(0x0062302c), RA(0x00623114), RA(0x006231fc), RA(0x006232e4)
#define VF_AI_COLOR            0x50
#define S_BIGMAP_ARROW         RA(0x0042a894)   // CRadar::RenderBigMap -> draw the player's arrow (radar, x, y, pos)
#define S_SHADOW_LOD_CMP       RA(0x0057b071)   // CGliderObject::Draw: comiss lod, [A_SHADOW_LOD_MAX] (3.05: shadows up to LOD 3)
#define A_SHADOW_LOD_MAX       RA(0x00625138)
#define A_BIGMAP_UNITS         RA(0x0065b424)   // float: world units -> big map cells
#define A_BIGMAP_ARROW         RA(0x00427050)
#define A_RENDER_STATE_PTR     RA(0x006dad08)   // object CRender::SetColor (A_SET_COLOR) works on
#define A_SET_COLOR            RA(0x004cd170)
