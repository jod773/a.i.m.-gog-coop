// Dynamic lighting for the Direct3D 9 renderer (d3d9wrap.c).
//
// The games light their world with fixed-function lights (a sun and an ambient level). This
// adds point lights - explosions, missiles, shots - per pixel: after each lit world draw the
// same geometry is drawn again with our shaders, adding the light on top. The pixel shader
// repeats the draw's texture stages (stage 0 and 1) once with the extra light as the diffuse
// colour and once with none; the difference is what the light adds, and the stages' alpha
// blends it exactly like the game's draw (the terrain's layers blend by vertex alpha). The game's
// own look stays as it was; with no lights nearby nothing extra is drawn.
//
// The game adapters report the lights of the frame (d9light_add) from their frame hooks.
#define COBJMACROS
#include <d3d9.h>
#include <math.h>
#include <string.h>
#include "util.h"
#include "d9light.h"

extern char g_ini[MAX_PATH];

#define MAXL 8
#define MAXADD 48
typedef struct { float p[3], c[3], r; } light;
static light g_add[MAXADD], g_cur[MAXL];
static int g_nadd, g_ncur, g_enabled = -1;
static IDirect3DVertexShader9 *g_vs[4];      // [has vertex colour | 2 texture coordinate sets]
static IDirect3DPixelShader9 *g_ps;
static int g_failed, g_passes, g_dbg = -1;
static DWORD g_skip_fvf, g_skip_op;
// harness "lightdbg <view> [fvf colorop]": [Dev] D9LightDebug views; draws of that FVF (and stage 0
// colour op) get no light (to find the draw behind an artefact)
void d9light_debug(int mode, DWORD fvf, DWORD op) { g_dbg = mode; g_skip_fvf = fvf; g_skip_op = op; }

int d9light_enabled(void)
{
    if (g_enabled < 0) g_enabled = GetPrivateProfileIntA("Video", "DynamicLights", 1, g_ini) != 0;
    return g_enabled;
}
void d9light_set_enabled(int on)
{
    g_enabled = on != 0;
    WritePrivateProfileStringA("Video", "DynamicLights", on ? "1" : "0", g_ini);
}

// Lights close to one another (a burst of shots, the pieces of one explosion) become one light,
// so a cluster does not add up to a blinding spot.
void d9light_add(const float pos[3], const float rgb[3], float radius)
{
    for (int i = 0; i < g_nadd; i++) {
        light *l = &g_add[i];
        float dx = l->p[0] - pos[0], dy = l->p[1] - pos[1], dz = l->p[2] - pos[2];
        float m = 0.6f * (l->r < radius ? l->r : radius);
        if (dx * dx + dy * dy + dz * dz > m * m) continue;
        float wa = l->c[0] + l->c[1] + l->c[2], wb = rgb[0] + rgb[1] + rgb[2], wt = wa + wb > 0 ? wa + wb : 1;
        for (int k = 0; k < 3; k++) {
            l->p[k] = (l->p[k] * wa + pos[k] * wb) / wt;
            float mx = l->c[k] > rgb[k] ? l->c[k] : rgb[k];
            l->c[k] = l->c[k] + rgb[k] * 0.35f;            // grows a little, up to 1.6x the brighter one
            if (l->c[k] > mx * 1.6f) l->c[k] = mx * 1.6f;
        }
        if (radius > l->r) l->r = radius;
        return;
    }
    if (g_nadd >= MAXADD) return;
    light *l = &g_add[g_nadd++];
    memcpy(l->p, pos, 12);
    memcpy(l->c, rgb, 12);
    l->r = radius;
}

// ---- explosions and missiles (both games) ------------------------------------------------------
// An explosion object gets one flash when it first appears. It is remembered for as long as it
// exists (explosion objects outlive their flash), so it never flashes again.
#define FLASH_MS 650
typedef struct { const void *o; DWORD born, seen; float p[3], k; } blast;
static blast g_ex[96];
static float g_scale = 1.0f;          // world units of 1 m-ish (Clan Wars 1, A.I.M. smaller)

void d9light_scale(float s) { g_scale = s; }

