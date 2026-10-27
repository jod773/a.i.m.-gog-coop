// Dynamic lights of A.I.M. for the Direct3D 9 renderer (common/d9light.c): explosions flash,
// missiles and shots carry a light. Collected from the object list every frame.
#include "engine.h"
#include "coop.h"
#include "../common/d9light.h"
#include <math.h>
#include <string.h>

// the object type at +0x54 (set by the constructors) and the class vtable
#define TYPE_EXPLO   0x0f            // CExploObject (vtable 0x52e0e8)
#define TYPE_MISSILE 0x10            // CMissileObject (vtable 0x52e008)
#define VT_EXPLO     0x0052e0e8
#define VT_MISSILE   0x0052e008

void aim1_lights_frame(void)
{
    int display_is_d3d9(void);
    if (!display_is_d3d9() || !d9light_enabled() || !coop_in_flight()) return;     // (only the Direct3D 9 renderer draws them)
    const float *cam = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        const uint8_t *o = (const uint8_t *)nd[0];
        if (!o || !o[OBJ_ALIVE]) continue;
        int type = *(const int *)(o + 0x54);
        if (type != TYPE_EXPLO && type != TYPE_MISSILE) continue;
        const float *p = (const float *)(o + OBJ_POS);
        float dx = p[0] - cam[0], dy = p[1] - cam[1];
        if (dx * dx + dy * dy > 1500.0f * 1500.0f) continue;
        uintptr_t vt = *(const uintptr_t *)o;
        if (type == TYPE_EXPLO && vt == VT_EXPLO) d9light_explosion(o, p, cam);
        else if (type == TYPE_MISSILE && vt == VT_MISSILE) d9light_missile(p, cam);
    }
    d9light_frame(cam);
}
// test: "objstat" - objects within 1500 by vtable
void aim1_objstat(void)
{
    if (!coop_in_flight()) return;
    struct { uintptr_t vt; int n; float d; } st[64];
    int ns = 0;
    const float *pp = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    for (void **nd = *(void ***)(A_OBJECT_LIST + OBJLIST_HEAD); nd; nd = (void **)nd[1]) {
        const uint8_t *o = (const uint8_t *)nd[0];
        if (!o) continue;
        const float *p = (const float *)(o + OBJ_POS);
        float dx = p[0] - pp[0], dy = p[1] - pp[1], d = sqrtf(dx * dx + dy * dy);
        if (d > 1500.0f) continue;
        uintptr_t vt = *(uintptr_t *)o;
        int k;
        for (k = 0; k < ns; k++) if (st[k].vt == vt) break;
        if (k == ns && ns < 64) { st[ns].vt = vt; st[ns].n = 0; st[ns].d = d; ns++; }
        if (k < ns) { st[k].n++; if (d < st[k].d) st[k].d = d; }
    }
    for (int k = 0; k < ns; k++) mlog("objstat: vt %06x x%d nearest %.0f", (unsigned)st[k].vt, st[k].n, st[k].d);
}
