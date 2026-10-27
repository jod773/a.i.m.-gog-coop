// A.I.M. co-op: UI strings. English defaults; any key can be overridden in
// AIMCoop.ini, section [Text] (the game's code page, 1250 for the GOG release).
#include "coop.h"
#include <string.h>

static const char *const k_text[][2] = {
    {"dbg_title", "DEBUG (tilde closes):"},
    {"dbg_next", "1. Teleport to the next building (%s)"},
    {"dbg_prev", "2. Teleport to the previous building"},
    {"dbg_enter", "3. Enter the nearest building"},
    {"dbg_pos", "4. Show my position"},
    {"dbg_player", "5. Teleport to another player"},
    {"dbg_teleported", "Teleported to %s"},
    {"dbg_passages", "6. Unlock passages (rating + 1000 crystals)"},
    {"dbg_unlocked", "Rating raised, 1000 crystals added"},
    {"name", "Your name: %s"},
    {"name_typing", "New name: %s_"},
    {"host_new", "Host a new co-op game"},
    {"host_load", "Host a co-op game from a save"},
    {"join", "Join %s's game (%d players)"},
    {"refresh", "Search the network again"},
    {"classic", "Classic network game (arena)"},
    {"none_found", "No co-op games found on the network"},
    {"connecting", "Connecting to %s..."},
    {"connect_failed", "Could not connect to %s"},
    {"receiving", "Receiving the host's world... %d%%"},
    {"host_failed", "Could not open the co-op session (port in use?)"},
    {"joined", "%s joined the game"},
    {"left", "%s left the game"},
    {"lost", "Connection to the host lost"},
    {"destroyed", "%s was destroyed"},
    {"spectating", "You are down - watching %s (TAB: next player)"},
    {"all_down", "Everyone is down"},
    {"down_hint", "A teammate can pick up your mechmind and fly it into a building to bring you back"},
    {"mind_picked", "You have %s's mechmind - fly into a building to bring them back"},
    {"mind_delivered", "%s's mechmind was delivered - they are coming back"},
    {"mind_returned", "%s is back - their mechmind left your hold"},
    {"respawning", "Your mechmind was delivered - coming back next to the host"},
    {"respawn_host", "Your mechmind was delivered, but the host can't respawn yet - rejoin from the main menu"},
    {"in_base", "%s entered %s"},
    {"in_menu", "%s is in the main menu"},
    {"chat_prompt", "Say:"},
    {"ql_done", "%s completed a task"},
    {"ql_failed", "%s failed a task"},
    {"ql_fail_host", "Co-op: the task is still active - only the host's game can fail it"},
    {"ql_new", "%s got a new task"},
    {"ql_journal", "%s has a new journal entry"},
};

const char *aim1_ui(const char *key)
{
    static char buf[16][256];
    static int slot;
    for (size_t i = 0; i < sizeof k_text / sizeof k_text[0]; i++) {
        if (strcmp(k_text[i][0], key)) continue;
        char *b = buf[slot++ & 15];
        GetPrivateProfileStringA("Text", key, k_text[i][1], b, sizeof buf[0], g_ini);
        return b;
    }
    return key;
}
