// Dynamic lights of A.I.M. 2 for the Direct3D 9 renderer (common/d9light.c): explosions
// flash, missiles and shots carry a light. Collected from the object list every frame.
#include "engine.h"
#include "coop.h"
#include "../common/d9light.h"

#define OBJ_TYPE_EXPLO   0x0f        // CExploObject
#define OBJ_TYPE_MISSILE 0x10        // CMissileObject (missiles and gun shots)

void lights_test(float dx, float dy, float dz, float r) { d9light_test(dx, dy, dz, r); }

void lights_frame(void)
{
    int display_is_d3d9(void);
    if (!VT_EXPLO || !display_is_d3d9() || !d9light_enabled() || !coop_in_flight()) return;
    const float *cam = (const float *)(G_PLAYER_GLIDER + OBJ_POS);
    for (void **nd = *(void ***)A_OBJLIST_HEAD; nd; nd = (void **)nd[1]) {
        uint8_t *o = (uint8_t *)nd[0];
        if (!o || !o[OBJ_ALIVE]) continue;
        int type = *(int *)(o + 0x54);
        if (type != OBJ_TYPE_EXPLO && type != OBJ_TYPE_MISSILE) continue;
        const float *p = (const float *)(o + OBJ_POS);
        float dx = p[0] - cam[0], dy = p[1] - cam[1];
        if (dx * dx + dy * dy > 1500.0f * 1500.0f) continue;
        if (type == OBJ_TYPE_EXPLO && *(uintptr_t *)o == VT_EXPLO) d9light_explosion(o, p, cam);
        else if (type == OBJ_TYPE_MISSILE && *(uintptr_t *)o == VT_MISSILE) d9light_missile(p, cam);
    }
    d9light_frame(cam);
}