void d9light_explosion(const void *key, const float p[3], const float cam[3])
{
    DWORD now = GetTickCount();
    int free_slot = -1;
    float same = 60.0f * g_scale;
    for (int i = 0; i < 96; i++) {
        blast *e = &g_ex[i];
        if (e->o == key && now - e->seen < 400) {
            float dx = e->p[0] - p[0], dy = e->p[1] - p[1];
            if (dx * dx + dy * dy < same * same) { e->seen = now; return; }
        }
        if (free_slot < 0 && (!e->o || now - e->seen > 400)) free_slot = i;
    }
    if (free_slot < 0) return;
    blast *e = &g_ex[free_slot];
    e->o = key;
    e->born = e->seen = now;
    memcpy(e->p, p, 12);
    e->p[2] += 3.0f * g_scale;
    // the player's own muzzle flashes: a faint glow on the glider, not a blinding one
    float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2], near_ = 40.0f * g_scale;
    e->k = dx * dx + dy * dy + dz * dz < near_ * near_ ? 0.12f : 1.0f;
}

// a shot's light fades in as it leaves the player (no glare on the player's own glider)
void d9light_missile(const float p[3], const float cam[3])
{
    float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz) / g_scale;
    float k = d < 30.0f ? 0.0f : d > 120.0f ? 1.0f : (d - 30.0f) / 90.0f;
    if (k <= 0) return;
    float pos[3] = {p[0], p[1], p[2] + 3.0f * g_scale}, c[3] = {0.55f * k, 0.4f * k, 0.2f * k};
    d9light_add(pos, c, 130.0f * g_scale);
}

static void flashes(void)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < 96; i++) {
        blast *e = &g_ex[i];
        if (!e->o || now - e->born >= FLASH_MS) continue;
        float t = 1.0f - (float)(now - e->born) / FLASH_MS;
        float k = t * t * 3.2f * e->k;
        float c[3] = {1.0f * k, 0.6f * k, 0.25f * k};
        d9light_add(e->p, c, 230.0f * g_scale);
    }
}

static float g_test[5];       // test light: offset from the camera (right, up, forward), radius (harness "testlight")
static float g_view[9];       // camera right, up, forward (from the last view matrix)
void d9light_test(float dx, float dy, float dz, float r) { g_test[0] = dx; g_test[1] = dy; g_test[2] = dz; g_test[3] = r; g_test[4] = !g_test[4]; }

// the frame's lights: the strongest MAXL (by intensity over distance to the camera)
void d9light_frame(const float cam[3])
{
    g_ncur = 0;
    flashes();
    if (g_test[4]) {
        float p[3], c[3] = {2.5f, 1.4f, 0.6f};
        for (int k = 0; k < 3; k++) p[k] = cam[k] + g_test[0] * g_view[k] + g_test[1] * g_view[3 + k] + g_test[2] * g_view[6 + k];
        d9light_add(p, c, g_test[3]);
    }
    float score[MAXADD];
    for (int i = 0; i < g_nadd; i++) {
        float dx = g_add[i].p[0] - cam[0], dy = g_add[i].p[1] - cam[1], dz = g_add[i].p[2] - cam[2];
        score[i] = (g_add[i].c[0] + g_add[i].c[1] + g_add[i].c[2]) * g_add[i].r / (sqrtf(dx * dx + dy * dy + dz * dz) + 50.0f);
    }
    while (g_ncur < MAXL) {
        int best = -1;
        for (int i = 0; i < g_nadd; i++) if (score[i] > 0 && (best < 0 || score[i] > score[best])) best = i;
        if (best < 0) break;
        g_cur[g_ncur++] = g_add[best];
        score[best] = 0;
    }
    // the rest (more than MAXL lights) join the nearest kept one: no light pops in and out between frames
    for (int i = 0; i < g_nadd && g_ncur; i++) {
        if (score[i] <= 0) continue;
        int best = 0;
        float bd = 1e30f;
        for (int j = 0; j < g_ncur; j++) {
            float dx = g_cur[j].p[0] - g_add[i].p[0], dy = g_cur[j].p[1] - g_add[i].p[1], dz = g_cur[j].p[2] - g_add[i].p[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = j; }
        }
        for (int k = 0; k < 3; k++) {
            float lim = g_cur[best].c[k] * 1.5f + 0.2f;
            g_cur[best].c[k] += g_add[i].c[k] * 0.5f;
            if (g_cur[best].c[k] > lim) g_cur[best].c[k] = lim;
        }
    }
    static int last = -1;
    static int logged;
    if ((g_ncur > 0) != (last > 0) && logged++ < 6) mlog("d9light: %d lights (of %d), %d passes since", g_ncur, g_nadd, g_passes);
    last = g_ncur;
    g_nadd = 0;
}

