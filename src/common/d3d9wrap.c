// Direct3D 8 on Direct3D 9: the games keep talking Direct3D 8, this module
// implements those interfaces on top of a Direct3D 9 device ([Video]
// Renderer=d3d9). It is the base for the renderer improvements (effects need
// Direct3D 9 features); with Renderer=d3d8 the game's own Direct3D 8 is used.
//
// Every Direct3D 8 object is a small wrapper around its Direct3D 9 object; a
// Direct3D 9 resource finds its wrapper again through private data. Vertex
// shaders get dcl instructions from their Direct3D 8 declaration; pixel
// shaders (ps 1.x) are passed through.
#define COBJMACROS
#define INITGUID
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "d9light.h"

// ---- Direct3D 8 structures that differ from Direct3D 9 ---------------------------------------------
typedef struct {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
} PP8;

typedef struct {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width, Height;
} SURFDESC8;

typedef struct {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size, Width, Height, Depth;
} VOLDESC8;

typedef struct {
    char Driver[512], Description[512];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} ADAPTERID8;

#define CAPS8_SIZE  (53 * 4)          // D3DCAPS8 is the first part of D3DCAPS9

// {7C3B5E1A-2F4D-4B8E-9A61-3D2C8B4F0A11}
DEFINE_GUID(GUID_WRAP8, 0x7c3b5e1a, 0x2f4d, 0x4b8e, 0x9a, 0x61, 0x3d, 0x2c, 0x8b, 0x4f, 0x0a, 0x11);

// ---- wrappers ----------------------------------------------------------------------------------------
enum { K_SURF, K_TEX, K_CUBE, K_VOLTEX, K_VOL, K_VB, K_IB, K_SWAP };

typedef struct dev8 dev8;
typedef struct {
    void **vt;
    IUnknown *r;                      // the Direct3D 9 object
    dev8 *dev;
    int kind;
    BYTE *mirror;                     // dynamic buffers: CPU copy (readable), uploaded on Unlock
    UINT size, lock_off, lock_len;
    DWORD lock_flags;
    int scale;                        // render target textures: enlarged by this factor
} w8;

typedef struct {
    void **vt;
    LONG ref;
    IDirect3D9 *r;
} d3d8;

#define MAX_VS 4096
typedef struct {
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexShader9 *vs;       // NULL: fixed function with a declaration
    DWORD *decl8, *func8;
    UINT ndecl8, nfunc8;
    float consts[96 * 4];             // constants loaded by the declaration
    int cfirst[16], ccount[16], nconst;
} vshader;

struct dev8 {
    void **vt;
    LONG ref;
    IDirect3DDevice9 *r;
    d3d8 *parent;
    UINT base_vertex;
    DWORD cur_vs;
    vshader *vs[MAX_VS];
    IDirect3DSurface9 *cur_ds;
    int rt_scale;                     // the current render target is enlarged this much
    w8 *vb0, *ib;                     // stream 0 / index buffer (for redrawing scaled 2D quads)
    UINT stride0;
    struct sblock *rec;              // a state block being recorded (Begin/EndStateBlock)
    IDirect3DSurface9 *big_ds;        // depth buffer for enlarged targets (the game's one is too small)
    IDirect3DSurface9 *big_ds_for;    // ... standing in for this one of the game's
};

static int g_shadow_scale = 1;        // [Video] ShadowScale: the shadow render targets are this much larger

static void *vt_d3d8[16], *vt_dev8[97], *vt_surf[11], *vt_tex[22], *vt_cube[22], *vt_voltex[22], *vt_vol[11],
            *vt_vb[14], *vt_ib[14], *vt_swap[5];

static void **vt_of(int kind)
{
    switch (kind) {
    case K_SURF: return vt_surf;
    case K_TEX: return vt_tex;
    case K_CUBE: return vt_cube;
    case K_VOLTEX: return vt_voltex;
    case K_VOL: return vt_vol;
    case K_VB: return vt_vb;
    case K_IB: return vt_ib;
    default: return vt_swap;
    }
}

// The wrapper of a Direct3D 9 object (created on first sight). r carries the
// reference the caller gets; the wrapper shares the object's reference count.
static w8 *wrap(dev8 *d, void *obj, int kind)
{
    if (!obj) return NULL;
    IDirect3DResource9 *r = obj;
    w8 *w = NULL;
    DWORD sz = sizeof w;
    if (kind != K_SWAP && kind != K_VOL && SUCCEEDED(IDirect3DResource9_GetPrivateData(r, &GUID_WRAP8, &w, &sz)) && w) return w;
    if (kind == K_VOL && SUCCEEDED(IDirect3DVolume9_GetPrivateData((IDirect3DVolume9 *)obj, &GUID_WRAP8, &w, &sz)) && w) return w;
    w = calloc(1, sizeof *w);
    w->vt = vt_of(kind);
    w->r = obj;
    w->dev = d;
    w->kind = kind;
    if (kind == K_VOL) IDirect3DVolume9_SetPrivateData((IDirect3DVolume9 *)obj, &GUID_WRAP8, &w, sizeof w, 0);
    else if (kind != K_SWAP) IDirect3DResource9_SetPrivateData(r, &GUID_WRAP8, &w, sizeof w, 0);
    return w;
}

static void *unwrap(void *p) { return p ? ((w8 *)p)->r : NULL; }

static int tex_kind(IDirect3DBaseTexture9 *t)
{
    D3DRESOURCETYPE ty = IDirect3DBaseTexture9_GetType(t);
    return ty == D3DRTYPE_CUBETEXTURE ? K_CUBE : ty == D3DRTYPE_VOLUMETEXTURE ? K_VOLTEX : K_TEX;
}

// ---- IUnknown / IDirect3DResource8 for all wrappers --------------------------------------------------
static HRESULT WINAPI w_QI(w8 *w, REFIID iid, void **out) { (void)iid; *out = w; IUnknown_AddRef(w->r); return S_OK; }
static ULONG WINAPI w_AddRef(w8 *w) { return IUnknown_AddRef(w->r); }
static ULONG WINAPI w_Release(w8 *w)
{
    IUnknown *r = w->r;
    IUnknown_AddRef(r);
    ULONG n = IUnknown_Release(r);
    if (n == 1) {                      // the last reference: the object goes, and so does its wrapper
        if (w->kind == K_VOL) IDirect3DVolume9_FreePrivateData((IDirect3DVolume9 *)r, &GUID_WRAP8);
        else if (w->kind != K_SWAP) IDirect3DResource9_FreePrivateData((IDirect3DResource9 *)r, &GUID_WRAP8);
        n = IUnknown_Release(r);
        if (n == 0) { free(w->mirror); free(w); }
        return n;
    }
    return IUnknown_Release(r);
}
static HRESULT WINAPI w_GetDevice(w8 *w, void **out) { *out = w->dev; IDirect3DDevice9_AddRef(w->dev->r); w->dev->ref++; return S_OK; }
static HRESULT WINAPI w_SetPrivateData(w8 *w, REFGUID g, const void *d, DWORD n, DWORD f) { return IDirect3DResource9_SetPrivateData((IDirect3DResource9 *)w->r, g, d, n, f); }
static HRESULT WINAPI w_GetPrivateData(w8 *w, REFGUID g, void *d, DWORD *n) { return IDirect3DResource9_GetPrivateData((IDirect3DResource9 *)w->r, g, d, n); }
static HRESULT WINAPI w_FreePrivateData(w8 *w, REFGUID g) { return IDirect3DResource9_FreePrivateData((IDirect3DResource9 *)w->r, g); }
static DWORD WINAPI w_SetPriority(w8 *w, DWORD p) { return IDirect3DResource9_SetPriority((IDirect3DResource9 *)w->r, p); }
static DWORD WINAPI w_GetPriority(w8 *w) { return IDirect3DResource9_GetPriority((IDirect3DResource9 *)w->r); }
static void WINAPI w_PreLoad(w8 *w) { IDirect3DResource9_PreLoad((IDirect3DResource9 *)w->r); }
static D3DRESOURCETYPE WINAPI w_GetType(w8 *w) { return IDirect3DResource9_GetType((IDirect3DResource9 *)w->r); }

// ---- textures ------------------------------------------------------------------------------------------
static DWORD WINAPI t_SetLOD(w8 *w, DWORD l) { return IDirect3DBaseTexture9_SetLOD((IDirect3DBaseTexture9 *)w->r, l); }
static DWORD WINAPI t_GetLOD(w8 *w) { return IDirect3DBaseTexture9_GetLOD((IDirect3DBaseTexture9 *)w->r); }
static DWORD WINAPI t_GetLevelCount(w8 *w) { return IDirect3DBaseTexture9_GetLevelCount((IDirect3DBaseTexture9 *)w->r); }

static void surfdesc8(const D3DSURFACE_DESC *s, SURFDESC8 *d)
{
    d->Format = s->Format; d->Type = s->Type; d->Usage = s->Usage; d->Pool = s->Pool;
    d->MultiSampleType = s->MultiSampleType; d->Width = s->Width; d->Height = s->Height;
    UINT bpp = s->Format == D3DFMT_DXT1 ? 4 : (s->Format >= D3DFMT_DXT2 && s->Format <= D3DFMT_DXT5) ? 8 :
               (s->Format == D3DFMT_R5G6B5 || s->Format == D3DFMT_X1R5G5B5 || s->Format == D3DFMT_A1R5G5B5 ||
                s->Format == D3DFMT_A4R4G4B4 || s->Format == D3DFMT_D16 || s->Format == D3DFMT_L6V5U5 ||
                s->Format == D3DFMT_V8U8 || s->Format == D3DFMT_A8L8) ? 16 :
               (s->Format == D3DFMT_A8 || s->Format == D3DFMT_L8 || s->Format == D3DFMT_P8) ? 8 : 32;
    d->Size = s->Width * s->Height * bpp / 8;
}

static HRESULT WINAPI tx_GetLevelDesc(w8 *w, UINT l, SURFDESC8 *d)
{
    D3DSURFACE_DESC s;
    HRESULT hr = IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)w->r, l, &s);
    if (SUCCEEDED(hr) && w->scale > 1) { s.Width /= w->scale; s.Height /= w->scale; }
    if (SUCCEEDED(hr)) surfdesc8(&s, d);
    return hr;
}
static HRESULT WINAPI tx_GetSurfaceLevel(w8 *w, UINT l, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)w->r, l, &s);
    *out = SUCCEEDED(hr) ? wrap(w->dev, s, K_SURF) : NULL;
    if (*out) ((w8 *)*out)->scale = w->scale;
    return hr;
}
// Direct3D 8 accepted DISCARD / NOOVERWRITE on any lock; Direct3D 9 only on dynamic resources
#define LOCK_RETRY(call) do { HRESULT hr_ = call; if (FAILED(hr_) && (f & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) { f &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE); hr_ = call; } return hr_; } while (0)
static HRESULT WINAPI tx_LockRect(w8 *w, UINT l, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) { LOCK_RETRY(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)w->r, l, lr, rc, f)); }
static HRESULT WINAPI tx_UnlockRect(w8 *w, UINT l) { return IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)w->r, l); }
static HRESULT WINAPI tx_AddDirtyRect(w8 *w, const RECT *rc) { return IDirect3DTexture9_AddDirtyRect((IDirect3DTexture9 *)w->r, rc); }

static HRESULT WINAPI cb_GetLevelDesc(w8 *w, UINT l, SURFDESC8 *d)
{
    D3DSURFACE_DESC s;
    HRESULT hr = IDirect3DCubeTexture9_GetLevelDesc((IDirect3DCubeTexture9 *)w->r, l, &s);
    if (SUCCEEDED(hr)) surfdesc8(&s, d);
    return hr;
}
static HRESULT WINAPI cb_GetCubeMapSurface(w8 *w, D3DCUBEMAP_FACES f, UINT l, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DCubeTexture9_GetCubeMapSurface((IDirect3DCubeTexture9 *)w->r, f, l, &s);
    *out = SUCCEEDED(hr) ? wrap(w->dev, s, K_SURF) : NULL;
    return hr;
}
static HRESULT WINAPI cb_LockRect(w8 *w, D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) { LOCK_RETRY(IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)w->r, face, l, lr, rc, f)); }
static HRESULT WINAPI cb_UnlockRect(w8 *w, D3DCUBEMAP_FACES f, UINT l) { return IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)w->r, f, l); }
static HRESULT WINAPI cb_AddDirtyRect(w8 *w, D3DCUBEMAP_FACES f, const RECT *rc) { return IDirect3DCubeTexture9_AddDirtyRect((IDirect3DCubeTexture9 *)w->r, f, rc); }

