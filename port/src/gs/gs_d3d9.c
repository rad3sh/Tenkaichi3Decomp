/*
 * Direct3D 9 bootstrap backend.
 *
 * This is intentionally small: it owns the native D3D9 device and presents a cleared window while the
 * shared GS capture and the full texture/primitive translation are brought over incrementally. The backend
 * still implements the complete GsBackend contract so it can be selected without changing game code.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_internal.h"
#include "gs_draw.h"

extern SDL_Window *GsDraw_WindowCreate(int opengl);

#define D3D9_MAX_TEXTURES 4096

typedef struct D3d9Texture {
    IDirect3DTexture9 *texture;
    IDirect3DSurface9 *surface;
    uint32_t width;
    uint32_t height;
    int renderTarget;
    int format;
} D3d9Texture;

typedef struct D3d9Target {
    D3d9Texture *color;
    D3d9Texture *aux;
    IDirect3DSurface9 *depth;
} D3d9Target;

typedef struct D3d9Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
} D3d9Vertex;

#define D3D9_FVF_COLOR (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define D3D9_FVF_TEXTURED (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static SDL_Window *sWindow;
static IDirect3D9 *sD3d;
static IDirect3DDevice9 *sDevice;
static IDirect3DSurface9 *sBackbuffer;
static D3DPRESENT_PARAMETERS sPresent;
static D3d9Texture *sWhite;
static D3d9Target sTargets[MAX_TARGETS];
static int sTextureCount;
static int sDeviceLost;
static int sPipeCount;
static uint32_t sPipeKeys[1024];
static D3DFORMAT sAuxFormat = D3DFMT_A8R8G8B8;
static unsigned char sTargetSeen[MAX_TARGETS];
static int sCurrentTarget = -1;

static void d3d9_log_hr(const char *what, HRESULT hr) {
    fprintf(stderr, "bt3: d3d9: %s failed (0x%08lX)\n", what, (unsigned long)hr);
}

static int d3d9_texture_size(uint32_t *width, uint32_t *height) {
    uint32_t max = 0;
    D3DCAPS9 caps;
    if (sD3d == NULL || sDevice == NULL || sD3d->lpVtbl->GetDeviceCaps(sD3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps) != D3D_OK) {
        return 0;
    }
    max = caps.MaxTextureWidth < caps.MaxTextureHeight ? caps.MaxTextureWidth : caps.MaxTextureHeight;
    return *width > 0 && *height > 0 && *width <= max && *height <= max;
}

static void d3d9_texture_release(D3d9Texture *texture) {
    if (texture == NULL) {
        return;
    }
    if (texture->surface != NULL) {
        texture->surface->lpVtbl->Release(texture->surface);
    }
    if (texture->texture != NULL) {
        texture->texture->lpVtbl->Release(texture->texture);
    }
    free(texture);
}

static void d3d9_target_release(D3d9Target *target) {
    if (target->depth != NULL) {
        target->depth->lpVtbl->Release(target->depth);
        target->depth = NULL;
    }
    d3d9_texture_release(target->aux);
    target->aux = NULL;
    d3d9_texture_release(target->color);
    target->color = NULL;
}

static void d3d9_release_all(void) {
    int i;
    for (i = 0; i < MAX_TARGETS; i++) {
        d3d9_target_release(&sTargets[i]);
    }
    if (sWhite != NULL) {
        if (sWhite->surface != NULL) {
            sWhite->surface->lpVtbl->Release(sWhite->surface);
        }
        if (sWhite->texture != NULL) {
            sWhite->texture->lpVtbl->Release(sWhite->texture);
        }
        free(sWhite);
        sWhite = NULL;
    }
    if (sBackbuffer != NULL) {
        sBackbuffer->lpVtbl->Release(sBackbuffer);
        sBackbuffer = NULL;
    }
    if (sDevice != NULL) {
        sDevice->lpVtbl->Release(sDevice);
        sDevice = NULL;
    }
    if (sD3d != NULL) {
        sD3d->lpVtbl->Release(sD3d);
        sD3d = NULL;
    }
    sTextureCount = 0;
    memset(sTargetSeen, 0, sizeof(sTargetSeen));
    sCurrentTarget = -1;
}

static int d3d9_create_device(void) {
    D3DCAPS9 caps;
    D3DADAPTER_IDENTIFIER9 adapter;
    D3DDEVTYPE type = D3DDEVTYPE_HAL;
    DWORD behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    HRESULT hr;
    SDL_PropertiesID props;
    void *hwnd;

    props = SDL_GetWindowProperties(sWindow);
    hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd == NULL) {
        fprintf(stderr, "bt3: d3d9: SDL window has no Win32 HWND\n");
        return 0;
    }
    memset(&sPresent, 0, sizeof(sPresent));
    sPresent.Windowed = TRUE;
    sPresent.SwapEffect = D3DSWAPEFFECT_DISCARD;
    sPresent.BackBufferFormat = D3DFMT_X8R8G8B8;
    sPresent.BackBufferWidth = 0;
    sPresent.BackBufferHeight = 0;
    sPresent.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    sD3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (sD3d == NULL) {
        fprintf(stderr, "bt3: d3d9: Direct3DCreate9 failed\n");
        return 0;
    }
    if (sD3d->lpVtbl->GetAdapterIdentifier(sD3d, D3DADAPTER_DEFAULT, 0, &adapter) == D3D_OK) {
        fprintf(stderr, "bt3: D3D9 adapter: %s, driver %s\n", adapter.Description, adapter.Driver);
    }
    if (sD3d->lpVtbl->GetDeviceCaps(sD3d, D3DADAPTER_DEFAULT, type, &caps) == D3D_OK) {
        fprintf(stderr, "bt3: D3D9 caps: shader vs_%d_%d ps_%d_%d max_texture %lu\n",
                D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
                D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
                (unsigned long)(caps.MaxTextureWidth < caps.MaxTextureHeight ? caps.MaxTextureWidth : caps.MaxTextureHeight));
    }
    hr = sD3d->lpVtbl->CreateDevice(sD3d, D3DADAPTER_DEFAULT, type, (HWND)hwnd, behavior, &sPresent, &sDevice);
    if (FAILED(hr)) {
        behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        hr = sD3d->lpVtbl->CreateDevice(sD3d, D3DADAPTER_DEFAULT, type, (HWND)hwnd, behavior, &sPresent, &sDevice);
    }
    if (FAILED(hr)) {
        d3d9_log_hr("CreateDevice", hr);
        return 0;
    }
    hr = sDevice->lpVtbl->GetRenderTarget(sDevice, 0, &sBackbuffer);
    if (FAILED(hr)) {
        d3d9_log_hr("GetRenderTarget", hr);
        return 0;
    }
    if (sD3d->lpVtbl->CheckDeviceFormat(sD3d, D3DADAPTER_DEFAULT, type, sPresent.BackBufferFormat,
                                        D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8) == D3D_OK) {
        sAuxFormat = D3DFMT_A8;
    } else {
        fprintf(stderr, "bt3: D3D9 A8 render target unavailable; using A8R8G8B8 auxiliary targets\n");
    }
    fprintf(stderr, "bt3: D3D9 device ready (%s vertex processing)\n",
            behavior == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software");
    return 1;
}

static void d3d9_draw_gs(const GsDraw *draw);

static void d3d9_bind_backbuffer(void) {
    if (sDevice != NULL && sBackbuffer != NULL) {
        sDevice->lpVtbl->SetRenderTarget(sDevice, 0, sBackbuffer);
        sDevice->lpVtbl->SetDepthStencilSurface(sDevice, NULL);
    }
}

static void d3d9_present_target(void) {
    IDirect3DSurface9 *source;
    HRESULT hr;
    if (sCurrentTarget < 0 || sCurrentTarget >= MAX_TARGETS || sTargets[sCurrentTarget].color == NULL ||
        sTargets[sCurrentTarget].color->surface == NULL || sBackbuffer == NULL) {
        return;
    }
    source = sTargets[sCurrentTarget].color->surface;
    hr = sDevice->lpVtbl->StretchRect(sDevice, source, NULL, sBackbuffer, NULL, D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        d3d9_log_hr("StretchRect(present)", hr);
    }
}

static int d3d9_init(void) {
    HRESULT hr;
    static const DWORD white = 0xFFFFFFFFu;
    D3d9Texture *texture;
    D3DLOCKED_RECT locked;

    sWindow = GsDraw_WindowCreate(0);
    if (sWindow == NULL) {
        return 0;
    }
    if (!d3d9_create_device()) {
        d3d9_release_all();
        return 0;
    }
    texture = (D3d9Texture *)calloc(1, sizeof(*texture));
    if (texture == NULL) {
        d3d9_release_all();
        return 0;
    }
    hr = sDevice->lpVtbl->CreateTexture(sDevice, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture->texture, NULL);
    if (FAILED(hr)) {
        d3d9_log_hr("CreateTexture(white)", hr);
        free(texture);
        d3d9_release_all();
        return 0;
    }
    texture->width = texture->height = 1;
    if (FAILED(texture->texture->lpVtbl->LockRect(texture->texture, 0, &locked, NULL, 0))) {
        texture->texture->lpVtbl->Release(texture->texture);
        free(texture);
        d3d9_release_all();
        return 0;
    }
    *(DWORD *)locked.pBits = white;
    texture->texture->lpVtbl->UnlockRect(texture->texture, 0);
    sWhite = texture;
    fprintf(stderr, "bt3: D3D9 bootstrap backend initialized\n");
    return 1;
}

static void d3d9_frame_end(void) {
    HRESULT hr;
    uint32_t i;
    SDL_PumpEvents();
    if (sDevice == NULL) {
        return;
    }
    if (sDeviceLost) {
        hr = sDevice->lpVtbl->TestCooperativeLevel(sDevice);
        if (hr == D3DERR_DEVICELOST) {
            return;
        }
        sDeviceLost = 0;
    }
    d3d9_bind_backbuffer();
    hr = sDevice->lpVtbl->Clear(sDevice, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(hr)) {
        d3d9_log_hr("Clear", hr);
        return;
    }
    hr = sDevice->lpVtbl->BeginScene(sDevice);
    if (FAILED(hr)) {
        d3d9_log_hr("BeginScene", hr);
        return;
    }
    for (i = 0; i < gsDrawCount; i++) {
        d3d9_draw_gs(&gsDraws[i]);
    }
    d3d9_present_target();
    sDevice->lpVtbl->EndScene(sDevice);
    hr = sDevice->lpVtbl->Present(sDevice, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST) {
        sDeviceLost = 1;
    } else if (FAILED(hr)) {
        d3d9_log_hr("Present", hr);
    }
}

static void d3d9_scale_changed(void) {
    int i;
    if (sDevice == NULL) {
        return;
    }
    for (i = 0; i < MAX_TARGETS; i++) {
        d3d9_target_release(&sTargets[i]);
    }
}

static GsTex d3d9_white_tex(void) { return (GsTex)(uintptr_t)sWhite; }

static D3d9Texture *d3d9_texture_from_handle(GsTex handle) {
    return (D3d9Texture *)(uintptr_t)handle;
}

static D3DFORMAT d3d9_texture_format(int format) {
    switch (format) {
    case GS_TEXFMT_BC1: return D3DFMT_DXT1;
    case GS_TEXFMT_BC2: return D3DFMT_DXT3;
    case GS_TEXFMT_BC3: return D3DFMT_DXT5;
    default: return D3DFMT_A8R8G8B8;
    }
}

static GsTex d3d9_tex_create(uint32_t width, uint32_t height, int format, int levels) {
    D3d9Texture *texture;
    D3DFORMAT d3dFormat = d3d9_texture_format(format);
    HRESULT hr;
    if (sDevice == NULL || sTextureCount >= D3D9_MAX_TEXTURES || !d3d9_texture_size(&width, &height)) {
        return 0;
    }
    texture = (D3d9Texture *)calloc(1, sizeof(*texture));
    if (texture == NULL) {
        return 0;
    }
    hr = sDevice->lpVtbl->CreateTexture(sDevice, width, height, levels < 1 ? 1 : levels, 0, d3dFormat, D3DPOOL_MANAGED, &texture->texture, NULL);
    if (FAILED(hr)) {
        free(texture);
        return 0;
    }
    texture->width = width;
    texture->height = height;
    texture->renderTarget = 0;
    texture->format = format;
    sTextureCount++;
    return (GsTex)(uintptr_t)texture;
}

static int d3d9_tex_upload(GsTex handle, int level, uint32_t width, uint32_t height, int format, const void *pixels, uint32_t bytes) {
    D3d9Texture *texture = d3d9_texture_from_handle(handle);
    void *ownedPixels = (void *)pixels;
    D3DLOCKED_RECT locked;
    uint32_t y;
    uint32_t rowBytes;
    uint32_t rows;
    int blockCompressed;
    (void)format;
    if (texture == NULL || texture->texture == NULL || pixels == NULL || level < 0 || level >= 16 ||
        width == 0 || height == 0 || width > texture->width || height > texture->height) {
        free(ownedPixels);
        return 0;
    }
    blockCompressed = texture->format == GS_TEXFMT_BC1 || texture->format == GS_TEXFMT_BC2 || texture->format == GS_TEXFMT_BC3;
    rowBytes = blockCompressed ? ((width + 3) / 4) * (texture->format == GS_TEXFMT_BC1 ? 8 : 16) : width * 4;
    rows = blockCompressed ? (height + 3) / 4 : height;
    if (bytes < rowBytes * rows || FAILED(texture->texture->lpVtbl->LockRect(texture->texture, level, &locked, NULL, 0))) {
        free(ownedPixels);
        return 0;
    }
    for (y = 0; y < rows; y++) {
        memcpy((uint8_t *)locked.pBits + (size_t)y * locked.Pitch, (const uint8_t *)pixels + (size_t)y * rowBytes, rowBytes);
    }
    texture->texture->lpVtbl->UnlockRect(texture->texture, level);
    free(ownedPixels);
    return 1;
}

static void d3d9_tex_destroy(GsTex handle) {
    D3d9Texture *texture = d3d9_texture_from_handle(handle);
    if (texture == NULL || texture == sWhite) {
        return;
    }
    d3d9_texture_release(texture);
    if (sTextureCount > 0) {
        sTextureCount--;
    }
}

static int d3d9_tex_slots_left(void) { return D3D9_MAX_TEXTURES - sTextureCount; }

static void d3d9_target_ensure(int index) {
    uint32_t size = (uint32_t)(GS_W * SCALE);
    D3d9Target *target;
    HRESULT hr;
    if (index < 0 || index >= MAX_TARGETS || sDevice == NULL || !d3d9_texture_size(&size, &size)) {
        return;
    }
    target = &sTargets[index];
    if (target->color != NULL) {
        return;
    }
    target->color = (D3d9Texture *)calloc(1, sizeof(*target->color));
    target->aux = (D3d9Texture *)calloc(1, sizeof(*target->aux));
    if (target->color == NULL || target->aux == NULL) {
        d3d9_target_release(target);
        return;
    }
    hr = sDevice->lpVtbl->CreateTexture(sDevice, size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target->color->texture, NULL);
    if (FAILED(hr)) {
        d3d9_log_hr("CreateTexture(target color)", hr);
        d3d9_target_release(target);
        return;
    }
    hr = target->color->texture->lpVtbl->GetSurfaceLevel(target->color->texture, 0, &target->color->surface);
    if (FAILED(hr)) {
        d3d9_target_release(target);
        return;
    }
    target->color->width = target->color->height = size;
    target->color->renderTarget = 1;
    hr = sDevice->lpVtbl->CreateTexture(sDevice, size, size, 1, D3DUSAGE_RENDERTARGET, sAuxFormat, D3DPOOL_DEFAULT, &target->aux->texture, NULL);
    if (FAILED(hr)) {
        d3d9_target_release(target);
        return;
    }
    hr = target->aux->texture->lpVtbl->GetSurfaceLevel(target->aux->texture, 0, &target->aux->surface);
    if (FAILED(hr)) {
        d3d9_target_release(target);
        return;
    }
    target->aux->width = target->aux->height = size;
    target->aux->renderTarget = 1;
    hr = sDevice->lpVtbl->CreateDepthStencilSurface(sDevice, size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &target->depth, NULL);
    if (FAILED(hr)) {
        d3d9_target_release(target);
    }
}

static void d3d9_target_drop(int index) {
    if (index >= 0 && index < MAX_TARGETS) {
        d3d9_target_release(&sTargets[index]);
    }
}

static GsTex d3d9_target_color(int index) { return index >= 0 && index < MAX_TARGETS ? (GsTex)(uintptr_t)sTargets[index].color : 0; }
static GsTex d3d9_target_aux(int index) { return index >= 0 && index < MAX_TARGETS ? (GsTex)(uintptr_t)sTargets[index].aux : 0; }
static GsTex d3d9_target_depth(int index) { return index >= 0 && index < MAX_TARGETS ? (GsTex)(uintptr_t)sTargets[index].depth : 0; }
static int d3d9_pipe_get(uint32_t key) {
    int i;
    for (i = 0; i < sPipeCount; i++) {
        if (sPipeKeys[i] == key) {
            return i;
        }
    }
    if (sPipeCount >= 1024) {
        return -1;
    }
    sPipeKeys[sPipeCount] = key;
    return sPipeCount++;
}

static void d3d9_set_target(int index) {
    IDirect3DSurface9 *color;
    IDirect3DSurface9 *depth;
    if (index < 0 || index >= MAX_TARGETS || sTargets[index].color == NULL || sDevice == NULL) {
        return;
    }
    color = sTargets[index].color->surface;
    depth = sTargets[index].depth;
    if (color == NULL || FAILED(sDevice->lpVtbl->SetRenderTarget(sDevice, 0, color))) {
        return;
    }
    if (depth != NULL) {
        sDevice->lpVtbl->SetDepthStencilSurface(sDevice, depth);
    }
    if (!sTargetSeen[index]) {
        sDevice->lpVtbl->ColorFill(sDevice, color, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
        sTargetSeen[index] = 1;
    }
    sCurrentTarget = index;
}

static void d3d9_set_scissor(const int *scissor) {
    RECT rect;
    if (sDevice == NULL || scissor == NULL) {
        return;
    }
    rect.left = scissor[0];
    rect.top = scissor[1];
    rect.right = scissor[0] + scissor[2];
    rect.bottom = scissor[1] + scissor[3];
    sDevice->lpVtbl->SetScissorRect(sDevice, &rect);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SCISSORTESTENABLE, TRUE);
}

static void d3d9_apply_pipeline(uint32_t key) {
    uint32_t bkey = key & 0x3FF;
    int ztst = (int)((key >> 10) & 3), zwrite = (int)((key >> 12) & 1);
    int wmask = (int)((key >> 16) & 15);
    int enabled = (bkey & 1) != 0;
    int src = (int)((bkey >> 1) & 7), dst = (int)((bkey >> 4) & 7);
    D3DBLEND srcBlend = src == 0 ? D3DBLEND_ZERO : src == 1 || src == 5 ? D3DBLEND_ONE : src == 4 ? D3DBLEND_INVDESTALPHA : D3DBLEND_SRCALPHA;
    D3DBLEND dstBlend = dst == 0 ? D3DBLEND_ZERO : dst == 1 || dst == 5 ? D3DBLEND_ONE : dst == 4 ? D3DBLEND_INVDESTALPHA : D3DBLEND_DESTALPHA;
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ALPHABLENDENABLE, enabled);
    if (enabled) {
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SRCBLEND, srcBlend);
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_DESTBLEND, dstBlend);
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_BLENDOP, (bkey >> 1 & 7) == 3 ? D3DBLENDOP_REVSUBTRACT : D3DBLENDOP_ADD);
    }
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ZENABLE, ztst == 0 ? D3DZB_FALSE : D3DZB_TRUE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ZFUNC, ztst == 1 ? D3DCMP_ALWAYS : ztst == 2 ? D3DCMP_GREATEREQUAL : D3DCMP_GREATER);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ZWRITEENABLE, zwrite != 0);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_COLORWRITEENABLE, wmask & 15);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_LIGHTING, FALSE);
}

static void d3d9_draw_gs(const GsDraw *draw) {
    uint32_t i;
    D3d9Texture *texture = d3d9_texture_from_handle(draw->tex);
    D3d9Vertex *vertices;
    uint32_t count = draw->count;
    if (draw->vu || draw->native || draw->target < 0 || draw->target >= MAX_TARGETS || count == 0 || count > 65535) {
        return;
    }
    d3d9_set_target(draw->target);
    d3d9_apply_pipeline(draw->pipeline >= 0 && draw->pipeline < sPipeCount ? sPipeKeys[draw->pipeline] : 0);
    d3d9_set_scissor(draw->scissor);
    vertices = (D3d9Vertex *)malloc((size_t)count * sizeof(*vertices));
    if (vertices == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        const Vtx *source = &gsVerts[draw->first + i];
        vertices[i].x = source->x * (float)SCALE - 0.5f;
        vertices[i].y = source->y * (float)SCALE - 0.5f;
        vertices[i].z = source->z;
        vertices[i].rhw = 1.0f;
        vertices[i].color = D3DCOLOR_ARGB(source->a, source->r, source->g, source->b);
        vertices[i].u = source->s;
        vertices[i].v = source->t;
    }
    sDevice->lpVtbl->SetFVF(sDevice, D3D9_FVF_TEXTURED);
    sDevice->lpVtbl->SetTexture(sDevice, 0, texture != NULL ? (IDirect3DBaseTexture9 *)texture->texture : NULL);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MINFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MAGFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSU, (draw->sampler & 2) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSV, (draw->sampler & 4) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SCISSORTESTENABLE, TRUE);
    sDevice->lpVtbl->DrawPrimitiveUP(sDevice, draw->count == 2 ? D3DPT_LINELIST : D3DPT_TRIANGLELIST,
                                     draw->count == 2 ? 1 : draw->count / 3, vertices, sizeof(*vertices));
    free(vertices);
}

static int d3d9_target_read(int index, uint8_t *rgba) {
    IDirect3DSurface9 *source;
    IDirect3DSurface9 *staging = NULL;
    D3DLOCKED_RECT locked;
    uint8_t *row;
    int x, y;
    HRESULT hr;
    if (index < 0 || index >= MAX_TARGETS || rgba == NULL || sTargets[index].color == NULL || sTargets[index].color->surface == NULL) {
        return 0;
    }
    source = sTargets[index].color->surface;
    hr = sDevice->lpVtbl->CreateOffscreenPlainSurface(sDevice, GS_W * SCALE, GS_H * SCALE, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &staging, NULL);
    if (FAILED(hr)) {
        d3d9_log_hr("CreateOffscreenPlainSurface(read)", hr);
        return 0;
    }
    hr = sDevice->lpVtbl->GetRenderTargetData(sDevice, source, staging);
    if (FAILED(hr) || FAILED(staging->lpVtbl->LockRect(staging, &locked, NULL, D3DLOCK_READONLY))) {
        staging->lpVtbl->Release(staging);
        return 0;
    }
    for (y = 0; y < 448; y++) {
        row = (uint8_t *)locked.pBits + (size_t)(y * SCALE + SCALE / 2) * locked.Pitch;
        for (x = 0; x < 512; x++) {
            memcpy(rgba + (y * 512 + x) * 4, row + (x * SCALE + SCALE / 2) * 4, 4);
        }
    }
    staging->lpVtbl->UnlockRect(staging);
    staging->lpVtbl->Release(staging);
    return 1;
}

static void d3d9_target_copy(int src, int dst) {
    if (src < 0 || src >= MAX_TARGETS || dst < 0 || dst >= MAX_TARGETS || sTargets[src].color == NULL ||
        sTargets[dst].color == NULL || sTargets[src].color->surface == NULL || sTargets[dst].color->surface == NULL) {
        return;
    }
    sDevice->lpVtbl->StretchRect(sDevice, sTargets[src].color->surface, NULL, sTargets[dst].color->surface, NULL, D3DTEXF_NONE);
}

GsBackend sD3d9Backend = {
    "d3d9",
    d3d9_init,
    d3d9_frame_end,
    d3d9_scale_changed,
    d3d9_white_tex,
    d3d9_tex_create,
    d3d9_tex_upload,
    d3d9_tex_destroy,
    d3d9_tex_slots_left,
    d3d9_target_ensure,
    d3d9_target_drop,
    d3d9_target_color,
    d3d9_target_aux,
    d3d9_target_depth,
    d3d9_pipe_get,
    d3d9_target_read,
    d3d9_target_copy,
};
#endif