int d9light_count(void) { return d9light_enabled() ? g_ncur : 0; }

// ---- shaders -----------------------------------------------------------------------------------
static const char k_vs[] =
    "float4x4 proj : register(c0);\n"
    "float4x4 world : register(c4);\n"
    "float4x4 view : register(c8);\n"
    "struct I { float4 p : POSITION; float3 n : NORMAL;\n"
    "#ifdef COLOR\n float4 c : COLOR0;\n#endif\n"
    " float2 t : TEXCOORD0;\n"
    "#ifdef TEX2\n float2 t1 : TEXCOORD1;\n#endif\n"
    "};\n"
    "struct O { float4 p : POSITION; float4 t : TEXCOORD0; float3 w : TEXCOORD1; float3 n : TEXCOORD2;\n"
    " float4 c : TEXCOORD3; float z : TEXCOORD4; };\n"
    "O main(I i) {\n"
    " O o; float4 p = float4(i.p.xyz, 1);\n"
    " float4 w = mul(p, world); o.p = mul(mul(w, view), proj); o.w = w.xyz;\n"
    " o.n = mul(i.n, (float3x3)world); o.z = mul(w, view).z;\n"
    "#ifdef TEX2\n o.t = float4(i.t, i.t1);\n#else\n o.t = i.t.xyxy;\n#endif\n"
    "#ifdef COLOR\n o.c = i.c;\n#else\n o.c = 1;\n#endif\n"
    " return o; }\n";