static void voldesc8(const D3DVOLUME_DESC *s, VOLDESC8 *d)
{
    d->Format = s->Format; d->Type = s->Type; d->Usage = s->Usage; d->Pool = s->Pool;
    d->Width = s->Width; d->Height = s->Height; d->Depth = s->Depth; d->Size = s->Width * s->Height * s->Depth * 4;
}
static HRESULT WINAPI vt_GetLevelDesc(w8 *w, UINT l, VOLDESC8 *d)
{
    D3DVOLUME_DESC s;
    HRESULT hr = IDirect3DVolumeTexture9_GetLevelDesc((IDirect3DVolumeTexture9 *)w->r, l, &s);
    if (SUCCEEDED(hr)) voldesc8(&s, d);
    return hr;
}
static HRESULT WINAPI vt_GetVolumeLevel(w8 *w, UINT l, void **out)
{
    IDirect3DVolume9 *v = NULL;
    HRESULT hr = IDirect3DVolumeTexture9_GetVolumeLevel((IDirect3DVolumeTexture9 *)w->r, l, &v);
    *out = SUCCEEDED(hr) ? wrap(w->dev, v, K_VOL) : NULL;
    return hr;
}
static HRESULT WINAPI vt_LockBox(w8 *w, UINT l, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD f) { return IDirect3DVolumeTexture9_LockBox((IDirect3DVolumeTexture9 *)w->r, l, lb, b, f); }
static HRESULT WINAPI vt_UnlockBox(w8 *w, UINT l) { return IDirect3DVolumeTexture9_UnlockBox((IDirect3DVolumeTexture9 *)w->r, l); }
static HRESULT WINAPI vt_AddDirtyBox(w8 *w, const D3DBOX *b) { return IDirect3DVolumeTexture9_AddDirtyBox((IDirect3DVolumeTexture9 *)w->r, b); }

// ---- surfaces and volumes ------------------------------------------------------------------------------
static HRESULT WINAPI s_GetContainer(w8 *w, REFIID iid, void **out)
{
    (void)iid;
    IDirect3DBaseTexture9 *t = NULL;
    if (SUCCEEDED(IDirect3DSurface9_GetContainer((IDirect3DSurface9 *)w->r, &IID_IDirect3DBaseTexture9, (void **)&t)) && t) {
        *out = wrap(w->dev, t, tex_kind(t));
        return S_OK;
    }
    return w_GetDevice(w, out);
}
static HRESULT WINAPI s_GetDesc(w8 *w, SURFDESC8 *d)
{
    D3DSURFACE_DESC s;
    HRESULT hr = IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)w->r, &s);
    if (SUCCEEDED(hr) && w->scale > 1) { s.Width /= w->scale; s.Height /= w->scale; }
    if (SUCCEEDED(hr)) surfdesc8(&s, d);
    return hr;
}
static HRESULT WINAPI s_LockRect(w8 *w, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) { LOCK_RETRY(IDirect3DSurface9_LockRect((IDirect3DSurface9 *)w->r, lr, rc, f)); }
static HRESULT WINAPI s_UnlockRect(w8 *w) { return IDirect3DSurface9_UnlockRect((IDirect3DSurface9 *)w->r); }

static HRESULT WINAPI v_SetPrivateData(w8 *w, REFGUID g, const void *d, DWORD n, DWORD f) { return IDirect3DVolume9_SetPrivateData((IDirect3DVolume9 *)w->r, g, d, n, f); }
static HRESULT WINAPI v_GetPrivateData(w8 *w, REFGUID g, void *d, DWORD *n) { return IDirect3DVolume9_GetPrivateData((IDirect3DVolume9 *)w->r, g, d, n); }
static HRESULT WINAPI v_FreePrivateData(w8 *w, REFGUID g) { return IDirect3DVolume9_FreePrivateData((IDirect3DVolume9 *)w->r, g); }
static HRESULT WINAPI v_GetContainer(w8 *w, REFIID iid, void **out)
{
    (void)iid;
    IDirect3DBaseTexture9 *t = NULL;
    if (SUCCEEDED(IDirect3DVolume9_GetContainer((IDirect3DVolume9 *)w->r, &IID_IDirect3DBaseTexture9, (void **)&t)) && t) {
        *out = wrap(w->dev, t, K_VOLTEX);
        return S_OK;
    }
    return E_NOINTERFACE;
}
static HRESULT WINAPI v_GetDesc(w8 *w, VOLDESC8 *d)
{
    D3DVOLUME_DESC s;
    HRESULT hr = IDirect3DVolume9_GetDesc((IDirect3DVolume9 *)w->r, &s);
    if (SUCCEEDED(hr)) voldesc8(&s, d);
    return hr;
}
static HRESULT WINAPI v_LockBox(w8 *w, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD f) { return IDirect3DVolume9_LockBox((IDirect3DVolume9 *)w->r, lb, b, f); }
static HRESULT WINAPI v_UnlockBox(w8 *w) { return IDirect3DVolume9_UnlockBox((IDirect3DVolume9 *)w->r); }

// ---- buffers -------------------------------------------------------------------------------------------
static HRESULT mirror_lock(w8 *w, UINT o, UINT n, BYTE **p, DWORD f)
{
    if (!n || o + n > w->size) n = w->size - o;
    w->lock_off = o; w->lock_len = n; w->lock_flags = f;
    *p = w->mirror + o;
    return S_OK;
}
static HRESULT mirror_unlock(w8 *w)
{
    void *q = NULL;
    DWORD f = w->lock_flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    HRESULT hr = w->kind == K_VB ? IDirect3DVertexBuffer9_Lock((IDirect3DVertexBuffer9 *)w->r, w->lock_off, w->lock_len, &q, f)
                                 : IDirect3DIndexBuffer9_Lock((IDirect3DIndexBuffer9 *)w->r, w->lock_off, w->lock_len, &q, f);
    if (FAILED(hr)) return hr;
    memcpy(q, w->mirror + w->lock_off, w->lock_len);
    return w->kind == K_VB ? IDirect3DVertexBuffer9_Unlock((IDirect3DVertexBuffer9 *)w->r) : IDirect3DIndexBuffer9_Unlock((IDirect3DIndexBuffer9 *)w->r);
}
static HRESULT WINAPI vb_Lock(w8 *w, UINT o, UINT n, BYTE **p, DWORD f) { if (w->mirror) return mirror_lock(w, o, n, p, f); LOCK_RETRY(IDirect3DVertexBuffer9_Lock((IDirect3DVertexBuffer9 *)w->r, o, n, (void **)p, f)); }
static HRESULT WINAPI vb_Unlock(w8 *w) { if (w->mirror) return mirror_unlock(w); return IDirect3DVertexBuffer9_Unlock((IDirect3DVertexBuffer9 *)w->r); }
static HRESULT WINAPI vb_GetDesc(w8 *w, D3DVERTEXBUFFER_DESC *d) { return IDirect3DVertexBuffer9_GetDesc((IDirect3DVertexBuffer9 *)w->r, d); }
static HRESULT WINAPI ib_Lock(w8 *w, UINT o, UINT n, BYTE **p, DWORD f) { if (w->mirror) return mirror_lock(w, o, n, p, f); LOCK_RETRY(IDirect3DIndexBuffer9_Lock((IDirect3DIndexBuffer9 *)w->r, o, n, (void **)p, f)); }
static HRESULT WINAPI ib_Unlock(w8 *w) { if (w->mirror) return mirror_unlock(w); return IDirect3DIndexBuffer9_Unlock((IDirect3DIndexBuffer9 *)w->r); }
static HRESULT WINAPI ib_GetDesc(w8 *w, D3DINDEXBUFFER_DESC *d) { return IDirect3DIndexBuffer9_GetDesc((IDirect3DIndexBuffer9 *)w->r, d); }

// ---- swap chains -----------------------------------------------------------------------------------------
static HRESULT WINAPI sc_Present(w8 *w, const RECT *a, const RECT *b, HWND h, const RGNDATA *r) { return IDirect3DSwapChain9_Present((IDirect3DSwapChain9 *)w->r, a, b, h, r, 0); }
static HRESULT WINAPI sc_GetBackBuffer(w8 *w, UINT i, D3DBACKBUFFER_TYPE t, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DSwapChain9_GetBackBuffer((IDirect3DSwapChain9 *)w->r, i, t, &s);
    *out = SUCCEEDED(hr) ? wrap(w->dev, s, K_SURF) : NULL;
    return hr;
}

// ---- presentation parameters -------------------------------------------------------------------------------
static void pp9(const PP8 *p, D3DPRESENT_PARAMETERS *q)
{
    memset(q, 0, sizeof *q);
    q->BackBufferWidth = p->BackBufferWidth;
    q->BackBufferHeight = p->BackBufferHeight;
    q->BackBufferFormat = p->BackBufferFormat;
    q->BackBufferCount = p->BackBufferCount;
    q->MultiSampleType = p->MultiSampleType;
    q->SwapEffect = p->SwapEffect == 4 /* COPY_VSYNC */ ? D3DSWAPEFFECT_COPY : p->SwapEffect;
    q->hDeviceWindow = p->hDeviceWindow;
    q->Windowed = p->Windowed;
    q->EnableAutoDepthStencil = p->EnableAutoDepthStencil;
    q->AutoDepthStencilFormat = p->AutoDepthStencilFormat;
    q->Flags = p->Flags;
    q->FullScreen_RefreshRateInHz = p->FullScreen_RefreshRateInHz;
    if (p->Windowed) q->PresentationInterval = p->SwapEffect == 4 ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    else q->PresentationInterval = p->FullScreen_PresentationInterval;
}

// ---- vertex shaders: Direct3D 8 declarations -> Direct3D 9 ---------------------------------------------
static const BYTE k_usage[17][2] = {      // register -> usage, index (the D3DVSDE_* registers)
    {D3DDECLUSAGE_POSITION, 0}, {D3DDECLUSAGE_BLENDWEIGHT, 0}, {D3DDECLUSAGE_BLENDINDICES, 0}, {D3DDECLUSAGE_NORMAL, 0},
    {D3DDECLUSAGE_PSIZE, 0}, {D3DDECLUSAGE_COLOR, 0}, {D3DDECLUSAGE_COLOR, 1}, {D3DDECLUSAGE_TEXCOORD, 0},
    {D3DDECLUSAGE_TEXCOORD, 1}, {D3DDECLUSAGE_TEXCOORD, 2}, {D3DDECLUSAGE_TEXCOORD, 3}, {D3DDECLUSAGE_TEXCOORD, 4},
    {D3DDECLUSAGE_TEXCOORD, 5}, {D3DDECLUSAGE_TEXCOORD, 6}, {D3DDECLUSAGE_TEXCOORD, 7}, {D3DDECLUSAGE_POSITION, 1},
    {D3DDECLUSAGE_NORMAL, 1}};
static const BYTE k_typesize[8] = {4, 8, 12, 16, 4, 4, 4, 8};

static UINT decl_len(const DWORD *t)
{
    UINT n = 0;
    while (t[n] != 0xffffffff) {
        if (((t[n] >> 29) & 7) == 4) n += ((t[n] >> 25) & 0xf) * 4;   // constants follow the token
        n++;
    }
    return n + 1;
}

