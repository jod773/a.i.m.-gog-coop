// The mod's options, shared by both games' settings menus (see modopts.c).
#pragma once
#include "util.h"

typedef struct {
    const char *id;            // text keys opt_<id>, opt_<id>_desc, opt_<id>_<value>
    const char *label, *desc;  // English defaults
    int nvals;
    const char *vals[8];
    int (*get)(void);
    void (*set)(int v);
    int restart;               // takes effect at the next start
    int reset;                 // applied with a device Reset
} modopt;

void modopts_set_ui(const char *(*ui)(const char *key));   // the game's text lookup
void modopts_set_reset(int (*reset)(void));                 // live device Reset (returns 0 on failure)
const char *modopts_text(const char *key, const char *def);
void modopts_add(const modopt *o);
void modopts_add_common(int extras);   // display, gamepad, sound; extras: 1 shadow resolution, 2 dynamic lighting
int modopts_count(void);
const modopt *modopts_get(int i);
void modopts_line(int i, char *out, int n);   // "Label: value"
void modopts_desc(int i, char *out, int n);
void modopts_cycle(int i);                     // next value, applied at once where possible
const char *modopts_value(int i, int v);       // a value's text
void modopts_label(int i, char *out, int n);   // the name, "(restart)" if applied at the next start
void modopts_apply(const int *vals);           // set all options (vals[i] < 0: unchanged)