// c0-7 light position, 1 / radius; c8-15 light colour; c16 (count, -, fog mode, debug view);
// c17 fog (start, end, density); c18 (texture 0 bound, texture 1 bound, stage 0 / 1 coordinate set);
// c19/c20 stage 0 (colour op, arg1, arg2, alpha op) (alpha arg1, arg2); c21/c22 stage 1;
// c23 texture factor; c24 diffuse colour and alpha (w < 0: the vertex colour)
static const char k_ps[] =
    "sampler s0 : register(s0);\n"
    "sampler s1 : register(s1);\n"
    "float4 lp[8] : register(c0);\n"
    "float4 lc[8] : register(c8);\n"
    "float4 misc : register(c16);\n"
    "float4 fog : register(c17);\n"
    "float4 tx : register(c18);\n"
    "float4 st[4] : register(c19);\n"
    "float4 tf : register(c23);\n"
    "float4 md : register(c24);\n"
    "float4 tt[4] : register(c25);\n"                         // texture transforms (stage 0 u, v; stage 1 u, v)
    // a stage argument: D3DTA_ select (0 diffuse 1 current 2 texture 3 factor) + 8 complement + 16 alpha replicate
    "float4 targ(float a, float4 cur, float4 dif, float4 tex) {\n"
    " float s = fmod(a, 8); float f = floor(a / 8);\n"
    " float4 v = s < 0.5 ? dif : s < 1.5 ? cur : s < 2.5 ? tex : s < 3.5 ? tf : float4(0, 0, 0, 0);\n"
    " if (fmod(f, 2) > 0.5) v = 1 - v;\n"
    " if (f > 1.5) v = v.aaaa;\n"
    " return v; }\n"
    "float4 top(float op, float4 a, float4 b, float ca, float ta, float da) {\n"
    " if (op < 2.5) return op < 1.5 ? b : (op < 2.5 ? a : b);\n"
    " if (op < 3.5) return b;\n"
    " if (op < 4.5) return a * b;\n"
    " if (op < 5.5) return a * b * 2;\n"
    " if (op < 6.5) return a * b * 4;\n"
    " if (op < 7.5) return a + b;\n"
    " if (op < 8.5) return a + b - 0.5;\n"
    " if (op < 9.5) return (a + b - 0.5) * 2;\n"
    " if (op < 10.5) return a - b;\n"
    " if (op < 11.5) return a + b - a * b;\n"
    " if (op < 12.5) return lerp(b, a, da);\n"
    " if (op < 13.5) return lerp(b, a, ta);\n"
    " if (op < 14.5) return lerp(b, a, tf.a);\n"
    " if (op < 15.5) return a + b * (1 - ta);\n"
    " if (op < 16.5) return lerp(b, a, ca);\n"
    " return a * b; }\n"
    // stages 0 and 1 for the diffuse colour dif; textures t0, t1
    "float4 stages(float4 dif, float4 t0, float4 t1) {\n"
    " float4 cur = dif;\n"
    " [unroll] for (int k = 0; k < 2; k++) {\n"
    "  float4 s = st[k * 2], u = st[k * 2 + 1]; float4 t = k == 0 ? t0 : t1;\n"
    "  if (s.x < 1.5) break;\n"
    "  float3 rgb = top(s.x, targ(s.y, cur, dif, t), targ(s.z, cur, dif, t), cur.a, t.a, dif.a).rgb;\n"
    "  float al = s.w < 1.5 ? cur.a : top(s.w, targ(u.x, cur, dif, t), targ(u.y, cur, dif, t), cur.a, t.a, dif.a).a;\n"
    "  cur = saturate(float4(rgb, al)); }\n"
    " return cur; }\n"
    "float4 main(float4 t : TEXCOORD0, float3 w : TEXCOORD1, float3 n : TEXCOORD2, float4 c : TEXCOORD3, float z : TEXCOORD4) : COLOR {\n"
    " float2 c0 = tx.z > 0.5 ? t.zw : t.xy, c1 = tx.w > 0.5 ? t.zw : t.xy;\n"
    " c0 = float2(dot(float3(c0, 1), tt[0].xyz), dot(float3(c0, 1), tt[1].xyz));\n"
    " c1 = float2(dot(float3(c1, 1), tt[2].xyz), dot(float3(c1, 1), tt[3].xyz));\n"
    " float4 t0 = tx.x > 0.5 ? tex2D(s0, c0) : float4(1, 1, 1, 1);\n"
    " float4 t1 = tx.y > 0.5 ? tex2D(s1, c1) : float4(1, 1, 1, 1);\n"
    " float3 N = normalize(n); float3 acc = 0;\n"
    " [unroll] for (int k = 0; k < 8; k++) {\n"
    "  float3 L = lp[k].xyz - w; float d = length(L);\n"
    "  float a = saturate(1 - d * lp[k].w); a *= a;\n"
    "  float nl = saturate(dot(N, L / max(d, 0.001)) * 0.6 + 0.4);\n"
    "  acc += lc[k].rgb * (a * nl * (k < misc.x ? 1 : 0)); }\n"
    " acc = acc / (1 + 0.35 * acc) * misc.y;\n"             // soft cap: clusters do not burn out to white
    " float f = 1;\n"
    " if (misc.z > 2.5) { float e = fog.z * z; f = exp(-e * e); }\n"
    " else if (misc.z > 1.5) f = exp(-fog.z * z);\n"
    " else if (misc.z > 0.5) f = saturate((fog.y - z) / max(fog.y - fog.x, 0.001));\n"
    " float4 dc = md.w < 0 ? c : md;\n"
    " float4 lit = stages(float4(dc.rgb * acc, dc.a), t0, t1);\n"
    " float4 dark = stages(float4(0, 0, 0, dc.a), t0, t1);\n"
    " float3 add = max(lit.rgb - dark.rgb, 0) * f;\n"
    " if (misc.w > 2.5 && misc.w < 3.5) return float4(saturate(1 - length(lp[0].xyz - w) * lp[0].w).xxx, 1);\n"
    " if (misc.w > 1.5 && misc.w < 2.5) return float4(frac(w * 0.01), 1);\n"
    " if (misc.w > 0.5 && misc.w < 1.5) return float4(1, 0, 1, 1);\n"
    " if (misc.w > 6.5 && misc.w < 7.5) return float4(c.rgb, 1);\n"
    " if (misc.w > 5.5 && misc.w < 6.5) return float4(t0.rgb, 1);\n"
    " if (misc.w > 4.5 && misc.w < 5.5) return float4(f.xxx, 1);\n"
    " if (misc.w > 3.5 && misc.w < 4.5) return float4(acc, 1);\n"
    " if (misc.w > 7.5 && misc.w < 8.5) return float4(lit.aaa, 1);\n"
    " if (misc.w > 8.5) return float4(add, 1);\n"
    " return float4(add, lit.a); }\n";

typedef HRESULT (WINAPI *compile_fn)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                                      const char *, UINT, UINT, void **, void **);

