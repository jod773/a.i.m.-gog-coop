// Language selection + Data\Lang\<code>\ file redirection.
#pragma once
#include "util.h"

#define MAX_LANGS 16

typedef struct {
    char code[16];
    char name[64];      // display name, in the game code page
} lang_t;

extern lang_t g_langs[MAX_LANGS];
extern int g_nlangs;
extern int g_lang;                  // index into g_langs
extern char g_ini[MAX_PATH];        // the mod's own ini file

void lang_init(const char *ini_name, int cp, const char *native_code, const char *native_name);
const char *lang_code(void);
int lang_find(const char *code);
void lang_select(int idx);          // persist + switch redirection
// Map a path to its language override, if one exists. Returns 1 if `out`
// was filled with a different path.
int lang_redirect(const char *in, char *out, size_t outsz);
void lang_install_file_hooks(void);
