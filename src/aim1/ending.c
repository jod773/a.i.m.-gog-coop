// A.I.M. co-op: the end of the game is everyone's.
//
// The story ends in a building: its script's _PROCESS(4) / _PROCESS(5) (0x422550, stdcall(int)) sets game state
// 0x677124 = 7 / 6, the event byte 0x677120, the app's quit flag +0x44 and +0x3e of the open building screen
// (app +0x2a4, NULL in flight: _PROCESS anywhere else writes through NULL). When that screen closes, CycleGame
// (0x4245fa) shows the win text and the credits (0x41e930, thiscall(app, 1 = state 6 / 2 = state 7)) and goes back to
// the main menu, where coop.c closes the session. Only the player whose script ran it saw the ending: the host's guests
// were left flying alone, and a guest who ended it in its copy of the world just left.
// Now the host sends MSG_GAME_END to everyone, and a guest's ending goes to the host first (the host decides the story),
// which ends the game for everyone. A player in a building ends it as its script would; anyone else (flying, in a
// panel, down) gets the credits at the next frame (ProcessInput, before anything is drawn) and then the main menu.
#include "coop.h"
#include "../common/net.h"
#include <stdlib.h>
#include <string.h>

#define A_PROCESS     0x00422550      // the scripts' _PROCESS(n): 4 / 5 = the two endings
#define A_THE_END     0x0041e930      // the win text and the credits
#define A_GAME_STATE  0x00677124      // 2 = leave the game, 6 / 7 = the endings
#define A_GAME_EVENT  0x00677120      // byte: CycleGame acts on the state after this frame
#define APP_QUIT      0x44
#define APP_BUILDING  0x2a4           // the open menu (the building screen in a base; NULL in flight)

enum { MSG_GAME_END = 56 };           // int n: the game ends with _PROCESS(n)

typedef char (__stdcall *process_fn)(int);
typedef void (THISCALL *the_end_fn)(uint8_t *, int);
static process_fn o_process;
static int g_pending;                 // an ending to show at the next frame

static int in_game(void)              // a world is up (flying, in a building or a panel, or down)
{
    return G_APP && *(int *)(G_APP + APP_MODE) != 0 && !*(int *)(G_APP + APP_QUIT) && !lobby_waiting();
}

static void end_here(int n)
{
    // in a building: as its script does (app +0x2a4 is any open menu - a panel's in flight - but only the
    // building's closing leads CycleGame to the credits)
    if (G_APP && coop_in_base() && *(uint8_t **)(G_APP + APP_BUILDING)) o_process(n);
    else g_pending = n;
}

static void end_game(int n)
{
    if (net_mode() == NET_CLIENT) {               // the host ends it for everyone, us included
        net_send(0, MSG_GAME_END, &n, sizeof n, 1);
        mlog("ending: the game ends here (_PROCESS(%d)) - asking the host to end it for everyone", n);
        return;
    }
    if (net_mode() == NET_HOST) {
        net_send(NET_ALL, MSG_GAME_END, &n, sizeof n, 1);
        mlog("ending: the game ends (_PROCESS(%d)) - for everyone", n);
    }
    end_here(n);
}

static char __stdcall h_process(int n)
{
    if (n != 4 && n != 5) return o_process(n);
    end_game(n);
    return 1;
}

// ProcessInput (respawn.c), every frame in flight and while down
void ending_frame(void)
{
    if (!g_pending || !G_APP) return;
    int n = g_pending;
    g_pending = 0;
    mlog("ending: the end of the game (_PROCESS(%d)) - the credits", n);
    CALL(A_THE_END, the_end_fn)(G_APP, n == 4 ? 2 : 1);
    // then the main menu: "leave the game" as ESC does - CycleGame frees the world after this frame. (The quit
    // flag alone, set in a frame, makes CycleGame return without that: the game flew on.)
    *(int *)A_GAME_STATE = 2;
    *(uint8_t *)A_GAME_EVENT = 1;
    mlog("ending: credits over - back to the main menu");
}

int ending_handle(int type, int from, const uint8_t *data, int len)
{
    if (type != MSG_GAME_END) return 0;
    int n = len == sizeof(int) ? *(const int *)data : 0;
    if ((n != 4 && n != 5) || !in_game() || g_pending) return 1;
    if (net_mode() == NET_HOST) {
        mlog("ending: player %d finished the game", from);
        end_game(n);
    } else if (net_mode() == NET_CLIENT && from == 0) {
        mlog("ending: the host finished the game (_PROCESS(%d))", n);
        end_here(n);
    }
    return 1;
}

// harness: "gameend <4|5>" - as if a building's script ended the game
void ending_test(int n)
{
    if (o_process && in_game()) end_game(n == 4 ? 4 : 5);
}

void ending_install(void)
{
    static const uint8_t pro[10] = {0x64, 0xa1, 0, 0, 0, 0, 0x8b, 0x4c, 0x24, 0x04};   // mov eax, fs:[0]; mov ecx, [esp+4]
    o_process = (process_fn)hook_jmp(A_PROCESS, pro, sizeof pro, (void *)h_process);
    mlog("ending: %s", o_process ? "the end of the game reaches every player" : "NOT installed (unexpected code)");
}