static void *compile(compile_fn cf, const char *src, const char *target, int color, int tex2)
{
    struct { const char *n, *v; } defs[3] = {{NULL, NULL}, {NULL, NULL}, {NULL, NULL}};
    int k = 0;
    if (color) { defs[k].n = "COLOR"; defs[k++].v = "1"; }
    if (tex2) { defs[k].n = "TEX2"; defs[k++].v = "1"; }
    void *code = NULL, *err = NULL;
    HRESULT hr = cf(src, strlen(src), "d9light", defs, NULL, "main", target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        typedef void *(WINAPI *ptr_fn)(void *);
        if (err) mlog("d9light: %s compile failed: %s", target, (char *)((ptr_fn)(*(void ***)err)[3])(err));
        return NULL;
    }
    return code;
}

static int make_shaders(IDirect3DDevice9 *dev)
{
    if (g_ps) return 1;
    if (g_failed) return 0;
    g_failed = 1;
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn cf = m ? (compile_fn)GetProcAddress(m, "D3DCompile") : NULL;
    if (!cf) { mlog("d9light: d3dcompiler_47.dll not found, no dynamic lighting"); return 0; }
    typedef void *(WINAPI *ptr_fn)(void *);
    typedef ULONG (WINAPI *rel_fn)(void *);
    void *b[5];
    for (int i = 0; i < 4; i++) b[i] = compile(cf, k_vs, "vs_3_0", i & 1, i & 2);
    b[4] = compile(cf, k_ps, "ps_3_0", 0, 0);
    int ok = 1;
    for (int i = 0; i < 5; i++) ok = ok && b[i];
    for (int i = 0; ok && i < 4; i++)
        ok = SUCCEEDED(IDirect3DDevice9_CreateVertexShader(dev, ((ptr_fn)(*(void ***)b[i])[3])(b[i]), &g_vs[i]));
    if (ok) ok = SUCCEEDED(IDirect3DDevice9_CreatePixelShader(dev, ((ptr_fn)(*(void ***)b[4])[3])(b[4]), &g_ps));
    for (int i = 0; i < 5; i++) if (b[i]) ((rel_fn)(*(void ***)b[i])[2])(b[i]);
    mlog("d9light: shaders %s", ok ? "ready" : "FAILED");
    if (!ok) return 0;
    g_failed = 0;
    return 1;
}

void d9light_device_lost(void)      // (the shaders are not pool resources: they survive a Reset)
{
}

// ---- the light pass -----------------------------------------------------------------------------
static struct {
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    DWORD ab, src, dst, zw, zf, fog, op, bias, cw;
} g_saved;

static void transpose(float *o, const D3DMATRIX *m)
{
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[c * 4 + r] = m->m[r][c];
}

// A texture stage argument for the shader: the D3DTA_ selector (diffuse, current, texture,
// factor; others: 0) + 8 complement + 16 alpha replicate.
static float sarg(DWORD a)
{
    DWORD s = a & D3DTA_SELECTMASK;
    float v = s <= D3DTA_TFACTOR ? (float)s : 4.0f;
    if (a & D3DTA_COMPLEMENT) v += 8.0f;
    if (a & D3DTA_ALPHAREPLICATE) v += 16.0f;
    return v;
}

