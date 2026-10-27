// Dynamic lighting for the Direct3D 9 renderer (see d9light.c).
#pragma once

int d9light_enabled(void);                    // [Video] DynamicLights
void d9light_set_enabled(int on);
// The game adapters, once per frame before it is drawn: the frame's point lights, then
// d9light_frame with the camera position (keeps the strongest few).
void d9light_add(const float pos[3], const float rgb[3], float radius);
// every frame for each existing explosion object (key: the object) / missile or shot
void d9light_explosion(const void *key, const float pos[3], const float cam[3]);
void d9light_missile(const float pos[3], const float cam[3]);
void d9light_scale(float s);                  // world units per metre-ish (Clan Wars: 1)
void d9light_frame(const float cam[3]);
int d9light_count(void);
void d9light_test(float dx, float dy, float dz, float r);   // toggles a test light near the camera
