// A.I.M. co-op: the end of the game is everyone's.
//
// The story ends with a script's _PROCESS(4) / _PROCESS(5) (0x422550, stdcall(int)): game state 0x677124 = 7 / 6, the
// event byte 0x677120 and the app's quit flag +0x44. CycleGame (0x4245fa) then shows the win text and the credits
// (0x41e930) and goes back to the main menu, where coop.c closes the session. Only the player whose script ran it saw the
// ending: the host's guests were left flying alone in a world nobody hosted any more, and a guest whose copy of the world
// ran it just left. Now the host sends MSG_GAME_END to everyone before its own ending, and a guest's ending goes to the
// host first (the host decides the story), which ends the game for everyone.
#include "coop.h"
#include "../common/net.h"
#include <stdlib.h>
#include <string.h>

#define A_PROCESS 0x00422550          // the scripts' _PROCESS(n): 4 / 5 = the two endings

enum { MSG_GAME_END = 56 };           // int n: the game ends with _PROCESS(n)

typedef char (__stdcall *process_fn)(int);
static process_fn o_process;

static int in_game(void)            // a world is up (flying, in a base or a panel, or down and spectating)
{
    return G_APP && *(int *)(G_APP + APP_MODE) != 0 && !*(int *)(G_APP + 0x44) && !lobby_waiting();
}

static void end_for_all(int n)
{
    net_send(NET_ALL, MSG_GAME_END, &n, sizeof n, 1);
    mlog("ending: the game ends (_PROCESS(%d)) - for everyone", n);
    o_process(n);
}

static char __stdcall h_process(int n)
{
    if (n != 4 && n != 5) return o_process(n);
    if (net_mode() == NET_HOST) { end_for_all(n); return 1; }
    if (net_mode() == NET_CLIENT) {               // the host ends it for everyone, us included
        net_send(0, MSG_GAME_END, &n, sizeof n, 1);
        mlog("ending: our script ends the game (_PROCESS(%d)) - asking the host to end it for everyone", n);
        return 1;
    }
    return o_process(n);
}

int ending_handle(int type, int from, const uint8_t *data, int len)
{
    if (type != MSG_GAME_END) return 0;
    int n = len == sizeof(int) ? *(const int *)data : 0;
    if ((n != 4 && n != 5) || !in_game()) return 1;
    if (net_mode() == NET_HOST) {
        mlog("ending: player %d finished the game", from);
        end_for_all(n);
    } else if (net_mode() == NET_CLIENT && from == 0) {
        mlog("ending: the host finished the game (_PROCESS(%d))", n);
        o_process(n);
    }
    return 1;
}

// harness: "gameend <4|5>" - as if a script ended the game
void ending_test(int n)
{
    if (o_process) h_process(n == 4 ? 4 : 5);
}

void ending_install(void)
{
    static const uint8_t pro[10] = {0x64, 0xa1, 0, 0, 0, 0, 0x8b, 0x4c, 0x24, 0x04};   // mov eax, fs:[0]; mov ecx, [esp+4]
    o_process = (process_fn)hook_jmp(A_PROCESS, pro, sizeof pro, (void *)h_process);
    mlog("ending: %s", o_process ? "the end of the game reaches every player" : "NOT installed (unexpected code)");
}