// Direct3D 8 read the w component of a scalar instruction's source when no
// swizzle was given; Direct3D 9 insists on a replicate swizzle.
static void fix_scalar_swizzles(DWORD *c)
{
    static const BYTE nparams[32] = {0, 2, 3, 3, 4, 3, 2, 2, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3, 4, 2, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0};
    while (*c != 0x0000ffff) {
        DWORD op = *c & 0xffff;
        if (op == 0xfffe) { c += 1 + ((*c >> 16) & 0x7fff); continue; }          // comment
        int n = op < 32 ? nparams[op] : (op == 78 || op == 79) ? 2 : -1;
        if (n < 0) return;                                                     // unknown: leave the rest alone
        int scalar = op == 6 || op == 7 || op == 14 || op == 15 || op == 78 || op == 79;   // rcp rsq exp log expp logp
        if (scalar && (c[2] & 0x00ff0000) == 0x00e40000) c[2] = (c[2] & 0xff00ffff) | 0x00ff0000;
        // m4x3 / m3x3 write .xyz and m3x2 .xy (Direct3D 8 accepted any write mask)
        if (op == 21 || op == 23) c[1] = (c[1] & ~0x000f0000u) | 0x00070000;
        if (op == 24) c[1] = (c[1] & ~0x000f0000u) | 0x00030000;
        // oFog / oPts (rasterizer outputs 1, 2) are written whole in Direct3D 9
        if (n >= 2 && op != 0 && ((c[1] >> 28) & 7) == D3DSPR_RASTOUT && (c[1] & 0x7ff) != 0) c[1] |= 0x000f0000;
        c += 1 + n;
    }
}
static HRESULT make_vshader(dev8 *d, const DWORD *decl, const DWORD *func, vshader *v)
{
    D3DVERTEXELEMENT9 el[64];
    int ne = 0, regs[32], nregs = 0;
    WORD stream = 0, offset = 0;
    for (UINT i = 0; decl[i] != 0xffffffff && ne < 63; i++) {
        DWORD t = decl[i];
        switch ((t >> 29) & 7) {
        case 1:                                          // D3DVSD_STREAM
            stream = (WORD)(t & 0xf);
            offset = 0;
            break;
        case 2:                                          // D3DVSD_REG / D3DVSD_SKIP
            if (t & 0x10000000) { offset += (WORD)(((t >> 16) & 0xf) * 4); break; }
            {
                int type = (t >> 16) & 0xf, reg = t & 0x1f;
                if (type > 7 || reg > 16) break;
                el[ne].Stream = stream;
                el[ne].Offset = offset;
                el[ne].Type = (BYTE)type;
                el[ne].Method = D3DDECLMETHOD_DEFAULT;
                el[ne].Usage = k_usage[reg][0];
                el[ne].UsageIndex = k_usage[reg][1];
                ne++;
                offset += k_typesize[type];
                if (nregs < 32) regs[nregs++] = reg;
            }
            break;
        case 4:                                          // D3DVSD_CONST: constants loaded with the shader
            {
                int count = (t >> 25) & 0xf, addr = t & 0x7f;
                if (v->nconst < 16 && addr + count <= 96) {
                    v->cfirst[v->nconst] = addr;
                    v->ccount[v->nconst] = count;
                    memcpy(v->consts + addr * 4, decl + i + 1, count * 16);
                    v->nconst++;
                }
                i += count * 4;
            }
            break;
        }
    }
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    el[ne] = end;
    HRESULT hr = IDirect3DDevice9_CreateVertexDeclaration(d->r, el, &v->decl);
    if (FAILED(hr)) return hr;
    if (!func) return S_OK;
    // vs 1.x token stream + one dcl per input register after the version token
    UINT n = 0;
    while (func[n] != 0x0000ffff) n++;
    DWORD *code = malloc((n + 1 + nregs * 3) * 4);
    UINT k = 0;
    code[k++] = func[0] == 0xfffe0100 ? 0xfffe0101 : func[0];
    for (int i = 0; i < nregs; i++) {
        code[k++] = D3DSIO_DCL;                         // shader model 1.x: no instruction length
        code[k++] = 0x80000000 | k_usage[regs[i]][0] | ((DWORD)k_usage[regs[i]][1] << 16);
        code[k++] = 0x900f0000 | regs[i];               // v<reg>, all components
    }
    memcpy(code + k, func + 1, (n - 1) * 4);
    k += n - 1;
    code[k++] = 0x0000ffff;
    fix_scalar_swizzles(code + 1 + nregs * 3);
    hr = IDirect3DDevice9_CreateVertexShader(d->r, code, &v->vs);
    if (FAILED(hr)) {
        // find the first instruction the runtime refuses: shorter and shorter versions
        static const BYTE np[32] = {0, 2, 3, 3, 4, 3, 2, 2, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3, 4, 2, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0};
        DWORD *tmp = malloc(k * 4);
        UINT p = 1;
        while (p < k && code[p] != 0x0000ffff) {
            DWORD op = code[p] & 0xffff;
            UINT len = op == 0xfffe ? 1 + ((code[p] >> 16) & 0x7fff) : op == D3DSIO_DCL ? 3 : 1 + (op < 32 ? np[op] : 2);
            memcpy(tmp, code, k * 4);
            for (UINT j = 0; j < len; j++) tmp[p + j] = 0;          // this instruction -> nops
            IDirect3DVertexShader9 *t9 = NULL;
            if (op != 0xfffe && op != D3DSIO_DCL && SUCCEEDED(IDirect3DDevice9_CreateVertexShader(d->r, tmp, &t9))) {
                mlog("d3d9: refused instruction at %u: %08lx %08lx %08lx %08lx", p, code[p], code[p + 1], code[p + 2], code[p + 3]);
                IDirect3DVertexShader9_Release(t9);
            }
            p += len;
        }
        free(tmp);
        mlog("d3d9: vertex shader rejected (%08lx), %u tokens:", hr, k);
        for (UINT i = 0; i < k; i += 8)
            mlog("  %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", code[i], i + 1 < k ? code[i + 1] : 0, i + 2 < k ? code[i + 2] : 0, i + 3 < k ? code[i + 3] : 0,
                 i + 4 < k ? code[i + 4] : 0, i + 5 < k ? code[i + 5] : 0, i + 6 < k ? code[i + 6] : 0, i + 7 < k ? code[i + 7] : 0);
    }
    free(code);
    return hr;
}

// failures of device calls (first ones of each kind) for diagnosis
static HRESULT chk(const char *what, HRESULT hr, DWORD a, DWORD b)
{
    static int n;
    if (FAILED(hr) && n < 60) { n++; mlog("d3d9: %s(%lx, %lx) failed %08lx", what, a, b, hr); }
    return hr;
}

// [d3d9frame] log the draws of one frame (diagnosis)
volatile int g_d9_logframe;
static void logdraw(dev8 *d, const char *what, DWORD type, DWORD count)
{
    if (!g_d9_logframe) return;
    DWORD z = 0, zw = 0, ab = 0, cull = 0, zf = 0, at = 0, light = 0, fvf = 0;
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_ZENABLE, &z);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_ZWRITEENABLE, &zw);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_ALPHABLENDENABLE, &ab);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_CULLMODE, &cull);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_ZFUNC, &zf);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_ALPHATESTENABLE, &at);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_LIGHTING, &light);
    IDirect3DDevice9_GetFVF(d->r, &fvf);
    IDirect3DBaseTexture9 *t0 = NULL;
    IDirect3DDevice9_GetTexture(d->r, 0, &t0);
    if (t0) IDirect3DBaseTexture9_Release(t0);
    IDirect3DSurface9 *rt = NULL;
    IDirect3DDevice9_GetRenderTarget(d->r, 0, &rt);
    if (rt) IDirect3DSurface9_Release(rt);
    D3DVIEWPORT9 vp;
    IDirect3DDevice9_GetViewport(d->r, &vp);
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DDevice9_GetPixelShader(d->r, &ps);
    if (ps) IDirect3DPixelShader9_Release(ps);
    DWORD sb = 0, db = 0, c0 = 0, c1 = 0, a0 = 0, tc1 = 0;
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_SRCBLEND, &sb);
    IDirect3DDevice9_GetRenderState(d->r, D3DRS_DESTBLEND, &db);
    IDirect3DDevice9_GetTextureStageState(d->r, 0, D3DTSS_COLOROP, &c0);
    IDirect3DDevice9_GetTextureStageState(d->r, 0, D3DTSS_ALPHAOP, &a0);
    IDirect3DDevice9_GetTextureStageState(d->r, 1, D3DTSS_COLOROP, &c1);
    IDirect3DDevice9_GetTextureStageState(d->r, 1, D3DTSS_TEXCOORDINDEX, &tc1);
    mlog("frame: %s t%lu n%lu vs %lx fvf %lx ps %p tex %p rt %p vp %lu,%lu %lux%lu z%lu/%lu/%lu ab%lu(%lu,%lu) at%lu cull%lu L%lu op %lu/%lu %lu(tc%lu) lit%d",
         what, type, count, d->cur_vs, fvf, ps, t0, rt, vp.X, vp.Y, vp.Width, vp.Height, z, zw, zf, ab, sb, db, at, cull, light, c0, a0, c1, tc1, d9light_count());
    if (light) {
        D3DMATERIAL9 m;
        DWORD dms = 0, ams = 0, ems = 0, amb = 0;
        IDirect3DDevice9_GetMaterial(d->r, &m);
        IDirect3DDevice9_GetRenderState(d->r, D3DRS_DIFFUSEMATERIALSOURCE, &dms);
        IDirect3DDevice9_GetRenderState(d->r, D3DRS_AMBIENTMATERIALSOURCE, &ams);
        IDirect3DDevice9_GetRenderState(d->r, D3DRS_EMISSIVEMATERIALSOURCE, &ems);
        IDirect3DDevice9_GetRenderState(d->r, D3DRS_AMBIENT, &amb);
        mlog("frame:   mat d %.2f %.2f %.2f %.2f a %.2f %.2f %.2f e %.2f %.2f %.2f src %lu/%lu/%lu ambient %08lx",
             m.Diffuse.r, m.Diffuse.g, m.Diffuse.b, m.Diffuse.a, m.Ambient.r, m.Ambient.g, m.Ambient.b, m.Emissive.r, m.Emissive.g, m.Emissive.b, dms, ams, ems, amb);
    }
    if (c1 > 1) {                     // multi-texture draws: the stage arguments
        DWORD s[2][6], dm = 0, tf = 0;
        for (int i = 0; i < 2; i++) {
            static const D3DTEXTURESTAGESTATETYPE k[6] = {D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_COLOROP};
            for (int j = 0; j < 6; j++) IDirect3DDevice9_GetTextureStageState(d->r, i, k[j], &s[i][j]);
        }
        IDirect3DDevice9_GetRenderState(d->r, D3DRS_DIFFUSEMATERIALSOURCE, &dm);
        IDirect3DDevice9_GetRenderState(d->r, D3DRS_TEXTUREFACTOR, &tf);
        IDirect3DBaseTexture9 *t1 = NULL;
        IDirect3DDevice9_GetTexture(d->r, 1, &t1);
        if (t1) IDirect3DBaseTexture9_Release(t1);
        mlog("frame:   s0 c%lx,%lx a%lu(%lx,%lx)  s1 c%lu(%lx,%lx) a%lu(%lx,%lx) tex1 %p dms %lu tf %08lx",
             s[0][0], s[0][1], s[0][2], s[0][3], s[0][4], s[1][5], s[1][0], s[1][1], s[1][2], s[1][3], s[1][4], t1, dm, tf);
    }
}
// ---- state blocks ----------------------------------------------------------------------------------------
// Direct3D 9 cannot Reset while state blocks exist and the games never release theirs (Direct3D 8
// did not mind), so state blocks are kept here: the recorded Set calls are replayed on Apply.
enum { SB_RS, SB_TSS, SB_TEX, SB_XFORM, SB_MAT, SB_LIGHT, SB_LIGHTON, SB_VS, SB_PS, SB_VSC, SB_PSC, SB_VP, SB_CLIP, SB_STREAM, SB_IDX };
typedef struct {
    BYTE type;
    DWORD a, b;
    void *p;                          // texture / buffer wrapper (referenced) or constants (malloc'ed)
    DWORD v[26];                      // matrix, light, material, viewport, plane
} sbop;
typedef struct sblock { int n, cap; sbop *ops; } sblock;

