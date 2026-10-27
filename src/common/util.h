// Shared helpers for the A.I.M. and A.I.M. 2: Clan Wars co-op mod DLLs.
#pragma once
#include <windows.h>
#include <stdint.h>

#define THISCALL __attribute__((thiscall))

// Offset of the executable from its preferred base (0x400000). The Clan Wars
// build is ASLR-enabled; RA() relocates an address taken from the disassembly.
extern uintptr_t g_exe_delta;
void crashlog_install(void);
void crashlog_watch_main_thread(void);   // end the process if its main thread ends without it
void audio_install(void);
void gamepad_install(void);
int gamepad_enabled(void);
void gamepad_set_enabled(int on);
void gamepad_poll(void);
void gamepad_take(int *dx, int *dy, int btn[3]);
void gamepad_keys(uint8_t out[256]);
// textinput.c: a line typed on the keyboard (the game sees no keys meanwhile)
void textinput_start(const char *initial, int max);
int textinput_active(void);
void textinput_stop(void);
int textinput_poll(char *text, int n);   // 0 typing, 1 Enter, -1 Esc
#define RA(a) ((uintptr_t)(a) + g_exe_delta)

// ---- logging -------------------------------------------------------------
void log_open(const char *path);
void mlog(const char *fmt, ...);

// ---- paths -----------------------------------------------------------------
extern char g_root[MAX_PATH];      // game folder, with trailing backslash
void paths_init(void);
int file_exists(const char *path);
int dir_exists(const char *path);

// ---- text encoding ---------------------------------------------------------
// Convert UTF-8 to the game's code page (cp1251). Returns malloc'ed string.
char *utf8_to_cp(const char *s, int cp);

// ---- code patching ---------------------------------------------------------
// All patch helpers verify the expected original bytes first and refuse to
// patch (returning 0) when they don't match, so a wrong executable is never
// corrupted.
int patch_bytes(uintptr_t addr, const void *expect, const void *data, size_t len);
// Replace "call rel32" at addr with a call to target. Returns the old target.
uintptr_t patch_call(uintptr_t addr, void *target);
// Write "jmp rel32" at addr (len >= 5, rest padded with nops), after
// verifying `expect`. Returns a trampoline that executes the overwritten
// instructions and jumps back (instructions must be position independent).
void *hook_jmp(uintptr_t addr, const void *expect, size_t len, void *target);
// Redirect an imported function in every module's IAT.
int iat_hook_all(const char *dll, const char *func, void *repl, void **orig);
// Same, for a single module; *orig receives the slot's current target.
int iat_hook_one(HMODULE mod, const char *dll, const char *func, void *repl, void **orig);

// ---- small string map (key -> value) ----------------------------------------
typedef struct strmap strmap;
strmap *strmap_new(void);
void strmap_put(strmap *m, const char *k, const char *v);
const char *strmap_get(strmap *m, const char *k);
int strmap_count(strmap *m);
typedef void (*strmap_cb)(const char *k, const char *v, void *ctx);
void strmap_each(strmap *m, strmap_cb cb, void *ctx);
// Load "key<TAB>value" lines (UTF-8 file, converted to cp). '\n' and '\t'
// escapes are honoured; lines starting with '#' are comments.
int strmap_load_tsv(strmap *m, const char *path, int cp);