// stages 0 and 1 of the draw -> c18 - c24 (pc + 72)
static int stage_consts(IDirect3DDevice9 *dev, DWORD fvf, float *pc)
{
    int ntc = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (int i = 0; i < 2; i++) {
        DWORD op = 0, a1 = 0, a2 = 0, aop = 0, b1 = 0, b2 = 0, tci = 0;
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_COLOROP, &op);
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_COLORARG1, &a1);
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_COLORARG2, &a2);
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_ALPHAOP, &aop);
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_ALPHAARG1, &b1);
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_ALPHAARG2, &b2);
        IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_TEXCOORDINDEX, &tci);
        if (i == 1) {
            DWORD op0 = 0;
            IDirect3DDevice9_GetTextureStageState(dev, 0, D3DTSS_COLOROP, &op0);
            if (op0 == D3DTOP_DISABLE) op = D3DTOP_DISABLE;
        }
        float *s = pc + 76 + i * 8;           // c19 / c21
        // texture + light (the terrain's base layer): the added light would show no texture and
        // outshine the layers above it; it lights the texture instead
        if (op == D3DTOP_ADD || op == D3DTOP_ADDSIGNED || op == D3DTOP_ADDSIGNED2X || op == D3DTOP_ADDSMOOTH) op = D3DTOP_MODULATE;
        // 2x / 4x brightness (made for a dim sun-lit colour): the same light as plain layers
        if (op == D3DTOP_MODULATE2X || op == D3DTOP_MODULATE4X) op = D3DTOP_MODULATE;
        s[0] = (float)op; s[1] = sarg(a1); s[2] = sarg(a2); s[3] = (float)aop;
        s[4] = sarg(b1); s[5] = sarg(b2); s[6] = 0; s[7] = 0;
        IDirect3DBaseTexture9 *t = NULL;
        if (op != D3DTOP_DISABLE) IDirect3DDevice9_GetTexture(dev, i, &t);
        if (t) IDirect3DBaseTexture9_Release(t);
        // generated coordinates (reflections) or a missing coordinate set: the texture counts as white
        int usable = t && (tci >> 16) == 0 && (int)(tci & 0xffff) < ntc;
        pc[72 + i] = usable ? 1.0f : 0.0f;
        pc[74 + i] = (tci & 0xffff) >= 1 ? 1.0f : 0.0f;
        // texture transform (animated sprites pick their frame of a sheet this way): c25 - c28
        float *tt = pc + 100 + i * 8;
        tt[0] = 1; tt[1] = 0; tt[2] = 0; tt[4] = 0; tt[5] = 1; tt[6] = 0;
        DWORD ttf = 0;
        if (usable) IDirect3DDevice9_GetTextureStageState(dev, i, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
        if (ttf) {
            if (ttf != D3DTTFF_COUNT2) return 0;           // projected / 3D coordinates: not repeated
            D3DMATRIX m;
            IDirect3DDevice9_GetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &m);
            tt[0] = m._11; tt[1] = m._21; tt[2] = m._31;
            tt[4] = m._12; tt[5] = m._22; tt[6] = m._32;
        }
    }
    DWORD tf = 0, src = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_TEXTUREFACTOR, &tf);
    pc[92] = ((tf >> 16) & 255) / 255.0f; pc[93] = ((tf >> 8) & 255) / 255.0f; pc[94] = (tf & 255) / 255.0f; pc[95] = (tf >> 24) / 255.0f;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_DIFFUSEMATERIALSOURCE, &src);
    if ((fvf & D3DFVF_DIFFUSE) && src == D3DMCS_COLOR1) {
        pc[96] = pc[97] = pc[98] = 1; pc[99] = -1;
    } else {
        D3DMATERIAL9 m;
        IDirect3DDevice9_GetMaterial(dev, &m);
        pc[96] = m.Diffuse.r; pc[97] = m.Diffuse.g; pc[98] = m.Diffuse.b; pc[99] = m.Diffuse.a < 0 ? 0 : m.Diffuse.a;
    }
    return 1;
}