static void sb_release(sbop *o)
{
    if ((o->type == SB_TEX || o->type == SB_STREAM || o->type == SB_IDX) && o->p) w_Release(o->p);
    if ((o->type == SB_VSC || o->type == SB_PSC) && o->p) free(o->p);
    o->p = NULL;
}
static sbop *sb_slot(sblock *b, BYTE type, DWORD a)
{
    for (int i = 0; i < b->n; i++)                // the same state again: the last value counts
        if (b->ops[i].type == type && b->ops[i].a == a && type != SB_VSC && type != SB_PSC) { sb_release(&b->ops[i]); return &b->ops[i]; }
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->ops = realloc(b->ops, b->cap * sizeof(sbop)); }
    sbop *o = &b->ops[b->n++];
    memset(o, 0, sizeof *o);
    o->type = type;
    o->a = a;
    return o;
}
static void sb_ref(sbop *o, void *w) { o->p = w; if (w) w_AddRef(w); }
static void sb_free(sblock *b)
{
    if (!b) return;
    for (int i = 0; i < b->n; i++) sb_release(&b->ops[i]);
    free(b->ops);
    free(b);
}
// ---- IDirect3DDevice8 ----------------------------------------------------------------------------------------
#define D9 (d->r)
static HRESULT WINAPI d_QI(dev8 *d, REFIID iid, void **out) { (void)iid; *out = d; d->ref++; IDirect3DDevice9_AddRef(D9); return S_OK; }
static ULONG WINAPI d_AddRef(dev8 *d) { d->ref++; return IDirect3DDevice9_AddRef(D9); }
static ULONG WINAPI d_Release(dev8 *d)
{
    ULONG n = IDirect3DDevice9_Release(D9);
    if (--d->ref == 0) {
        for (int i = 0; i < MAX_VS; i++)
            if (d->vs[i]) { free(d->vs[i]->decl8); free(d->vs[i]->func8); free(d->vs[i]); }
        free(d);
    }
    return n;
}
// harness "devlost": the device reports itself lost (as after alt-tab in fullscreen) until the
// game resets it - tests the games' recovery in a window
volatile int g_d9_fake_lost;
volatile int g_d9_in_reset;          // display.c leaves the window alone meanwhile
static HRESULT WINAPI d_TestCooperativeLevel(dev8 *d)
{
    if (g_d9_fake_lost) return g_d9_fake_lost++ < 30 ? D3DERR_DEVICELOST : D3DERR_DEVICENOTRESET;
    return IDirect3DDevice9_TestCooperativeLevel(D9);
}
static UINT WINAPI d_GetAvailableTextureMem(dev8 *d) { return IDirect3DDevice9_GetAvailableTextureMem(D9); }
static HRESULT WINAPI d_ResourceManagerDiscardBytes(dev8 *d, DWORD n) { (void)n; return IDirect3DDevice9_EvictManagedResources(D9); }
static HRESULT WINAPI d_GetDirect3D(dev8 *d, void **out) { *out = d->parent; d->parent->ref++; IDirect3D9_AddRef(d->parent->r); return S_OK; }
static void caps8(D3DCAPS9 *c)
{
    if (c->VertexShaderVersion > D3DVS_VERSION(1, 1)) c->VertexShaderVersion = D3DVS_VERSION(1, 1);
    if (c->PixelShaderVersion > D3DPS_VERSION(1, 4)) c->PixelShaderVersion = D3DPS_VERSION(1, 4);
}
static HRESULT WINAPI d_GetDeviceCaps(dev8 *d, void *out)
{
    D3DCAPS9 c;
    HRESULT hr = IDirect3DDevice9_GetDeviceCaps(D9, &c);
    if (SUCCEEDED(hr)) { caps8(&c); memcpy(out, &c, CAPS8_SIZE); }
    return hr;
}
static HRESULT WINAPI d_GetDisplayMode(dev8 *d, D3DDISPLAYMODE *m) { return IDirect3DDevice9_GetDisplayMode(D9, 0, m); }
static HRESULT WINAPI d_GetCreationParameters(dev8 *d, D3DDEVICE_CREATION_PARAMETERS *p) { return IDirect3DDevice9_GetCreationParameters(D9, p); }
static HRESULT WINAPI d_SetCursorProperties(dev8 *d, UINT x, UINT y, void *s) { return IDirect3DDevice9_SetCursorProperties(D9, x, y, unwrap(s)); }
static void WINAPI d_SetCursorPosition(dev8 *d, int x, int y, DWORD f) { IDirect3DDevice9_SetCursorPosition(D9, x, y, f); }
static BOOL WINAPI d_ShowCursor(dev8 *d, BOOL s) { return IDirect3DDevice9_ShowCursor(D9, s); }
static HRESULT WINAPI d_CreateAdditionalSwapChain(dev8 *d, PP8 *p, void **out)
{
    D3DPRESENT_PARAMETERS q;
    pp9(p, &q);
    IDirect3DSwapChain9 *sc = NULL;
    HRESULT hr = IDirect3DDevice9_CreateAdditionalSwapChain(D9, &q, &sc);
    *out = SUCCEEDED(hr) ? wrap(d, sc, K_SWAP) : NULL;
    return hr;
}
static HRESULT WINAPI d_Reset(dev8 *d, PP8 *p)
{
    D3DPRESENT_PARAMETERS q;
    pp9(p, &q);
    if (d->cur_ds) { IDirect3DSurface9_Release(d->cur_ds); d->cur_ds = NULL; }
    if (d->big_ds) {
        IDirect3DSurface9 *cur = NULL;
        IDirect3DDevice9_GetDepthStencilSurface(D9, &cur);
        if (cur) IDirect3DSurface9_Release(cur);
        if (cur == d->big_ds) IDirect3DDevice9_SetDepthStencilSurface(D9, NULL);
        IDirect3DSurface9_Release(d->big_ds);
        d->big_ds = NULL;
        d->big_ds_for = NULL;
    }
    if (g_d9_fake_lost) { mlog("d3d9: (simulated device loss) reset after %d checks", g_d9_fake_lost); g_d9_fake_lost = 0; }
    // fullscreen while the window is in the background (alt-tab): wait (the game asks again); a
    // fullscreen Reset from the background can hang
    if (!q.Windowed) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        HWND w = q.hDeviceWindow;
        if (!w && SUCCEEDED(IDirect3DDevice9_GetCreationParameters(D9, &cp))) w = cp.hFocusWindow;
        if (w && (GetForegroundWindow() != w || IsIconic(w))) {
            static int n;
            if (n++ < 3) mlog("d3d9: fullscreen Reset deferred (window in the background)");
            return D3DERR_DEVICELOST;
        }
    }
    g_d9_in_reset = 1;
    HRESULT hr = IDirect3DDevice9_Reset(D9, &q);
    g_d9_in_reset = 0;
    d->base_vertex = 0;
    if (FAILED(hr)) mlog("d3d9: Reset failed %08lx", hr);
    return hr;
}
static HRESULT WINAPI d_Present(dev8 *d, const RECT *a, const RECT *b, HWND h, const RGNDATA *r)
{
    if (g_d9_logframe) { g_d9_logframe--; mlog("frame: present"); }
    if (g_d9_fake_lost) { if (g_d9_fake_lost < 30) g_d9_fake_lost++; return D3DERR_DEVICELOST; }
    return IDirect3DDevice9_Present(D9, a, b, h, r);
}
static HRESULT WINAPI d_GetBackBuffer(dev8 *d, UINT i, D3DBACKBUFFER_TYPE t, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_GetBackBuffer(D9, 0, i, t, &s);
    *out = SUCCEEDED(hr) ? wrap(d, s, K_SURF) : NULL;
    return hr;
}
static HRESULT WINAPI d_GetRasterStatus(dev8 *d, D3DRASTER_STATUS *s) { return IDirect3DDevice9_GetRasterStatus(D9, 0, s); }
static void WINAPI d_SetGammaRamp(dev8 *d, DWORD f, const D3DGAMMARAMP *r) { IDirect3DDevice9_SetGammaRamp(D9, 0, f, r); }
static void WINAPI d_GetGammaRamp(dev8 *d, D3DGAMMARAMP *r) { IDirect3DDevice9_GetGammaRamp(D9, 0, r); }
static HRESULT WINAPI d_CreateTexture(dev8 *d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, void **out)
{
    IDirect3DTexture9 *t = NULL;
    // the shadow pass renders into 256 / 512 square targets (silhouette, blur): enlarge them
    int k = (u & D3DUSAGE_RENDERTARGET) && l == 1 && w == h && (w == 256 || w == 512) && g_shadow_scale > 1 ? g_shadow_scale : 1;
    w *= k;
    h *= k;
    HRESULT hr = IDirect3DDevice9_CreateTexture(D9, w, h, l, u, f, p, &t, NULL);
    if (FAILED(hr) || (u & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) mlog("d3d9: CreateTexture %ux%u levels %u usage %lx fmt %u pool %u = %08lx (%p)", w, h, l, u, f, p, hr, t);
    *out = SUCCEEDED(hr) ? wrap(d, t, K_TEX) : NULL;
    if (*out) ((w8 *)*out)->scale = k;
    return hr;
}
static HRESULT WINAPI d_CreateVolumeTexture(dev8 *d, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, void **out)
{
    IDirect3DVolumeTexture9 *t = NULL;
    HRESULT hr = IDirect3DDevice9_CreateVolumeTexture(D9, w, h, dp, l, u, f, p, &t, NULL);
    *out = SUCCEEDED(hr) ? wrap(d, t, K_VOLTEX) : NULL;
    return hr;
}
static HRESULT WINAPI d_CreateCubeTexture(dev8 *d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, void **out)
{
    IDirect3DCubeTexture9 *t = NULL;
    HRESULT hr = IDirect3DDevice9_CreateCubeTexture(D9, e, l, u, f, p, &t, NULL);
    *out = SUCCEEDED(hr) ? wrap(d, t, K_CUBE) : NULL;
    return hr;
}
static HRESULT WINAPI d_CreateVertexBuffer(dev8 *d, UINT n, DWORD u, DWORD fvf, D3DPOOL p, void **out)
{
    IDirect3DVertexBuffer9 *b = NULL;
    HRESULT hr = IDirect3DDevice9_CreateVertexBuffer(D9, n, u, fvf, p, &b, NULL);
    w8 *wb = SUCCEEDED(hr) ? wrap(d, b, K_VB) : NULL;
    if (wb && (u & D3DUSAGE_DYNAMIC) && g_shadow_scale > 1) { wb->mirror = calloc(1, n); wb->size = n; }
    if (FAILED(hr)) mlog("d3d9: CreateVertexBuffer %u usage %lx pool %u failed %08lx", n, u, p, hr);
    *out = wb;
    return hr;
}
static HRESULT WINAPI d_CreateIndexBuffer(dev8 *d, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p, void **out)
{
    IDirect3DIndexBuffer9 *b = NULL;
    HRESULT hr = IDirect3DDevice9_CreateIndexBuffer(D9, n, u, f, p, &b, NULL);
    w8 *wb = SUCCEEDED(hr) ? wrap(d, b, K_IB) : NULL;
    if (wb && (u & D3DUSAGE_DYNAMIC) && g_shadow_scale > 1) { wb->mirror = calloc(1, n); wb->size = n; }
    if (FAILED(hr)) mlog("d3d9: CreateIndexBuffer %u usage %lx pool %u failed %08lx", n, u, p, hr);
    *out = wb;
    return hr;
}
static HRESULT WINAPI d_CreateRenderTarget(dev8 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, BOOL lock, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_CreateRenderTarget(D9, w, h, f, ms, 0, lock, &s, NULL);
    if (FAILED(hr)) mlog("d3d9: CreateRenderTarget %ux%u fmt %u failed %08lx", w, h, f, hr);
    *out = SUCCEEDED(hr) ? wrap(d, s, K_SURF) : NULL;
    return hr;
}
static HRESULT WINAPI d_CreateDepthStencilSurface(dev8 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_CreateDepthStencilSurface(D9, w, h, f, ms, 0, FALSE, &s, NULL);
    if (FAILED(hr)) mlog("d3d9: CreateDepthStencilSurface %ux%u fmt %u failed %08lx", w, h, f, hr);
    *out = SUCCEEDED(hr) ? wrap(d, s, K_SURF) : NULL;
    return hr;
}
static HRESULT WINAPI d_CreateImageSurface(dev8 *d, UINT w, UINT h, D3DFORMAT f, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(D9, w, h, f, D3DPOOL_SYSTEMMEM, &s, NULL);
    if (FAILED(hr)) {           // compressed formats: the surface of a one-level system memory texture
        IDirect3DTexture9 *t = NULL;
        hr = IDirect3DDevice9_CreateTexture(D9, w, h, 1, 0, f, D3DPOOL_SYSTEMMEM, &t, NULL);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DTexture9_GetSurfaceLevel(t, 0, &s);
            IDirect3DTexture9_Release(t);      // the surface keeps its texture alive
        }
        if (FAILED(hr)) mlog("d3d9: CreateImageSurface %ux%u fmt %u failed %08lx", w, h, f, hr);
    }
    *out = SUCCEEDED(hr) ? wrap(d, s, K_SURF) : NULL;
    return hr;
}

static int fmt_bytes(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16: return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: return 1;
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 0;   // blocks
    default: return 4;
    }
}

// one rectangle of CopyRects by locking both surfaces (system memory / managed destinations)
static HRESULT copy_locked(IDirect3DSurface9 *src, const RECT *r, IDirect3DSurface9 *dst, POINT p, D3DFORMAT f)
{
    D3DLOCKED_RECT a, b;
    RECT dr = {p.x, p.y, p.x + (r->right - r->left), p.y + (r->bottom - r->top)};
    int bpp = fmt_bytes(f), rows = r->bottom - r->top, w = r->right - r->left;
    if (!bpp) { bpp = (f == D3DFMT_DXT1) ? 8 : 16; rows = (rows + 3) / 4; w = (w + 3) / 4; }   // 4x4 blocks
    if (FAILED(IDirect3DSurface9_LockRect(src, &a, r, D3DLOCK_READONLY))) return D3DERR_INVALIDCALL;
    if (FAILED(IDirect3DSurface9_LockRect(dst, &b, &dr, 0))) { IDirect3DSurface9_UnlockRect(src); return D3DERR_INVALIDCALL; }
    for (int y = 0; y < rows; y++) memcpy((BYTE *)b.pBits + y * b.Pitch, (BYTE *)a.pBits + y * a.Pitch, w * bpp);
    IDirect3DSurface9_UnlockRect(dst);
    IDirect3DSurface9_UnlockRect(src);
    return S_OK;
}

