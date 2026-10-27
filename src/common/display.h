// Display modes (fullscreen / windowed / borderless) for Direct3D 8 games.
#pragma once
#include "util.h"

enum { DISPLAY_FULLSCREEN, DISPLAY_WINDOWED, DISPLAY_BORDERLESS };

// mode: DISPLAY_*; test: test-harness instance (fixed window position x,y,
// never fullscreen, never confines the real cursor). Install after the
// developer harness so its cursor hook stays underneath.
void display_install(int mode, int test, int x, int y);
int display_mode(void);
// Selects the mode applied by the next device Reset. Returns 0 if refused.
int display_select(int mode);
HWND display_window(void);
// called when the game window becomes active again
void display_on_activate(void (*cb)(void));
void *display_device(void);            // the game's IDirect3DDevice8 (NULL before CreateDevice)
// [Video] Antialiasing (0/2/4/8), VSync (fullscreen; -1 = the game's), Anisotropy (forced level, 0 = off)
int display_msaa(void);
void display_set_msaa(int n);          // applied by the next device Reset
int display_vsync(void);
void display_set_vsync(int v);          // -1 the game's setting, 0 off, 1 on
int display_aniso(void);
void display_set_aniso(int n);
int display_fps_limit(void);            // [Video] FrameLimit (0 = off)
void display_set_fps_limit(int fps);
