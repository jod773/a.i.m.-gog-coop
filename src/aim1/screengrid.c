// Screen-space effect grid (A.I.M.): it overflowed its mesh at high resolutions (issue #6).
//
// CEffectRenderManager (0x656550) keeps one CPU mesh for its screen-space effects (e.g. the
// "TEX_INT_BM_S_BUMP" overlay of FUN_0040d8c0), allocated in FUN_004c4ca0 with room for 1200 vertices /
// 6000 indices - while the D3D buffer "SCREEN2" it is copied into holds 4800 / 24000.
// FUN_004c3e30 fills it with a grid over a screen rectangle in 32-pixel cells:
// (w/32 + 1) * (h/32 + 1) vertices and (w/32) * (h/32) * 6 indices, without checking the size.
// At 2560x1440 a large rectangle needs more than 6000 indices and the rest is written past the
// array into the heap (runs of 16-bit index triples over the next heap blocks). The game then
// crashes a moment later somewhere else: CObjectList::Run calling a "vtable" like 0401042b, or
// free() / ntdll heap errors, also when quitting. 1920x1080 stays (just) inside the arrays.
//
// Fix: allocate the CPU arrays as big as the D3D buffer (the 32-pixel grid of a whole 2560x1440
// screen fits, up to 2560x1600), and never build a grid that still would not fit: the rectangle is
// cut to the rows that fit (only for rectangles over 4000 cells, e.g. 3440x1440; logged).
#include "engine.h"
#include <stdlib.h>

#define A_FX_MESH_SIZES   0x004c4d0b   // FUN_004c4ca0: push 0x1770 (indices); push 0x4b0 (vertices)
#define A_SCREEN_GRID     0x004c3e30   // thiscall(mesh, x0, y0, x1, y1, a, b), ret 0x18
#define FX_INDICES        24000        // as the "SCREEN2" D3D buffer (FUN_004c4ca0)
#define FX_VERTICES       4800
#define MESH_MAX_VERTS    5            // mesh fields set by FUN_00428780 (vertices, indices)
#define MESH_MAX_INDICES  6

typedef void **(THISCALL *grid_fn)(void **, int, int, int, int, int, int);
static grid_fn o_grid;

static void **THISCALL screen_grid(void **mesh, int x0, int y0, int x1, int y1, int a, int b)
{
    int cw = abs(x1 - x0) >> 5, ch = abs(y1 - y0) >> 5;
    int maxv = (int)(intptr_t)mesh[MESH_MAX_VERTS], maxi = (int)(intptr_t)mesh[MESH_MAX_INDICES];
    if (cw > 0 && ((cw + 1) * (ch + 1) > maxv || cw * ch * 6 > maxi)) {
        int rows = maxv / (cw + 1) - 1;
        if (maxi / (cw * 6) < rows) rows = maxi / (cw * 6);
        if (rows < 0) rows = 0;
        static int nlog;
        if (nlog++ < 3) mlog("screengrid: %dx%d px grid too big for the mesh (%d/%d): %d of %d rows", abs(x1 - x0),
                             abs(y1 - y0), maxv, maxi, rows, ch);
        y1 = y1 >= y0 ? y0 + rows * 32 : y0 - rows * 32;
    }
    return o_grid(mesh, x0, y0, x1, y1, a, b);
}

void aim1_screengrid_install(void)
{
    // push 0x1770 / push 0x4b0 -> push 24000 / push 4800
    static const uint8_t was[10] = {0x68, 0x70, 0x17, 0, 0, 0x68, 0xb0, 0x04, 0, 0};
    uint8_t now[10] = {0x68, 0, 0, 0, 0, 0x68, 0, 0, 0, 0};
    *(uint32_t *)(now + 1) = FX_INDICES;
    *(uint32_t *)(now + 6) = FX_VERTICES;
    int ok = patch_bytes(A_FX_MESH_SIZES, was, now, sizeof now) != 0;
    static const uint8_t prologue[6] = {0x81, 0xec, 0x88, 0, 0, 0};   // sub esp, 0x88
    o_grid = (grid_fn)hook_jmp(A_SCREEN_GRID, prologue, sizeof prologue, (void *)screen_grid);
    ok &= o_grid != NULL;
    mlog("screengrid: effect mesh %s", ok ? "sized for high resolutions" : "fix NOT installed (unexpected code)");
}