static HRESULT WINAPI d_CopyRects(dev8 *d, void *s8, const RECT *rects, UINT n, void *t8, const POINT *pts)
{
    IDirect3DSurface9 *src = unwrap(s8), *dst = unwrap(t8);
    if (!src || !dst) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC sd, dd;
    IDirect3DSurface9_GetDesc(src, &sd);
    IDirect3DSurface9_GetDesc(dst, &dd);
    if (sd.Format != dd.Format) return D3DERR_INVALIDCALL;   // as in Direct3D 8: no conversions
    RECT full = {0, 0, (LONG)sd.Width, (LONG)sd.Height};
    POINT zero = {0, 0};
    if (!n) { rects = &full; pts = &zero; n = 1; }
    // a multisampled render target into system memory: resolve it first
    if (sd.Pool == D3DPOOL_DEFAULT && (sd.Usage & D3DUSAGE_RENDERTARGET) && dd.Pool == D3DPOOL_SYSTEMMEM &&
        sd.Width == dd.Width && sd.Height == dd.Height && sd.MultiSampleType != D3DMULTISAMPLE_NONE) {
        IDirect3DSurface9 *tmp = NULL;
        HRESULT hr = IDirect3DDevice9_CreateRenderTarget(D9, sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &tmp, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_StretchRect(D9, src, NULL, tmp, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(D9, tmp, dst);
        if (tmp) IDirect3DSurface9_Release(tmp);
        return hr;
    }
    // a whole render target into system memory
    if (sd.Pool == D3DPOOL_DEFAULT && (sd.Usage & D3DUSAGE_RENDERTARGET) && dd.Pool == D3DPOOL_SYSTEMMEM &&
        sd.Width == dd.Width && sd.Height == dd.Height && sd.MultiSampleType == D3DMULTISAMPLE_NONE)
        return IDirect3DDevice9_GetRenderTargetData(D9, src, dst);
    HRESULT hr = S_OK;
    for (UINT i = 0; i < n; i++) {
        POINT p = pts ? pts[i] : (POINT){rects[i].left, rects[i].top};
        RECT dr = {p.x, p.y, p.x + (rects[i].right - rects[i].left), p.y + (rects[i].bottom - rects[i].top)};
        if (sd.Pool == D3DPOOL_DEFAULT && dd.Pool == D3DPOOL_DEFAULT)
            hr = IDirect3DDevice9_StretchRect(D9, src, &rects[i], dst, &dr, D3DTEXF_NONE);
        else if (sd.Pool == D3DPOOL_SYSTEMMEM && dd.Pool == D3DPOOL_DEFAULT)
            hr = IDirect3DDevice9_UpdateSurface(D9, src, &rects[i], dst, &p);
        else
            hr = copy_locked(src, &rects[i], dst, p, sd.Format);
    }
    return hr;
}
static HRESULT WINAPI d_UpdateTexture(dev8 *d, void *s, void *t) { return IDirect3DDevice9_UpdateTexture(D9, unwrap(s), unwrap(t)); }
static HRESULT WINAPI d_GetFrontBuffer(dev8 *d, void *s) { return IDirect3DDevice9_GetFrontBufferData(D9, 0, unwrap(s)); }
// [d3d9frame] dump the enlarged targets as they are left (diagnosis)
static void dump_target(dev8 *d, IDirect3DSurface9 *s, int n)
{
    D3DSURFACE_DESC sd;
    IDirect3DSurface9_GetDesc(s, &sd);
    if (sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8) return;
    IDirect3DSurface9 *m = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d->r, sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &m, NULL))) return;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d->r, s, m)) && SUCCEEDED(IDirect3DSurface9_LockRect(m, &lr, NULL, D3DLOCK_READONLY))) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%srt_dump_%d.bmp", g_root, n);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh = {0x4d42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
            BITMAPINFOHEADER ih = {sizeof ih, (LONG)sd.Width, -(LONG)sd.Height, 1, 32, BI_RGB};
            fh.bfSize = fh.bfOffBits + sd.Width * sd.Height * 4;
            DWORD w;
            WriteFile(f, &fh, sizeof fh, &w, NULL);
            WriteFile(f, &ih, sizeof ih, &w, NULL);
            for (UINT y = 0; y < sd.Height; y++) WriteFile(f, (BYTE *)lr.pBits + y * lr.Pitch, sd.Width * 4, &w, NULL);
            CloseHandle(f);
            mlog("frame: dumped %s (%ux%u)", path, sd.Width, sd.Height);
        }
        IDirect3DSurface9_UnlockRect(m);
    }
    IDirect3DSurface9_Release(m);
}