// The draw about to be repeated: a lit, depth-tested world draw with normals, into the
// back buffer, by the fixed-function pipeline. Sets up the pass and returns 1.
int d9light_begin(IDirect3DDevice9 *dev, int game_vs)
{
    if (!d9light_count() || game_vs) return 0;
    DWORD v = 0, fvf = 0, srcb = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_LIGHTING, &v);
    if (!v) return 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &v);
    if (!v) return 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_VERTEXBLEND, &v);
    if (v) return 0;
    IDirect3DDevice9_GetFVF(dev, &fvf);
    if (!(fvf & D3DFVF_NORMAL) || (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW || !(fvf & D3DFVF_TEXCOUNT_MASK)) return 0;
    if (g_skip_fvf && fvf == g_skip_fvf) {
        DWORD op0 = 0;
        IDirect3DDevice9_GetTextureStageState(dev, 0, D3DTSS_COLOROP, &op0);
        if (!g_skip_op || op0 == g_skip_op) return 0;
    }
    DWORD ab = 0, dst = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &ab);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_DESTBLEND, &dst);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_SRCBLEND, &srcb);
    if (ab && dst == D3DBLEND_ONE) return 0;                // a glow: already additive
    DWORD zw = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    // see-through draws without depth (grass tufts, foliage): their layers blend over one another inside
    // the one draw, so a pass after it would add the light once per layer
    if (ab && !zw) return 0;
    if (ab && !(srcb == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA) && !(srcb == D3DBLEND_ONE && dst == D3DBLEND_ZERO))
        return 0;                                           // multiplying passes (detail, light maps)
    {   // self-lit things (shots, flames, sparks, the effects' sprites) glow by themselves
        D3DMATERIAL9 m;
        DWORD es = 0;
        IDirect3DDevice9_GetRenderState(dev, D3DRS_EMISSIVEMATERIALSOURCE, &es);
        IDirect3DDevice9_GetMaterial(dev, &m);
        if (es == D3DMCS_MATERIAL && m.Emissive.r + m.Emissive.g + m.Emissive.b > 1.2f) return 0;
    }
    IDirect3DSurface9 *rt = NULL, *bb = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    int main_target = rt && rt == bb;
    if (rt) IDirect3DSurface9_Release(rt);
    if (bb) IDirect3DSurface9_Release(bb);
    if (!main_target || !make_shaders(dev)) return 0;

    D3DMATRIX w, vw, pr;
    IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &w);
    IDirect3DDevice9_GetTransform(dev, D3DTS_VIEW, &vw);
    IDirect3DDevice9_GetTransform(dev, D3DTS_PROJECTION, &pr);
    for (int k = 0; k < 3; k++) { g_view[k] = vw.m[k][0]; g_view[3 + k] = vw.m[k][1]; g_view[6 + k] = vw.m[k][2]; }
    float vc[48];
    transpose(vc, &pr);
    transpose(vc + 16, &w);
    transpose(vc + 32, &vw);
    float pc[29 * 4];
    memset(pc, 0, sizeof pc);
    for (int i = 0; i < g_ncur; i++) {
        memcpy(pc + i * 4, g_cur[i].p, 12);
        pc[i * 4 + 3] = 1.0f / g_cur[i].r;
        memcpy(pc + 32 + i * 4, g_cur[i].c, 12);
    }
    DWORD fe = 0, fm = 0, fs = 0, fend = 0, fd = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_FOGENABLE, &fe);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_FOGTABLEMODE, &fm);
    if (!fm) IDirect3DDevice9_GetRenderState(dev, D3DRS_FOGVERTEXMODE, &fm);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_FOGSTART, &fs);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_FOGEND, &fend);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_FOGDENSITY, &fd);
    pc[64] = (float)g_ncur;
    pc[65] = 1.0f;
    if (g_dbg < 0) g_dbg = GetPrivateProfileIntA("Dev", "D9LightDebug", 0, g_ini);
    pc[67] = (float)g_dbg;
    // D3DFOG_EXP 1, EXP2 2, LINEAR 3 -> 2, 3, 1
    pc[66] = !fe ? 0.0f : fm == D3DFOG_LINEAR ? 1.0f : fm == D3DFOG_EXP ? 2.0f : fm == D3DFOG_EXP2 ? 3.0f : 0.0f;
    memcpy(&pc[68], &fs, 4);
    memcpy(&pc[69], &fend, 4);
    memcpy(&pc[70], &fd, 4);
    if (!stage_consts(dev, fvf, pc)) return 0;

    IDirect3DDevice9_GetVertexShader(dev, &g_saved.vs);
    IDirect3DDevice9_GetPixelShader(dev, &g_saved.ps);
    g_saved.ab = ab;
    g_saved.src = srcb;
    g_saved.dst = dst;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZWRITEENABLE, &g_saved.zw);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZFUNC, &g_saved.zf);
    g_saved.fog = fe;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_BLENDOP, &g_saved.op);

    int ntc = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    IDirect3DDevice9_SetVertexShader(dev, g_vs[((fvf & D3DFVF_DIFFUSE) ? 1 : 0) | (ntc >= 2 ? 2 : 0)]);
    IDirect3DDevice9_SetPixelShader(dev, g_ps);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, vc, 12);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, pc, 29);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, ab && srcb == D3DBLEND_SRCALPHA ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_DEPTHBIAS, &g_saved.bias);
    { float b = -0.00004f; DWORD bd; memcpy(&bd, &b, 4); IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, bd); }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_COLORWRITEENABLE, &g_saved.cw);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, g_saved.cw & 7);   // later passes may blend with the destination alpha
    g_passes++;
    return 1;
}

void d9light_end(IDirect3DDevice9 *dev)
{
    IDirect3DDevice9_SetVertexShader(dev, g_saved.vs);
    IDirect3DDevice9_SetPixelShader(dev, g_saved.ps);
    if (g_saved.vs) IDirect3DVertexShader9_Release(g_saved.vs);
    if (g_saved.ps) IDirect3DPixelShader9_Release(g_saved.ps);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, g_saved.ab);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, g_saved.src);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, g_saved.dst);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, g_saved.op);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, g_saved.zw);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, g_saved.zf);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, g_saved.fog);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, g_saved.bias);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, g_saved.cw);
}