static HRESULT WINAPI d_SetRenderTarget(dev8 *d, void *rt, void *ds)
{
    if (g_d9_logframe) {
        static int nd;
        IDirect3DSurface9 *cur = NULL;
        IDirect3DDevice9_GetRenderTarget(d->r, 0, &cur);
        if (cur) {
            D3DSURFACE_DESC cd;
            IDirect3DSurface9_GetDesc(cur, &cd);
            if (cd.Width != cd.Height ? 0 : nd < 6) dump_target(d, cur, nd++);
            IDirect3DSurface9_Release(cur);
        }
    }
    if (rt) d->rt_scale = ((w8 *)rt)->scale > 1 ? ((w8 *)rt)->scale : 1;
    if (g_d9_logframe) mlog("frame: SetRenderTarget %p ds %p", unwrap(rt), unwrap(ds));
    HRESULT hr = S_OK;
    if (rt) hr = IDirect3DDevice9_SetRenderTarget(D9, 0, unwrap(rt));
    IDirect3DSurface9 *z = unwrap(ds);
    if (z && rt && d->rt_scale > 1) {
        // an enlarged target (ShadowScale) with the game's depth buffer: Direct3D 9 draws nothing when the
        // depth buffer is smaller than the target (the base screens' 3D icons share the shadow targets)
        D3DSURFACE_DESC td, zd;
        IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)unwrap(rt), &td);
        IDirect3DSurface9_GetDesc(z, &zd);
        if (zd.Width < td.Width || zd.Height < td.Height) {
            D3DSURFACE_DESC bd;
            if (d->big_ds) IDirect3DSurface9_GetDesc(d->big_ds, &bd);
            if (d->big_ds && (bd.Width < td.Width || bd.Height < td.Height || bd.Format != zd.Format)) {
                IDirect3DSurface9_Release(d->big_ds);
                d->big_ds = NULL;
            }
            if (!d->big_ds) {
                HRESULT h2 = IDirect3DDevice9_CreateDepthStencilSurface(D9, td.Width, td.Height, zd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &d->big_ds, NULL);
                mlog("d3d9: depth buffer %ux%u for enlarged targets: %08lx", td.Width, td.Height, h2);
            }
            if (d->big_ds) { d->big_ds_for = z; z = d->big_ds; }
        }
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetDepthStencilSurface(D9, z);
    return hr;
}
static HRESULT WINAPI d_GetRenderTarget(dev8 *d, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_GetRenderTarget(D9, 0, &s);
    *out = SUCCEEDED(hr) ? wrap(d, s, K_SURF) : NULL;
    return hr;
}
static HRESULT WINAPI d_GetDepthStencilSurface(dev8 *d, void **out)
{
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_GetDepthStencilSurface(D9, &s);
    if (s && s == d->big_ds && d->big_ds_for) {     // the game sees its own depth buffer
        IDirect3DSurface9_Release(s);
        s = d->big_ds_for;
        IDirect3DSurface9_AddRef(s);
    }
    *out = SUCCEEDED(hr) && s ? wrap(d, s, K_SURF) : NULL;
    return SUCCEEDED(hr) && !s ? D3DERR_NOTFOUND : hr;
}
static HRESULT WINAPI d_BeginScene(dev8 *d) { return IDirect3DDevice9_BeginScene(D9); }
static HRESULT WINAPI d_EndScene(dev8 *d) { return IDirect3DDevice9_EndScene(D9); }
static HRESULT WINAPI d_Clear(dev8 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s)
{
    if (g_d9_logframe) mlog("frame: clear %lx color %08lx z %.2f", f, c, z);
    HRESULT hr = IDirect3DDevice9_Clear(D9, n, r, f, c, z, s);
    if (FAILED(hr) && (f & D3DCLEAR_STENCIL))            // Direct3D 8 ignored the stencil flag without a stencil buffer
        hr = IDirect3DDevice9_Clear(D9, n, r, f & ~D3DCLEAR_STENCIL, c, z, s);
    if (FAILED(hr) && (f & D3DCLEAR_ZBUFFER))            // ... and the depth flag without a depth buffer
        hr = IDirect3DDevice9_Clear(D9, n, r, f & D3DCLEAR_TARGET, c, z, s);
    return chk("Clear", hr, f, n);
}
static HRESULT WINAPI d_SetTransform(dev8 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { if (d->rec) { memcpy(sb_slot(d->rec, SB_XFORM, t)->v, m, 64); return S_OK; } return IDirect3DDevice9_SetTransform(D9, t, m); }
static HRESULT WINAPI d_GetTransform(dev8 *d, D3DTRANSFORMSTATETYPE t, D3DMATRIX *m) { return IDirect3DDevice9_GetTransform(D9, t, m); }
static HRESULT WINAPI d_MultiplyTransform(dev8 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { return IDirect3DDevice9_MultiplyTransform(D9, t, m); }
static HRESULT WINAPI d_SetViewport(dev8 *d, const D3DVIEWPORT9 *v)
{
    if (d->rec) { memcpy(sb_slot(d->rec, SB_VP, 0)->v, v, sizeof *v); return S_OK; }
    if (d->rt_scale > 1 && v) {                           // an enlarged target: the same area of it
        D3DVIEWPORT9 s = *v;
        s.X *= d->rt_scale; s.Y *= d->rt_scale; s.Width *= d->rt_scale; s.Height *= d->rt_scale;
        return IDirect3DDevice9_SetViewport(D9, &s);
    }
    return IDirect3DDevice9_SetViewport(D9, v);
}
static HRESULT WINAPI d_GetViewport(dev8 *d, D3DVIEWPORT9 *v) { return IDirect3DDevice9_GetViewport(D9, v); }
static HRESULT WINAPI d_SetMaterial(dev8 *d, const D3DMATERIAL9 *m) { if (d->rec) { memcpy(sb_slot(d->rec, SB_MAT, 0)->v, m, sizeof *m); return S_OK; } return IDirect3DDevice9_SetMaterial(D9, m); }
static HRESULT WINAPI d_GetMaterial(dev8 *d, D3DMATERIAL9 *m) { return IDirect3DDevice9_GetMaterial(D9, m); }
static HRESULT WINAPI d_SetLight(dev8 *d, DWORD i, const D3DLIGHT9 *l) { if (d->rec) { memcpy(sb_slot(d->rec, SB_LIGHT, i)->v, l, sizeof *l); return S_OK; } return IDirect3DDevice9_SetLight(D9, i, l); }
static HRESULT WINAPI d_GetLight(dev8 *d, DWORD i, D3DLIGHT9 *l) { return IDirect3DDevice9_GetLight(D9, i, l); }
static HRESULT WINAPI d_LightEnable(dev8 *d, DWORD i, BOOL e) { if (d->rec) { sb_slot(d->rec, SB_LIGHTON, i)->b = e; return S_OK; } return IDirect3DDevice9_LightEnable(D9, i, e); }
static HRESULT WINAPI d_GetLightEnable(dev8 *d, DWORD i, BOOL *e) { return IDirect3DDevice9_GetLightEnable(D9, i, e); }
static HRESULT WINAPI d_SetClipPlane(dev8 *d, DWORD i, const float *p) { if (d->rec) { memcpy(sb_slot(d->rec, SB_CLIP, i)->v, p, 16); return S_OK; } return IDirect3DDevice9_SetClipPlane(D9, i, p); }
static HRESULT WINAPI d_GetClipPlane(dev8 *d, DWORD i, float *p) { return IDirect3DDevice9_GetClipPlane(D9, i, p); }
static HRESULT WINAPI d_SetRenderState(dev8 *d, DWORD s, DWORD v)
{
    if (d->rec) { sb_slot(d->rec, SB_RS, s)->b = v; return S_OK; }
    switch (s) {
    case 10: case 30: case 40: case 164: return S_OK;     // LINEPATTERN, ZVISIBLE, EDGEANTIALIAS, PATCHSEGMENTS
    case 47: {                                            // ZBIAS -> DEPTHBIAS
        float b = (float)v * -0.000005f;
        return IDirect3DDevice9_SetRenderState(D9, D3DRS_DEPTHBIAS, *(DWORD *)&b);
    }
    case 153: return IDirect3DDevice9_SetSoftwareVertexProcessing(D9, v);
    }
    return chk("SetRenderState", IDirect3DDevice9_SetRenderState(D9, s, v), s, v);
}
static HRESULT WINAPI d_GetRenderState(dev8 *d, DWORD s, DWORD *v)
{
    switch (s) {
    case 10: case 30: case 40: case 164: *v = 0; return S_OK;
    case 47: {
        DWORD b;
        HRESULT hr = IDirect3DDevice9_GetRenderState(D9, D3DRS_DEPTHBIAS, &b);
        *v = (DWORD)(*(float *)&b / -0.000005f);
        return hr;
    }
    case 153: *v = IDirect3DDevice9_GetSoftwareVertexProcessing(D9); return S_OK;
    }
    return IDirect3DDevice9_GetRenderState(D9, s, v);
}
static HRESULT WINAPI d_SetClipStatus(dev8 *d, const D3DCLIPSTATUS9 *c) { return IDirect3DDevice9_SetClipStatus(D9, c); }
static HRESULT WINAPI d_GetClipStatus(dev8 *d, D3DCLIPSTATUS9 *c) { return IDirect3DDevice9_GetClipStatus(D9, c); }
static HRESULT WINAPI d_GetTexture(dev8 *d, DWORD st, void **out)
{
    IDirect3DBaseTexture9 *t = NULL;
    HRESULT hr = IDirect3DDevice9_GetTexture(D9, st, &t);
    *out = SUCCEEDED(hr) && t ? wrap(d, t, tex_kind(t)) : NULL;
    return hr;
}
static HRESULT WINAPI d_SetTexture(dev8 *d, DWORD st, void *t) { if (d->rec) { sb_ref(sb_slot(d->rec, SB_TEX, st), t); return S_OK; } return IDirect3DDevice9_SetTexture(D9, st, unwrap(t)); }
static int sampler_state(DWORD t)
{
    switch (t) {
    case 13: return D3DSAMP_ADDRESSU;
    case 14: return D3DSAMP_ADDRESSV;
    case 15: return D3DSAMP_BORDERCOLOR;
    case 16: return D3DSAMP_MAGFILTER;
    case 17: return D3DSAMP_MINFILTER;
    case 18: return D3DSAMP_MIPFILTER;
    case 19: return D3DSAMP_MIPMAPLODBIAS;
    case 20: return D3DSAMP_MAXMIPLEVEL;
    case 21: return D3DSAMP_MAXANISOTROPY;
    case 25: return D3DSAMP_ADDRESSW;
    }
    return 0;
}
static HRESULT WINAPI d_GetTextureStageState(dev8 *d, DWORD st, DWORD t, DWORD *v)
{
    int s = sampler_state(t);
    return s ? IDirect3DDevice9_GetSamplerState(D9, st, s, v) : IDirect3DDevice9_GetTextureStageState(D9, st, t, v);
}
static HRESULT WINAPI d_SetTextureStageState(dev8 *d, DWORD st, DWORD t, DWORD v)
{
    if (d->rec) { sb_slot(d->rec, SB_TSS, st << 8 | t)->b = v; return S_OK; }
    int s = sampler_state(t);
    return chk("SetTextureStageState", s ? IDirect3DDevice9_SetSamplerState(D9, st, s, v) : IDirect3DDevice9_SetTextureStageState(D9, st, t, v), t, v);
}
// Direct3D 9 refuses some stage setups the Direct3D 8 runtime accepted on the same hardware;
// the games only build their materials when validation passes
static HRESULT WINAPI d_ValidateDevice(dev8 *d, DWORD *n)
{
    HRESULT hr = IDirect3DDevice9_ValidateDevice(D9, n);
    if (FAILED(hr)) { if (n) *n = 1; hr = S_OK; }
    return hr;
}
static HRESULT WINAPI d_GetInfo(dev8 *d, DWORD id, void *p, DWORD n) { (void)d; (void)id; (void)p; (void)n; return S_FALSE; }
static HRESULT WINAPI d_SetPaletteEntries(dev8 *d, UINT p, const PALETTEENTRY *e) { return IDirect3DDevice9_SetPaletteEntries(D9, p, e); }
static HRESULT WINAPI d_GetPaletteEntries(dev8 *d, UINT p, PALETTEENTRY *e) { return IDirect3DDevice9_GetPaletteEntries(D9, p, e); }
static HRESULT WINAPI d_SetCurrentTexturePalette(dev8 *d, UINT p) { return IDirect3DDevice9_SetCurrentTexturePalette(D9, p); }
static HRESULT WINAPI d_GetCurrentTexturePalette(dev8 *d, UINT *p) { return IDirect3DDevice9_GetCurrentTexturePalette(D9, p); }
// Pre-transformed (XYZRHW) vertices drawn into an enlarged target: positions are pixels of
// the original size, so scale them (pixel centres stay on pixel centres).
static int rhw_scaled(dev8 *d)
{
    if (d->rt_scale <= 1) return 0;
    DWORD fvf = 0;
    IDirect3DDevice9_GetFVF(d->r, &fvf);
    return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && !(d->cur_vs & 0x80000000);
}
static BYTE *scaled_copy(dev8 *d, const BYTE *v, UINT n, UINT stride)
{
    BYTE *c = malloc((size_t)n * stride);
    memcpy(c, v, (size_t)n * stride);
    float k = (float)d->rt_scale;
    for (UINT i = 0; i < n; i++) {
        float *p = (float *)(c + i * stride);
        p[0] = (p[0] + 0.5f) * k - 0.5f;
        p[1] = (p[1] + 0.5f) * k - 0.5f;
    }
    return c;
}
static void restore_streams(dev8 *d)       // the UP draws unbind stream 0 and the indices
{
    IDirect3DDevice9_SetStreamSource(d->r, 0, d->vb0 ? (IDirect3DVertexBuffer9 *)d->vb0->r : NULL, 0, d->stride0);
    IDirect3DDevice9_SetIndices(d->r, d->ib ? (IDirect3DIndexBuffer9 *)d->ib->r : NULL);
}
static UINT prim_verts(D3DPRIMITIVETYPE t, UINT n)
{
    return t == D3DPT_POINTLIST ? n : t == D3DPT_LINELIST ? n * 2 : t == D3DPT_LINESTRIP ? n + 1 : t == D3DPT_TRIANGLELIST ? n * 3 : n + 2;
}
// dynamic lighting (d9light.c): lit world draws are repeated with the light pass
int d9light_begin(IDirect3DDevice9 *dev, int game_vs);
void d9light_end(IDirect3DDevice9 *dev);
int d9light_count(void);
#define LIT(d) (d9light_count() && !d->rec && d->rt_scale <= 1 && d9light_begin(d->r, (d->cur_vs & 0x80000000) != 0))

static HRESULT WINAPI d_DrawPrimitive_(dev8 *d, D3DPRIMITIVETYPE t, UINT s, UINT n);
static HRESULT WINAPI d_DrawPrimitive(dev8 *d, D3DPRIMITIVETYPE t, UINT s, UINT n)
{
    HRESULT hr = d_DrawPrimitive_(d, t, s, n);
    if (SUCCEEDED(hr) && LIT(d)) { IDirect3DDevice9_DrawPrimitive(D9, t, s, n); d9light_end(d->r); }
    return hr;
}
static HRESULT WINAPI d_DrawPrimitive_(dev8 *d, D3DPRIMITIVETYPE t, UINT s, UINT n)
{
    logdraw(d, "DP", t, n);
    if (rhw_scaled(d) && d->vb0 && d->vb0->mirror) {
        UINT nv = prim_verts(t, n);
        if ((s + nv) * d->stride0 <= d->vb0->size) {
            BYTE *c = scaled_copy(d, d->vb0->mirror + s * d->stride0, nv, d->stride0);
            HRESULT hr = IDirect3DDevice9_DrawPrimitiveUP(D9, t, n, c, d->stride0);
            free(c);
            restore_streams(d);
            return hr;
        }
    }
    return chk("DrawPrimitive", IDirect3DDevice9_DrawPrimitive(D9, t, s, n), t, n); }
static HRESULT WINAPI d_DrawIndexedPrimitive(dev8 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT si, UINT pc)
{
    logdraw(d, "DIP", t, pc);
    if (rhw_scaled(d) && d->vb0 && d->vb0->mirror && d->ib && d->ib->mirror) {
        D3DINDEXBUFFER_DESC id;
        IDirect3DIndexBuffer9_GetDesc((IDirect3DIndexBuffer9 *)d->ib->r, &id);
        UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2, ni = prim_verts(t, pc);
        UINT first = d->base_vertex + mi;
        if ((first + nv) * d->stride0 <= d->vb0->size && (si + ni) * isz <= d->ib->size) {
            BYTE *c = scaled_copy(d, d->vb0->mirror + first * d->stride0, nv, d->stride0);
            BYTE *ix = malloc((size_t)ni * isz);
            for (UINT i = 0; i < ni; i++) {             // indices relative to the copied range
                UINT v = isz == 4 ? ((DWORD *)d->ib->mirror)[si + i] : ((WORD *)d->ib->mirror)[si + i];
                v -= mi;
                if (isz == 4) ((DWORD *)ix)[i] = v; else ((WORD *)ix)[i] = (WORD)v;
            }
            HRESULT hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(D9, t, 0, nv, pc, ix, id.Format, c, d->stride0);
            free(c);
            free(ix);
            restore_streams(d);
            return hr;
        }
    }
    HRESULT hr = chk("DrawIndexedPrimitive", IDirect3DDevice9_DrawIndexedPrimitive(D9, t, d->base_vertex, mi, nv, si, pc), nv, pc);
    if (SUCCEEDED(hr) && LIT(d)) {
        IDirect3DDevice9_DrawIndexedPrimitive(D9, t, d->base_vertex, mi, nv, si, pc);
        d9light_end(d->r);
    }
    return hr;
}
static HRESULT WINAPI d_DrawPrimitiveUP(dev8 *d, D3DPRIMITIVETYPE t, UINT pc, const void *v, UINT st)
{
    logdraw(d, "DPUP", t, pc);
    if (rhw_scaled(d)) {
        BYTE *c = scaled_copy(d, v, prim_verts(t, pc), st);
        HRESULT hr = IDirect3DDevice9_DrawPrimitiveUP(D9, t, pc, c, st);
        free(c);
        restore_streams(d);
        return hr;
    }
    HRESULT hr0 = chk("DrawPrimitiveUP", IDirect3DDevice9_DrawPrimitiveUP(D9, t, pc, v, st), pc, st);
    if (SUCCEEDED(hr0) && LIT(d)) { IDirect3DDevice9_DrawPrimitiveUP(D9, t, pc, v, st); d9light_end(d->r); }
    restore_streams(d);
    return hr0;
}
static HRESULT WINAPI d_DrawIndexedPrimitiveUP(dev8 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *ix, D3DFORMAT f, const void *v, UINT st)
{
    logdraw(d, "DIPUP", t, pc);
    if (rhw_scaled(d)) {
        BYTE *c = scaled_copy(d, v, mi + nv, st);
        HRESULT hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(D9, t, mi, nv, pc, ix, f, c, st);
        free(c);
        restore_streams(d);
        return hr;
    }
    HRESULT hr0 = chk("DrawIndexedPrimitiveUP", IDirect3DDevice9_DrawIndexedPrimitiveUP(D9, t, mi, nv, pc, ix, f, v, st), nv, pc);
    if (SUCCEEDED(hr0) && LIT(d)) { IDirect3DDevice9_DrawIndexedPrimitiveUP(D9, t, mi, nv, pc, ix, f, v, st); d9light_end(d->r); }
    restore_streams(d);
    return hr0;
}
static HRESULT WINAPI d_ProcessVertices(dev8 *d, UINT s, UINT di, UINT n, void *b, DWORD f) { return IDirect3DDevice9_ProcessVertices(D9, s, di, n, unwrap(b), NULL, f); }

static vshader *vs_of(dev8 *d, DWORD h)
{
    return (h & 0x80000000) && (h & 0x7fffffff) < MAX_VS ? d->vs[h & 0x7fffffff] : NULL;
}
static HRESULT WINAPI d_CreateVertexShader(dev8 *d, const DWORD *decl, const DWORD *func, DWORD *h, DWORD usage)
{
    (void)usage;
    int slot = 1;
    while (slot < MAX_VS && d->vs[slot]) slot++;
    if (slot == MAX_VS || !decl) return E_OUTOFMEMORY;
    vshader *v = calloc(1, sizeof *v);
    HRESULT hr = make_vshader(d, decl, func, v);
    if (FAILED(hr)) {
        if (v->decl) IDirect3DVertexDeclaration9_Release(v->decl);
        free(v);
        return hr;
    }
    v->ndecl8 = decl_len(decl);
    v->decl8 = malloc(v->ndecl8 * 4);
    memcpy(v->decl8, decl, v->ndecl8 * 4);
    if (func) {
        UINT n = 0;
        while (func[n] != 0x0000ffff) n++;
        v->nfunc8 = n + 1;
        v->func8 = malloc(v->nfunc8 * 4);
        memcpy(v->func8, func, v->nfunc8 * 4);
    }
    d->vs[slot] = v;
    *h = 0x80000000 | slot;
    return S_OK;
}
static HRESULT WINAPI d_SetVertexShader(dev8 *d, DWORD h)
{
    if (d->rec) { sb_slot(d->rec, SB_VS, 0)->b = h; return S_OK; }
    d->cur_vs = h;
    if (!(h & 0x80000000)) {                              // an FVF: fixed function
        IDirect3DDevice9_SetVertexShader(D9, NULL);
        return chk("SetFVF", IDirect3DDevice9_SetFVF(D9, h), h, 0);
    }
    vshader *v = vs_of(d, h);
    if (!v) return D3DERR_INVALIDCALL;
    IDirect3DDevice9_SetVertexDeclaration(D9, v->decl);
    for (int i = 0; i < v->nconst; i++)
        IDirect3DDevice9_SetVertexShaderConstantF(D9, v->cfirst[i], v->consts + v->cfirst[i] * 4, v->ccount[i]);
    return IDirect3DDevice9_SetVertexShader(D9, v->vs);
}
static HRESULT WINAPI d_GetVertexShader(dev8 *d, DWORD *h) { *h = d->cur_vs; return S_OK; }
static HRESULT WINAPI d_DeleteVertexShader(dev8 *d, DWORD h)
{
    vshader *v = vs_of(d, h);
    if (!v) return D3DERR_INVALIDCALL;
    if (d->cur_vs == h) { IDirect3DDevice9_SetVertexShader(D9, NULL); d->cur_vs = 0; }
    if (v->vs) IDirect3DVertexShader9_Release(v->vs);
    if (v->decl) IDirect3DVertexDeclaration9_Release(v->decl);
    free(v->decl8);
    free(v->func8);
    free(v);
    d->vs[h & 0x7fffffff] = NULL;
    return S_OK;
}
static HRESULT WINAPI d_SetVertexShaderConstant(dev8 *d, DWORD r, const void *p, DWORD n) { if (d->rec) { sbop *o = sb_slot(d->rec, SB_VSC, r); o->b = n; o->p = malloc(n * 16); memcpy(o->p, p, n * 16); return S_OK; } return IDirect3DDevice9_SetVertexShaderConstantF(D9, r, p, n); }
static HRESULT WINAPI d_GetVertexShaderConstant(dev8 *d, DWORD r, void *p, DWORD n) { return IDirect3DDevice9_GetVertexShaderConstantF(D9, r, p, n); }
static HRESULT WINAPI d_GetVertexShaderDeclaration(dev8 *d, DWORD h, void *p, DWORD *n)
{
    vshader *v = vs_of(d, h);
    if (!v) return D3DERR_INVALIDCALL;
    if (!p) { *n = v->ndecl8 * 4; return S_OK; }
    if (*n < v->ndecl8 * 4) return D3DERR_MOREDATA;
    memcpy(p, v->decl8, v->ndecl8 * 4);
    return S_OK;
}
static HRESULT WINAPI d_GetVertexShaderFunction(dev8 *d, DWORD h, void *p, DWORD *n)
{
    vshader *v = vs_of(d, h);
    if (!v || !v->func8) return D3DERR_INVALIDCALL;
    if (!p) { *n = v->nfunc8 * 4; return S_OK; }
    if (*n < v->nfunc8 * 4) return D3DERR_MOREDATA;
    memcpy(p, v->func8, v->nfunc8 * 4);
    return S_OK;
}
static HRESULT WINAPI d_SetStreamSource(dev8 *d, UINT s, void *b, UINT st) { if (d->rec) { sbop *o = sb_slot(d->rec, SB_STREAM, s); sb_ref(o, b); o->b = st; return S_OK; } if (s == 0) { d->vb0 = b; d->stride0 = st; } return IDirect3DDevice9_SetStreamSource(D9, s, unwrap(b), 0, st); }
static HRESULT WINAPI d_GetStreamSource(dev8 *d, UINT s, void **out, UINT *st)
{
    IDirect3DVertexBuffer9 *b = NULL;
    UINT off;
    HRESULT hr = IDirect3DDevice9_GetStreamSource(D9, s, &b, &off, st);
    *out = SUCCEEDED(hr) && b ? wrap(d, b, K_VB) : NULL;
    return hr;
}
static HRESULT WINAPI d_SetIndices(dev8 *d, void *b, UINT base) { if (d->rec) { sbop *o = sb_slot(d->rec, SB_IDX, 0); sb_ref(o, b); o->b = base; return S_OK; } d->base_vertex = base; d->ib = b; return IDirect3DDevice9_SetIndices(D9, unwrap(b)); }
static HRESULT WINAPI d_GetIndices(dev8 *d, void **out, UINT *base)
{
    IDirect3DIndexBuffer9 *b = NULL;
    HRESULT hr = IDirect3DDevice9_GetIndices(D9, &b);
    *out = SUCCEEDED(hr) && b ? wrap(d, b, K_IB) : NULL;
    *base = d->base_vertex;
    return hr;
}
static HRESULT WINAPI d_CreatePixelShader(dev8 *d, const DWORD *f, DWORD *h)
{
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr;
    if (f && f[0] == 0xffff0100) {                        // ps 1.0 does not exist in Direct3D 9: same code as 1.1
        UINT n = 0;
        while (f[n] != 0x0000ffff) n++;
        DWORD *c = malloc((n + 1) * 4);
        memcpy(c, f, (n + 1) * 4);
        c[0] = 0xffff0101;
        hr = IDirect3DDevice9_CreatePixelShader(D9, c, &ps);
        free(c);
    } else hr = IDirect3DDevice9_CreatePixelShader(D9, f, &ps);
    if (FAILED(hr)) mlog("d3d9: pixel shader %08lx rejected (%08lx)", f ? f[0] : 0, hr);
    *h = (DWORD)(uintptr_t)ps;
    return hr;
}
static HRESULT WINAPI d_SetPixelShader(dev8 *d, DWORD h) { if (d->rec) { sb_slot(d->rec, SB_PS, 0)->b = h; return S_OK; } return IDirect3DDevice9_SetPixelShader(D9, (IDirect3DPixelShader9 *)(uintptr_t)h); }
static HRESULT WINAPI d_GetPixelShader(dev8 *d, DWORD *h)
{
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr = IDirect3DDevice9_GetPixelShader(D9, &ps);
    if (ps) IDirect3DPixelShader9_Release(ps);
    *h = (DWORD)(uintptr_t)ps;
    return hr;
}
static HRESULT WINAPI d_DeletePixelShader(dev8 *d, DWORD h)
{
    IDirect3DPixelShader9 *ps = (IDirect3DPixelShader9 *)(uintptr_t)h, *cur = NULL;
    if (!ps) return D3DERR_INVALIDCALL;
    IDirect3DDevice9_GetPixelShader(D9, &cur);
    if (cur) { if (cur == ps) IDirect3DDevice9_SetPixelShader(D9, NULL); IDirect3DPixelShader9_Release(cur); }
    IDirect3DPixelShader9_Release(ps);
    return S_OK;
}
static HRESULT WINAPI d_SetPixelShaderConstant(dev8 *d, DWORD r, const void *p, DWORD n) { if (d->rec) { sbop *o = sb_slot(d->rec, SB_PSC, r); o->b = n; o->p = malloc(n * 16); memcpy(o->p, p, n * 16); return S_OK; } return IDirect3DDevice9_SetPixelShaderConstantF(D9, r, p, n); }
static HRESULT WINAPI d_GetPixelShaderConstant(dev8 *d, DWORD r, void *p, DWORD n) { return IDirect3DDevice9_GetPixelShaderConstantF(D9, r, p, n); }
static HRESULT WINAPI d_GetPixelShaderFunction(dev8 *d, DWORD h, void *p, DWORD *n)
{
    (void)d;
    IDirect3DPixelShader9 *ps = (IDirect3DPixelShader9 *)(uintptr_t)h;
    UINT sz = *n;
    if (!ps) return D3DERR_INVALIDCALL;
    HRESULT hr = IDirect3DPixelShader9_GetFunction(ps, p, &sz);
    *n = sz;
    return hr;
}
static HRESULT WINAPI d_DrawRectPatch(dev8 *d, UINT h, const float *s, const D3DRECTPATCH_INFO *i) { return IDirect3DDevice9_DrawRectPatch(D9, h, s, i); }
static HRESULT WINAPI d_DrawTriPatch(dev8 *d, UINT h, const float *s, const D3DTRIPATCH_INFO *i) { return IDirect3DDevice9_DrawTriPatch(D9, h, s, i); }
static HRESULT WINAPI d_DeletePatch(dev8 *d, UINT h) { return IDirect3DDevice9_DeletePatch(D9, h); }

// ---- state blocks (see sblock above) --------------------------------------------------------------
static void sb_apply(dev8 *d, sblock *b)
{
    for (int i = 0; i < b->n; i++) {
        sbop *o = &b->ops[i];
        switch (o->type) {
        case SB_RS: d_SetRenderState(d, o->a, o->b); break;
        case SB_TSS: d_SetTextureStageState(d, o->a >> 8, o->a & 0xff, o->b); break;
        case SB_TEX: d_SetTexture(d, o->a, o->p); break;
        case SB_XFORM: d_SetTransform(d, o->a, (const D3DMATRIX *)o->v); break;
        case SB_MAT: d_SetMaterial(d, (const D3DMATERIAL9 *)o->v); break;
        case SB_LIGHT: d_SetLight(d, o->a, (const D3DLIGHT9 *)o->v); break;
        case SB_LIGHTON: d_LightEnable(d, o->a, o->b); break;
        case SB_VS: d_SetVertexShader(d, o->b); break;
        case SB_PS: d_SetPixelShader(d, o->b); break;
        case SB_VSC: d_SetVertexShaderConstant(d, o->a, o->p, o->b); break;
        case SB_PSC: d_SetPixelShaderConstant(d, o->a, o->p, o->b); break;
        case SB_VP: d_SetViewport(d, (const D3DVIEWPORT9 *)o->v); break;
        case SB_CLIP: d_SetClipPlane(d, o->a, (const float *)o->v); break;
        case SB_STREAM: d_SetStreamSource(d, o->a, o->p, o->b); break;
        case SB_IDX: d_SetIndices(d, o->p, o->b); break;
        }
    }
}
// the current value of every state the block holds
static void sb_capture(dev8 *d, sblock *b)
{
    for (int i = 0; i < b->n; i++) {
        sbop *o = &b->ops[i];
        void *w = NULL;
        switch (o->type) {
        case SB_RS: d_GetRenderState(d, o->a, &o->b); break;
        case SB_TSS: d_GetTextureStageState(d, o->a >> 8, o->a & 0xff, &o->b); break;
        case SB_TEX: sb_release(o); if (SUCCEEDED(d_GetTexture(d, o->a, &w))) o->p = w; break;   // GetTexture added the reference
        case SB_XFORM: IDirect3DDevice9_GetTransform(d->r, o->a, (D3DMATRIX *)o->v); break;
        case SB_MAT: IDirect3DDevice9_GetMaterial(d->r, (D3DMATERIAL9 *)o->v); break;
        case SB_LIGHT: IDirect3DDevice9_GetLight(d->r, o->a, (D3DLIGHT9 *)o->v); break;
        case SB_LIGHTON: { BOOL e = 0; IDirect3DDevice9_GetLightEnable(d->r, o->a, &e); o->b = e; break; }
        case SB_VS: o->b = d->cur_vs; break;
        case SB_PS: d_GetPixelShader(d, &o->b); break;
        case SB_VSC: IDirect3DDevice9_GetVertexShaderConstantF(d->r, o->a, o->p, o->b); break;
        case SB_PSC: IDirect3DDevice9_GetPixelShaderConstantF(d->r, o->a, o->p, o->b); break;
        case SB_VP: IDirect3DDevice9_GetViewport(d->r, (D3DVIEWPORT9 *)o->v); break;
        case SB_CLIP: IDirect3DDevice9_GetClipPlane(d->r, o->a, (float *)o->v); break;
        case SB_STREAM: { UINT st = 0; sb_release(o); if (SUCCEEDED(d_GetStreamSource(d, o->a, &w, &st))) { o->p = w; o->b = st; } break; }
        case SB_IDX: { UINT base = 0; sb_release(o); if (SUCCEEDED(d_GetIndices(d, &w, &base))) { o->p = w; o->b = base; } break; }
        }
    }
}
static HRESULT WINAPI d_BeginStateBlock(dev8 *d)
{
    if (d->rec) return D3DERR_INVALIDCALL;
    d->rec = calloc(1, sizeof(sblock));
    return S_OK;
}
static HRESULT WINAPI d_EndStateBlock(dev8 *d, DWORD *tok)
{
    if (!d->rec) return D3DERR_INVALIDCALL;
    *tok = (DWORD)(uintptr_t)d->rec;
    d->rec = NULL;
    return S_OK;
}
static HRESULT WINAPI d_ApplyStateBlock(dev8 *d, DWORD tok) { if (tok) sb_apply(d, (sblock *)(uintptr_t)tok); return S_OK; }
static HRESULT WINAPI d_CaptureStateBlock(dev8 *d, DWORD tok) { if (tok) sb_capture(d, (sblock *)(uintptr_t)tok); return S_OK; }
static HRESULT WINAPI d_DeleteStateBlock(dev8 *d, DWORD tok) { (void)d; sb_free((sblock *)(uintptr_t)tok); return S_OK; }
static HRESULT WINAPI d_CreateStateBlock(dev8 *d, D3DSTATEBLOCKTYPE t, DWORD *tok)
{
    // the states of the requested kind, with their current values
    static const BYTE pixel_rs[] = {7, 8, 9, 14, 15, 16, 19, 20, 23, 24, 25, 26, 27, 29, 34, 36, 37, 38, 47, 48, 52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 168, 171, 174};
    static const BYTE vertex_rs[] = {22, 28, 34, 35, 36, 37, 38, 48, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 165};
    sblock *b = calloc(1, sizeof *b);
    int pix = t == D3DSBT_ALL || t == D3DSBT_PIXELSTATE, ver = t == D3DSBT_ALL || t == D3DSBT_VERTEXSTATE;
    if (pix) for (size_t i = 0; i < sizeof pixel_rs; i++) sb_slot(b, SB_RS, pixel_rs[i]);
    if (ver) for (size_t i = 0; i < sizeof vertex_rs; i++) sb_slot(b, SB_RS, vertex_rs[i]);
    for (DWORD st = 0; st < 4; st++) {
        if (pix) for (DWORD s = 1; s <= 28; s++) if (s != 12) sb_slot(b, SB_TSS, st << 8 | s);
        if (t == D3DSBT_ALL) sb_slot(b, SB_TEX, st);
    }
    if (ver) {
        static const DWORD xf[] = {2, 3, 16, 17, 18, 19, 256};
        for (size_t i = 0; i < sizeof xf / sizeof xf[0]; i++) sb_slot(b, SB_XFORM, xf[i]);
        sb_slot(b, SB_MAT, 0);
        for (DWORD i = 0; i < 8; i++) { sb_slot(b, SB_LIGHT, i); sb_slot(b, SB_LIGHTON, i); }
        sb_slot(b, SB_VS, 0);
    }
    if (pix) sb_slot(b, SB_PS, 0);
    if (t == D3DSBT_ALL) { sb_slot(b, SB_VP, 0); sb_slot(b, SB_STREAM, 0); sb_slot(b, SB_IDX, 0); }
    sb_capture(d, b);
    *tok = (DWORD)(uintptr_t)b;
    return S_OK;
}
// ---- IDirect3D8 ------------------------------------------------------------------------------------------------
#define MODE_FORMATS 2
static const D3DFORMAT k_mode_fmt[MODE_FORMATS] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5};

static HRESULT WINAPI a_QI(d3d8 *a, REFIID iid, void **out) { (void)iid; *out = a; a->ref++; IDirect3D9_AddRef(a->r); return S_OK; }
static ULONG WINAPI a_AddRef(d3d8 *a) { a->ref++; return IDirect3D9_AddRef(a->r); }
static ULONG WINAPI a_Release(d3d8 *a)
{
    ULONG n = IDirect3D9_Release(a->r);
    if (--a->ref == 0) free(a);
    return n;
}
static HRESULT WINAPI a_RegisterSoftwareDevice(d3d8 *a, void *f) { return IDirect3D9_RegisterSoftwareDevice(a->r, f); }
static UINT WINAPI a_GetAdapterCount(d3d8 *a) { return IDirect3D9_GetAdapterCount(a->r); }
static HRESULT WINAPI a_GetAdapterIdentifier(d3d8 *a, UINT ad, DWORD f, ADAPTERID8 *id)
{
    D3DADAPTER_IDENTIFIER9 i;
    HRESULT hr = IDirect3D9_GetAdapterIdentifier(a->r, ad, f & ~2u /* D3DENUM_NO_WHQL_LEVEL */, &i);
    if (FAILED(hr)) return hr;
    memcpy(id->Driver, i.Driver, 512);
    memcpy(id->Description, i.Description, 512);
    id->DriverVersion = i.DriverVersion;
    id->VendorId = i.VendorId; id->DeviceId = i.DeviceId; id->SubSysId = i.SubSysId; id->Revision = i.Revision;
    id->DeviceIdentifier = i.DeviceIdentifier;
    id->WHQLLevel = i.WHQLLevel;
    return hr;
}
static UINT WINAPI a_GetAdapterModeCount(d3d8 *a, UINT ad)
{
    UINT n = 0;
    for (int i = 0; i < MODE_FORMATS; i++) n += IDirect3D9_GetAdapterModeCount(a->r, ad, k_mode_fmt[i]);
    return n;
}
static HRESULT WINAPI a_EnumAdapterModes(d3d8 *a, UINT ad, UINT mode, D3DDISPLAYMODE *m)
{
    for (int i = 0; i < MODE_FORMATS; i++) {
        UINT n = IDirect3D9_GetAdapterModeCount(a->r, ad, k_mode_fmt[i]);
        if (mode < n) return IDirect3D9_EnumAdapterModes(a->r, ad, k_mode_fmt[i], mode, m);
        mode -= n;
    }
    return D3DERR_INVALIDCALL;
}
static HRESULT WINAPI a_GetAdapterDisplayMode(d3d8 *a, UINT ad, D3DDISPLAYMODE *m) { return IDirect3D9_GetAdapterDisplayMode(a->r, ad, m); }
static HRESULT WINAPI a_CheckDeviceType(d3d8 *a, UINT ad, D3DDEVTYPE t, D3DFORMAT df, D3DFORMAT bf, BOOL w)
{
    HRESULT hr = IDirect3D9_CheckDeviceType(a->r, ad, t, df, bf, w);
    return hr;
}
static HRESULT WINAPI a_CheckDeviceFormat(d3d8 *a, UINT ad, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT f)
{
    HRESULT hr = IDirect3D9_CheckDeviceFormat(a->r, ad, t, af, u, rt, f);
    return hr;
}
static HRESULT WINAPI a_CheckDeviceMultiSampleType(d3d8 *a, UINT ad, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m) { return IDirect3D9_CheckDeviceMultiSampleType(a->r, ad, t, f, w, m, NULL); }
static HRESULT WINAPI a_CheckDepthStencilMatch(d3d8 *a, UINT ad, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) { return IDirect3D9_CheckDepthStencilMatch(a->r, ad, t, af, rf, df); }
static HRESULT WINAPI a_GetDeviceCaps(d3d8 *a, UINT ad, D3DDEVTYPE t, void *out)
{
    D3DCAPS9 c;
    HRESULT hr = IDirect3D9_GetDeviceCaps(a->r, ad, t, &c);
    if (SUCCEEDED(hr)) { caps8(&c); memcpy(out, &c, CAPS8_SIZE); }
    return hr;
}
static HMONITOR WINAPI a_GetAdapterMonitor(d3d8 *a, UINT ad) { return IDirect3D9_GetAdapterMonitor(a->r, ad); }
static HRESULT WINAPI a_CreateDevice(d3d8 *a, UINT ad, D3DDEVTYPE t, HWND w, DWORD f, PP8 *p, void **out)
{
    D3DPRESENT_PARAMETERS q;
    pp9(p, &q);
    IDirect3DDevice9 *r = NULL;
    f &= ~D3DCREATE_PUREDEVICE;      // Direct3D 9 pure devices cannot record state blocks the way Direct3D 8 ones did
    HRESULT hr = IDirect3D9_CreateDevice(a->r, ad, t, w, f, &q, &r);
    mlog("d3d9: CreateDevice %08lx (%ux%u, windowed %d, msaa %d)", hr, q.BackBufferWidth, q.BackBufferHeight, q.Windowed, q.MultiSampleType);
    if (FAILED(hr)) { *out = NULL; return hr; }
    dev8 *d = calloc(1, sizeof *d);
    d->vt = vt_dev8;
    d->ref = 1;
    d->r = r;
    d->parent = a;
    *out = d;
    return hr;
}

// ---- vtables -----------------------------------------------------------------------------------------------------
#define F(x) (void *)(x)
static void init_vtables(void)
{
    void *a[] = {F(a_QI), F(a_AddRef), F(a_Release), F(a_RegisterSoftwareDevice), F(a_GetAdapterCount), F(a_GetAdapterIdentifier),
                 F(a_GetAdapterModeCount), F(a_EnumAdapterModes), F(a_GetAdapterDisplayMode), F(a_CheckDeviceType),
                 F(a_CheckDeviceFormat), F(a_CheckDeviceMultiSampleType), F(a_CheckDepthStencilMatch), F(a_GetDeviceCaps),
                 F(a_GetAdapterMonitor), F(a_CreateDevice)};
    memcpy(vt_d3d8, a, sizeof a);
    void *dv[] = {F(d_QI), F(d_AddRef), F(d_Release), F(d_TestCooperativeLevel), F(d_GetAvailableTextureMem),
                  F(d_ResourceManagerDiscardBytes), F(d_GetDirect3D), F(d_GetDeviceCaps), F(d_GetDisplayMode),
                  F(d_GetCreationParameters), F(d_SetCursorProperties), F(d_SetCursorPosition), F(d_ShowCursor),
                  F(d_CreateAdditionalSwapChain), F(d_Reset), F(d_Present), F(d_GetBackBuffer), F(d_GetRasterStatus),
                  F(d_SetGammaRamp), F(d_GetGammaRamp), F(d_CreateTexture), F(d_CreateVolumeTexture), F(d_CreateCubeTexture),
                  F(d_CreateVertexBuffer), F(d_CreateIndexBuffer), F(d_CreateRenderTarget), F(d_CreateDepthStencilSurface),
                  F(d_CreateImageSurface), F(d_CopyRects), F(d_UpdateTexture), F(d_GetFrontBuffer), F(d_SetRenderTarget),
                  F(d_GetRenderTarget), F(d_GetDepthStencilSurface), F(d_BeginScene), F(d_EndScene), F(d_Clear),
                  F(d_SetTransform), F(d_GetTransform), F(d_MultiplyTransform), F(d_SetViewport), F(d_GetViewport),
                  F(d_SetMaterial), F(d_GetMaterial), F(d_SetLight), F(d_GetLight), F(d_LightEnable), F(d_GetLightEnable),
                  F(d_SetClipPlane), F(d_GetClipPlane), F(d_SetRenderState), F(d_GetRenderState), F(d_BeginStateBlock),
                  F(d_EndStateBlock), F(d_ApplyStateBlock), F(d_CaptureStateBlock), F(d_DeleteStateBlock),
                  F(d_CreateStateBlock), F(d_SetClipStatus), F(d_GetClipStatus), F(d_GetTexture), F(d_SetTexture),
                  F(d_GetTextureStageState), F(d_SetTextureStageState), F(d_ValidateDevice), F(d_GetInfo),
                  F(d_SetPaletteEntries), F(d_GetPaletteEntries), F(d_SetCurrentTexturePalette),
                  F(d_GetCurrentTexturePalette), F(d_DrawPrimitive), F(d_DrawIndexedPrimitive), F(d_DrawPrimitiveUP),
                  F(d_DrawIndexedPrimitiveUP), F(d_ProcessVertices), F(d_CreateVertexShader), F(d_SetVertexShader),
                  F(d_GetVertexShader), F(d_DeleteVertexShader), F(d_SetVertexShaderConstant), F(d_GetVertexShaderConstant),
                  F(d_GetVertexShaderDeclaration), F(d_GetVertexShaderFunction), F(d_SetStreamSource), F(d_GetStreamSource),
                  F(d_SetIndices), F(d_GetIndices), F(d_CreatePixelShader), F(d_SetPixelShader), F(d_GetPixelShader),
                  F(d_DeletePixelShader), F(d_SetPixelShaderConstant), F(d_GetPixelShaderConstant),
                  F(d_GetPixelShaderFunction), F(d_DrawRectPatch), F(d_DrawTriPatch), F(d_DeletePatch)};
    _Static_assert(sizeof dv / sizeof dv[0] == 97, "IDirect3DDevice8 has 97 methods");
    memcpy(vt_dev8, dv, sizeof dv);
    void *res[] = {F(w_QI), F(w_AddRef), F(w_Release), F(w_GetDevice), F(w_SetPrivateData), F(w_GetPrivateData),
                   F(w_FreePrivateData), F(w_SetPriority), F(w_GetPriority), F(w_PreLoad), F(w_GetType)};
    void *tx[] = {F(t_SetLOD), F(t_GetLOD), F(t_GetLevelCount), F(tx_GetLevelDesc), F(tx_GetSurfaceLevel), F(tx_LockRect),
                  F(tx_UnlockRect), F(tx_AddDirtyRect)};
    void *cb[] = {F(t_SetLOD), F(t_GetLOD), F(t_GetLevelCount), F(cb_GetLevelDesc), F(cb_GetCubeMapSurface), F(cb_LockRect),
                  F(cb_UnlockRect), F(cb_AddDirtyRect)};
    void *vx[] = {F(t_SetLOD), F(t_GetLOD), F(t_GetLevelCount), F(vt_GetLevelDesc), F(vt_GetVolumeLevel), F(vt_LockBox),
                  F(vt_UnlockBox), F(vt_AddDirtyBox)};
    memcpy(vt_tex, res, sizeof res);    memcpy(vt_tex + 11, tx, sizeof tx);
    memcpy(vt_cube, res, sizeof res);   memcpy(vt_cube + 11, cb, sizeof cb);
    memcpy(vt_voltex, res, sizeof res); memcpy(vt_voltex + 11, vx, sizeof vx);
    void *vb[] = {F(vb_Lock), F(vb_Unlock), F(vb_GetDesc)}, *ib[] = {F(ib_Lock), F(ib_Unlock), F(ib_GetDesc)};
    memcpy(vt_vb, res, sizeof res); memcpy(vt_vb + 11, vb, sizeof vb);
    memcpy(vt_ib, res, sizeof res); memcpy(vt_ib + 11, ib, sizeof ib);
    void *s[] = {F(w_QI), F(w_AddRef), F(w_Release), F(w_GetDevice), F(w_SetPrivateData), F(w_GetPrivateData),
                 F(w_FreePrivateData), F(s_GetContainer), F(s_GetDesc), F(s_LockRect), F(s_UnlockRect)};
    memcpy(vt_surf, s, sizeof s);
    void *v[] = {F(w_QI), F(w_AddRef), F(w_Release), F(w_GetDevice), F(v_SetPrivateData), F(v_GetPrivateData),
                 F(v_FreePrivateData), F(v_GetContainer), F(v_GetDesc), F(v_LockBox), F(v_UnlockBox)};
    memcpy(vt_vol, v, sizeof v);
    void *sc[] = {F(w_QI), F(w_AddRef), F(w_Release), F(sc_Present), F(sc_GetBackBuffer)};
    memcpy(vt_swap, sc, sizeof sc);
}

// Direct3DCreate8 replacement: NULL if Direct3D 9 is not available
void *d3d9wrap_create(void)
{
    typedef IDirect3D9 *(WINAPI *create9_fn)(UINT);
    HMODULE m = LoadLibraryA("d3d9.dll");
    create9_fn create9 = m ? (create9_fn)GetProcAddress(m, "Direct3DCreate9") : NULL;
    IDirect3D9 *r = create9 ? create9(D3D_SDK_VERSION) : NULL;
    if (!r) { mlog("d3d9: Direct3D 9 not available, using Direct3D 8"); return NULL; }
    static int inited;
    if (!inited) { init_vtables(); inited = 1; }
    extern char g_ini[MAX_PATH];
    g_shadow_scale = GetPrivateProfileIntA("Video", "ShadowScale", 1, g_ini);
    if (g_shadow_scale != 2 && g_shadow_scale != 4) g_shadow_scale = 1;
    d3d8 *a = calloc(1, sizeof *a);
    a->vt = vt_d3d8;
    a->ref = 1;
    a->r = r;
    mlog("d3d9: renderer is Direct3D 9");
    return a;
}

IDirect3DDevice9 *d3d9wrap_device9(void *dev8p) { return dev8p && ((dev8 *)dev8p)->vt == vt_dev8 ? ((dev8 *)dev8p)->r : NULL; }
