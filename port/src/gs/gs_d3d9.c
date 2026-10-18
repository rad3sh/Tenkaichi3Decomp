/*
 * Direct3D 9 bootstrap backend.
 *
 * This is intentionally small: it owns the native D3D9 device and presents a cleared window while the
 * shared GS capture and the full texture/primitive translation are brought over incrementally. The backend
 * still implements the complete GsBackend contract so it can be selected without changing game code.
 *
 * GPU VU shader path (SM 3.0):
 *   vu0/vu4/vu6 vertex programs are compiled at init from inline HLSL via D3DCompile (d3dcompiler_47.dll).
 *   gsVuVerts (12 floats/vertex) and gsVuIdx are uploaded once per frame into persistent D3D9 VB/IB.
 *   Constants from Vu0Uniform are loaded into VS registers c0..c18.
 *   If shader creation fails the backend falls back to the existing CPU-side transform path.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>   /* ID3DBlob, D3D_SHADER_MACRO, ID3DInclude (needed for D3DCompile signature) */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_internal.h"
#include "gs_draw.h"

extern SDL_Window *GsDraw_WindowCreate(int opengl);
extern int Port_PageAspectMilli(void);
extern int gsTargetCount;
extern int gGsMainFbp;

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

/* ---- GPU VU shader path state ---------------------------------------------------------------- */

/* Each vertex in gsVuVerts is 12 floats: position(xyzw), normal/color(xyzw), texcoord(xyzw).
   The vertex declaration below exposes these three streams as three float4 attributes. */
#define D3D9_VU_STRIDE (12 * sizeof(float))

/* VS constant-register layout, matching Vu0Uniform std140 (each field is 1 or 4 registers of float4):
     c0..c3   : boneA  (column-major mat4, 4 float4)
     c4..c7   : boneB
     c8       : pivotA
     c9       : pivotB
     c10..c13 : screen
     c14      : light
     c15      : color0
     c16      : color1
     c17      : misc
     c18      : light2   */
#define D3D9_VU_CONST_COUNT 19  /* float4 registers */

/* Dynamically loaded D3DCompile function pointer (d3dcompiler_47.dll / d3dcompiler_43.dll). */
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
    ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

static HMODULE sD3dCompilerDll;
static PFN_D3DCompile sD3dCompileFn;

/* The three VS programs (NULL if compilation failed → CPU fallback). */
static IDirect3DVertexShader9 *sVuShader[3]; /* [0]=vu0, [1]=vu4, [2]=vu6 */
static IDirect3DVertexDeclaration9 *sVuDecl;
static int sVuShaderReady; /* 1 if all three shaders + declaration are valid */

/* Persistent GPU vertex buffer for gsVuVerts (re-created if capacity grows). */
static IDirect3DVertexBuffer9 *sVuVb;
static uint32_t sVuVbCapacity; /* in vertices */

/* Persistent GPU index buffer for gsVuIdx (re-created if capacity grows). */
static IDirect3DIndexBuffer9 *sVuIb;
static uint32_t sVuIbCapacity; /* in indices */

/* ---- ps_3_0 GS pixel shader state ----------------------------------------------------------- */
/*
 * sGsPs implements the essential behaviour of shaders/gs.frag as a ps_3_0 HLSL shader.
 *
 * PS constant registers (float4):
 *   c0  mode  : x=textured (0/1/2/3), y=TFX (0=MODULATE, 1=DECAL, 2=HIGHLIGHT),
 *                z=TCC (0=RGB only, 1=RGBA), w=alpha-test function index (0=off, else ATST+1)
 *   c1  misc  : x=AREF (0..128 GS units), y=dest-alpha-test (0 off; 1/2: unused here),
 *                z=FBA (1 = set alpha bit 7), w=2D-pixel-scale (0 = sample per fragment)
 *   c2  rect  : uv clamp region for frame-buffer-as-texture (u0,v0,u1,v1)
 *   c3  orig  : xy=original texture size, z=largest original alpha, w=smooth-2D flag
 *   c4  extra : x=unused(depth, not written here), y=blendc (pre-multiply flag), z=0, w=0
 *
 * Limitations vs gs.frag:
 *   - No gl_FragDepth write: SM3 ps_3_0 can write DEPTH but we skip it for performance;
 *     depth is handled entirely by D3D9's fixed-function depth test (z from VS output).
 *   - dateTex (destination-alpha test) is not supported: D3D9 lacks the equivalent
 *     of texelFetch() on a copy-before-draw; misc.y is always 0 in practice.
 *   - 2D sub-pixel snap (misc.w != 0 paths) is approximated as plain uv pass-through;
 *     the GS-pixel-grid clamping is preserved via rect when mode.x==2.
 *   - mode.x==3 (texture pack replacements) is treated identically to mode.x==1.
 */
static IDirect3DPixelShader9 *sGsPs; /* NULL → fixed-function fallback */
static IDirect3DPixelShader9 *sD3d9OutlinePs;
static int sGsPsReady;               /* 1 when sGsPs != NULL and was successfully set */

/* HLSL source for the GS pixel shader (ps_3_0). */
static const char kGsPsSrc[] =
    /* PS constant registers */
    "float4 psMode  : register(c0);\n"  /* x=textured, y=TFX, z=TCC, w=ATST+1 (0=off) */
    "float4 psMisc  : register(c1);\n"  /* x=AREF, y=date, z=FBA, w=2D-scale */
    "float4 psRect  : register(c2);\n"  /* u0,v0,u1,v1 */
    "float4 psOrig  : register(c3);\n"  /* xy=origSz, z=maxAlpha, w=smooth */
    "float4 psExtra : register(c4);\n"  /* y=blendc (extra.y != 0 → premultiply alpha) */
    /* Sampler */
    "sampler2D tex : register(s0);\n"
    /* Input from VS.  Both VU-GPU (which sets stq.z = 1 in the VS) and the FVF non-VU path (which sends
       float2 UV with stq.z implicitly 1) share the same struct.  D3D9 zero-extends missing components,
       so stq.z defaults to 0 when only float2 is emitted.  We therefore always set TEXCOORD1 to carry
       the q-divide explicitly: VU VS writes float3; FVF path writes float2 and q = 1 via the vertex's
       u,v (see D3d9Vertex.u, .v). To avoid ambiguity, use a dedicated TC1 for q or simply skip
       the divide and trust that the VU VS has already divided (z = 1). */
    "struct PS_IN { float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
    "float4 main(PS_IN i) : COLOR0 {\n"
    /* GS alpha scale k = 255/128 ≈ 1.9922; carried through vColor.a * k. */
    "    float k = 255.0 / 128.0;\n"
    "    float3 rgb = i.col.rgb;\n"
    "    float  a   = i.col.a * k;\n"
    "    if (psMode.x != 0.0) {\n"
    "        float2 uv = i.uv;\n"
    /* Clamp for frame-buffer-as-texture (mode.x==2) or tight 2D sheets. */
    "        if (psMode.x == 2.0) uv = clamp(uv, psRect.xy, psRect.zw);\n"
    "        float4 t = tex2D(tex, uv);\n"
    /* Textures from GS memory / replacements: scale alpha by k (0x80 = fully opaque). */
    "        if (psMode.x == 1.0 || psMode.x == 3.0) t.a *= k;\n"
    /* Clamp replacement alpha to original's max; treat near-opaque as opaque. */
    "        if (psMode.x == 3.0 && psOrig.z < 1.0) t.a = min(t.a, psOrig.z);\n"
    "        else if (psMode.x == 3.0 && t.a > 0.93 && t.a < 1.0) t.a = 1.0;\n"
    /* TFX 0: MODULATE (the GS's 0x80 colour scale is k = 255/128). */
    "        if (psMode.y == 0.0) {\n"
    "            rgb = t.rgb * i.col.rgb * k;\n"    "            if (psMode.z != 0.0) a = t.a * a;\n"
    /* TFX 1: DECAL     (t.rgb; optional t.a) */
    "        } else if (psMode.y == 1.0) {\n"
    "            rgb = t.rgb;\n"
    "            if (psMode.z != 0.0) a = t.a;\n"
    /* TFX 2: HIGHLIGHT  (t * vColor * k + vColor.a; optional t.a + a) */
    "        } else {\n"
    "            rgb = t.rgb * i.col.rgb * k + i.col.aaa;\n"
    "            if (psMode.z != 0.0) a = (psMode.y == 2.0) ? t.a + a : t.a;\n"
    "        }\n"
    "    }\n"
    /* Alpha test (ATST). psMode.w == 0 → off. psMode.w-1 gives ATST index 0..7.
       Use a float kill value: clip(v) discards the fragment when v < 0. */
    "    if (psMode.w != 0.0) {\n"
    "        float ag = floor(a * 128.0 + 0.5);\n"
    "        int f = (int)psMode.w - 1;\n"
    "        float ref = psMisc.x;\n"
    "        float killv;\n"
    "        if      (f == 0) killv = -1.0;\n"
    "        else if (f == 1) killv =  1.0;\n"
    "        else if (f == 2) killv = ref - ag - 0.001;\n"   /* pass when ag < ref  */
    "        else if (f == 3) killv = ref - ag + 0.001;\n"   /* pass when ag <= ref */
    "        else if (f == 4) killv = 0.5 - abs(ag - ref);\n" /* pass when |ag-ref|<0.5 */
    "        else if (f == 5) killv = ag - ref - 0.001;\n"   /* pass when ag >= ref */
    "        else if (f == 6) killv = ag - ref + 0.001;\n"   /* pass when ag > ref  */
    "        else             killv = abs(ag - ref) - 0.5;\n" /* pass when |ag-ref|>=0.5 */
    "        clip(killv);\n"
    "    }\n"
    /* extra.y != 0: pre-multiply colour by alpha (GS As/0x80 blending above 0x80). */
    "    if (psExtra.y != 0.0) rgb *= a;\n"
    "    float4 out0;\n"
    "    out0.rgb = clamp(rgb, 0.0, 1.0);\n"
    "    out0.a   = clamp(a,   0.0, 1.0);\n"
    "    return out0;\n"
    "}\n";

/* ----------------------------------------------------------------------------------------------- */

static SDL_Window *sWindow;
static IDirect3D9 *sD3d;
static IDirect3DDevice9 *sDevice;
static IDirect3DSurface9 *sBackbuffer;
static D3DPRESENT_PARAMETERS sPresent;
static int sD3d9Vsync;
static D3d9Texture *sWhite;
static D3d9Target sTargets[MAX_TARGETS];
static int sTextureCount;
static int sDeviceLost;
static int sPipeCount;
static uint32_t sPipeKeys[1024];
/* Keep the auxiliary attachment in the same format as the colour target so native alpha snapshots can use
   StretchRect on D3D9 drivers that reject A8 source/destination pairs. Its alpha byte is still the GS mask. */
static D3DFORMAT sAuxFormat = D3DFMT_A8R8G8B8;
static D3DFORMAT sRgbaTextureFormat = D3DFMT_A8R8G8B8;
static int sD3d9MrtReady;
static D3d9Texture *sD3d9AuxCopy;
static D3d9Texture *sFbTex[2]; /* display-buffer pictures uploaded directly by the movie player */
static unsigned char sTargetSeen[MAX_TARGETS];
static int sCurrentTarget = -1;
static D3d9Vertex *sVuVertices;
static uint32_t sVuVertexCapacity;
/* Dynamic stream for ordinary GS/2D draws. DrawPrimitiveUP forces the runtime to copy every draw through a
   temporary upload path; keeping one persistent dynamic buffer removes that per-draw allocation and copy. */
static IDirect3DVertexBuffer9 *sGsVb;
static D3d9Vertex *sGsVertices;
static uint32_t sGsVertexCapacity;
static uint32_t sD3d9DrawCount;
static uint32_t sD3d9VuVertexCount;
typedef struct D3d9Telemetry {
    int enabled;
    uint32_t frame;
    uint32_t draws;
    uint32_t vuDraws;
    uint32_t draw2d;
    uint32_t nativeDraws;
    uint64_t vertices;
    uint64_t indices;
    uint32_t targetChanges;
    uint32_t textureChanges;
    uint32_t pipelineChanges;
    uint64_t uploadNs;
    uint64_t submitNs;
    uint64_t presentNs;
    uint32_t frames16;
    uint32_t frames33;
    uint64_t frameStartNs;
    uint64_t frameSubmitStartNs;
} D3d9Telemetry;
static D3d9Telemetry sD3d9Telemetry;
static uint32_t sD3d9LastTarget = UINT32_MAX;
static uint32_t sD3d9LastPipeline = UINT32_MAX;
static uintptr_t sD3d9LastTexture;
static uintptr_t sD3d9BoundTexture;
static uint32_t sD3d9AppliedPipeline = UINT32_MAX;
static float sD3d9AppliedBlendConstant;
static int sD3d9Scissor[4];
static int sD3d9ScissorValid;

static uint64_t d3d9_now_ns(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000000ll) / frequency.QuadPart);
}

static void d3d9_telemetry_init(void) {
    memset(&sD3d9Telemetry, 0, sizeof(sD3d9Telemetry));
    sD3d9Telemetry.enabled = getenv("BT3_D3D9_TELEMETRY") != NULL && atoi(getenv("BT3_D3D9_TELEMETRY")) != 0;
    sD3d9LastTarget = UINT32_MAX;
    sD3d9LastPipeline = UINT32_MAX;
    sD3d9LastTexture = 0;
    sD3d9AppliedPipeline = UINT32_MAX;
    sD3d9AppliedBlendConstant = -1.0f;
    sD3d9ScissorValid = 0;
}

static void d3d9_telemetry_frame_begin(void) {
    if (sD3d9Telemetry.enabled) {
        sD3d9Telemetry.frame++;
        sD3d9Telemetry.frameStartNs = d3d9_now_ns();
        sD3d9Telemetry.frameSubmitStartNs = sD3d9Telemetry.frameStartNs;
    }
}

static void d3d9_telemetry_log_frame(uint64_t frameNs) {
    if (!sD3d9Telemetry.enabled) return;
    if (frameNs > 16670000ull) sD3d9Telemetry.frames16++;
    if (frameNs > 33330000ull) sD3d9Telemetry.frames33++;
    fprintf(stderr, "d3d9-telemetry: frame=%u draws=%u vu_draws=%u draws_2d=%u native=%u vertices=%llu indices=%llu targets=%u textures=%u pipelines=%u upload_ms=%.3f submit_ms=%.3f present_ms=%.3f frame_ms=%.3f over16=%u over33=%u\n",
            sD3d9Telemetry.frame, sD3d9Telemetry.draws, sD3d9Telemetry.vuDraws, sD3d9Telemetry.draw2d,
            sD3d9Telemetry.nativeDraws, (unsigned long long)sD3d9Telemetry.vertices,
            (unsigned long long)sD3d9Telemetry.indices, sD3d9Telemetry.targetChanges,
            sD3d9Telemetry.textureChanges, sD3d9Telemetry.pipelineChanges,
            sD3d9Telemetry.uploadNs / 1000000.0, sD3d9Telemetry.submitNs / 1000000.0,
            sD3d9Telemetry.presentNs / 1000000.0, frameNs / 1000000.0,
            sD3d9Telemetry.frames16, sD3d9Telemetry.frames33);
    sD3d9Telemetry.draws = sD3d9Telemetry.vuDraws = sD3d9Telemetry.draw2d = sD3d9Telemetry.nativeDraws = 0;
    sD3d9Telemetry.vertices = sD3d9Telemetry.indices = 0;
    sD3d9Telemetry.targetChanges = sD3d9Telemetry.textureChanges = sD3d9Telemetry.pipelineChanges = 0;
    sD3d9Telemetry.uploadNs = sD3d9Telemetry.submitNs = sD3d9Telemetry.presentNs = 0;
}

static void d3d9_log_hr(const char *what, HRESULT hr) {
    fprintf(stderr, "bt3: d3d9: %s failed (0x%08lX)\n", what, (unsigned long)hr);
}

/* ---- HLSL vertex shader sources (SM 3.0) ---------------------------------------------------- */
/*
 * Constants mirror Vu0Uniform std140; matrices are stored column-major in consecutive float4 registers.
 * The uniform block layout (each row = one float4 register):
 *   c0..c3   boneA   (column-major mat4: c0=col0, c1=col1, c2=col2, c3=col3)
 *   c4..c7   boneB
 *   c8       pivotA
 *   c9       pivotB
 *   c10..c13 screen
 *   c14      light
 *   c15      color0
 *   c16      color1
 *   c17      misc    (x=XYOFFSET.x, y=XYOFFSET.y, z=depthMax, w=layer)
 *   c18      light2
 *
 * In HLSL SM3.0 we declare column_major float4x4 so that mul(M, v) matches GLSL mat4 * vec4.
 * Output POSITION is D3D9 clip space: x,y in [-w,w], z in [0,w], w=s.w.
 * The projection matches vu0.vert/vu4.vert/vu6.vert exactly.
 * COLOR0 carries the interpolated RGBA for the fixed-function pixel stage.
 * TEXCOORD0 carries (s, t, 1) for the fixed-function texture stage.
 */

/* Helper macro: the common screen-space formula used by all three programs.
   Returns clip-space (px,py,pz,pw) from post-transform (sx,sy,sz,sw) and misc (offx,offy,depthMax). */
#define D3D9_VU_PROJ_HLSL \
    "float4 vu_proj(float sx, float sy, float sz, float sw, float offx, float offy, float depthMax) {\n" \
    "    float nx = (sx - offx * sw) / 512.0 - sw;\n" \
    "    float ny = sw - (sy - offy * sw) / 512.0;\n" \
    "    float nd;\n" \
    "    if (sw > 0.0 && depthMax > 0.0) { nd = (floor(sz / sw) + 0.5) / depthMax * sw; nd = clamp(nd, 0.0, sw); }\n" \
    "    else { nd = clamp(sz / abs(depthMax), 0.0, 1.0) * sw; }\n" \
    "    return float4(nx, ny, nd, sw);\n" \
    "}\n"

static const char kVu0HlslSrc[] =
    /* vu0.vert: two-bone skinning, multiple sub-modes from misc.w */
    "column_major float4x4 boneA  : register(c0);\n"
    "column_major float4x4 boneB  : register(c4);\n"
    "float4 pivotA : register(c8);\n"
    "float4 pivotB : register(c9);\n"
    "column_major float4x4 screen : register(c10);\n"
    "float4 light  : register(c14);\n"
    "float4 color0 : register(c15);\n"
    "float4 color1 : register(c16);\n"
    "float4 misc   : register(c17);\n"
    "float4 light2 : register(c18);\n"
    D3D9_VU_PROJ_HLSL
    "struct VS_IN  { float4 inPos : POSITION; float4 inNormal : NORMAL; float4 inSt : TEXCOORD0; };\n"
    "struct VS_OUT { float4 pos : POSITION; float4 col : COLOR0; float3 stq : TEXCOORD0; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o;\n"
    "    float miscW = misc.w;\n"
    "    float w = (miscW == 4.0) ? 1.0 : i.inPos.w;\n"
    "    float3 pa = mul(boneA, float4(i.inPos.xyz - pivotA.xyz, 1.0)).xyz;\n"
    "    float3 pb = mul(boneB, float4(i.inPos.xyz - pivotB.xyz, 1.0)).xyz;\n"
    "    float4 s  = mul(screen, float4(lerp(pb, pa, w), 1.0));\n"
    "    if (miscW == 2.0 && s.w < 0.0) s = -s;\n"
    "    o.pos = vu_proj(s.x, s.y, s.z, s.w, misc.x, misc.y, misc.z);\n"
    "    if (miscW == 2.0) o.pos.z = clamp(s.z / abs(misc.z), 0.0, 1.0) * s.w;\n"
    "    float4 cr; float2 uv;\n"
    "    if (miscW == 5.0) {\n"
    "        float4 n1 = float4(i.inNormal.xyz, 1.0);\n"
    "        uv = float2(dot(light, n1) * 0.5 + 0.5, dot(light2, n1) * 0.5 + 0.5);\n"
    "        cr = saturate(color1 / 255.0);\n"
    "    } else if (miscW >= 3.0) {\n"
    "        cr = saturate(floor(float4(i.inNormal.rgb, miscW == 4.0 ? color0.a : i.inNormal.a)) / 255.0);\n"
    "        uv = i.inSt.xy;\n"
    "    } else if (miscW == 1.0) {\n"
    "        float3 bnA = mul((float3x3)boneA, i.inNormal.xyz);\n"
    "        float3 bnB = mul((float3x3)boneB, i.inNormal.xyz);\n"
    "        float na = dot(light.xyz, bnA), nb = dot(light.xyz, bnB);\n"
    "        cr = saturate(color1 / 255.0);\n"
    "        uv = float2((lerp(nb, na, w) + light.w) * 0.5 + 0.5, 0.0);\n"
    "    } else {\n"
    "        cr = saturate(color0 / 255.0);\n"
    "        uv = i.inSt.xy;\n"
    "    }\n"
    "    o.col = cr;\n"
    "    o.stq = float3(uv, 1.0);\n"
    "    return o;\n"
    "}\n";

static const char kVu4HlslSrc[] =
    /* vu4.vert: static geometry, screen = M * pos, colour from inColor (0..255 floats) */
    "column_major float4x4 boneA  : register(c0);\n"  /* unused but declared to keep reg layout */
    "column_major float4x4 boneB  : register(c4);\n"
    "float4 pivotA : register(c8);\n"
    "float4 pivotB : register(c9);\n"
    "column_major float4x4 screen : register(c10);\n"
    "float4 light  : register(c14);\n"
    "float4 color0 : register(c15);\n"
    "float4 color1 : register(c16);\n"
    "float4 misc   : register(c17);\n"
    "float4 light2 : register(c18);\n"
    D3D9_VU_PROJ_HLSL
    "struct VS_IN  { float4 inPos : POSITION; float4 inColor : NORMAL; float4 inSt : TEXCOORD0; };\n"
    "struct VS_OUT { float4 pos : POSITION; float4 col : COLOR0; float3 stq : TEXCOORD0; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o;\n"
    "    float4 s = mul(screen, i.inPos);\n"
    "    o.pos = vu_proj(s.x, s.y, s.z, s.w, misc.x, misc.y, misc.z);\n"
    "    o.col = saturate(floor(i.inColor) / 255.0);\n"
    "    o.stq = float3(i.inSt.xy, 1.0);\n"
    "    return o;\n"
    "}\n";

static const char kVu6HlslSrc[] =
    /* vu6.vert: ground shadow; screen = M*pos; shadow UV from boneA (shadowCam)*pos */
    "column_major float4x4 shadowCam : register(c0);\n"  /* boneA slot = shadowCam */
    "column_major float4x4 boneB     : register(c4);\n"
    "float4 scale    : register(c8);\n"   /* pivotA slot: scale.x */
    "float4 texScale : register(c9);\n"  /* pivotB slot: texScale.xy */
    "column_major float4x4 screen    : register(c10);\n"
    "float4 light  : register(c14);\n"
    "float4 color0 : register(c15);\n"
    "float4 color1 : register(c16);\n"
    "float4 misc   : register(c17);\n"
    "float4 light2 : register(c18);\n"
    D3D9_VU_PROJ_HLSL
    "struct VS_IN  { float4 inPos : POSITION; float4 inColor : NORMAL; float4 unused : TEXCOORD0; };\n"
    "struct VS_OUT { float4 pos : POSITION; float4 col : COLOR0; float3 stq : TEXCOORD0; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o;\n"
    "    float4 p  = float4(i.inPos.xyz, 1.0);\n"
    "    float4 s  = mul(screen,    p);\n"
    "    float4 c  = mul(shadowCam, p);\n"
    "    float2 st = (c.xy * scale.x + 1.0) * 0.5 * texScale.xy;\n"
    "    o.pos = vu_proj(s.x, s.y, s.z, s.w, misc.x, misc.y, misc.z);\n"
    "    o.col = saturate(floor(i.inColor) / 255.0);\n"
    "    o.stq = float3(st, 1.0);\n"
    "    return o;\n"
    "}\n";

/* ---- GPU VU shader initialisation ----------------------------------------------------------- */

/* Load D3DCompile dynamically; returns 1 on success. */
static int d3d9_load_compiler(void) {
    static const char *kDlls[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" };
    int i;
    if (sD3dCompileFn != NULL) return 1;
    for (i = 0; i < 3; i++) {
        sD3dCompilerDll = LoadLibraryA(kDlls[i]);
        if (sD3dCompilerDll != NULL) {
            sD3dCompileFn = (PFN_D3DCompile)(void *)GetProcAddress(sD3dCompilerDll, "D3DCompile");
            if (sD3dCompileFn != NULL) {
                fprintf(stderr, "bt3: d3d9: loaded %s\n", kDlls[i]);
                return 1;
            }
            FreeLibrary(sD3dCompilerDll);
            sD3dCompilerDll = NULL;
        }
    }
    fprintf(stderr, "bt3: d3d9: D3DCompile not found; VU GPU shader path disabled\n");
    return 0;
}

static IDirect3DVertexShader9 *d3d9_compile_vs(const char *src, const char *name) {
    ID3DBlob *code = NULL, *errors = NULL;
    IDirect3DVertexShader9 *vs = NULL;
    HRESULT hr;
    if (sD3dCompileFn == NULL) return NULL;
    hr = sD3dCompileFn(src, strlen(src), name, NULL, NULL, "main", "vs_3_0",
                       (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "bt3: d3d9: VS compile %s failed (0x%08lX)", name, (unsigned long)hr);
        if (errors) {
            fprintf(stderr, ": %s", (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        fprintf(stderr, "\n");
        return NULL;
    }
    if (errors) { errors->lpVtbl->Release(errors); errors = NULL; }
    hr = sDevice->lpVtbl->CreateVertexShader(sDevice, (const DWORD *)code->lpVtbl->GetBufferPointer(code), &vs);
    code->lpVtbl->Release(code);
    if (FAILED(hr)) { d3d9_log_hr(name, hr); return NULL; }
    return vs;
}

/* ---- GS pixel shader init / release --------------------------------------------------------- */

static IDirect3DPixelShader9 *d3d9_compile_ps(const char *src, const char *name) {
    ID3DBlob *code = NULL, *errors = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr;
    if (!d3d9_load_compiler()) return NULL;
    hr = sD3dCompileFn(src, strlen(src), name, NULL, NULL, "main", "ps_3_0", (1u << 15), 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) { fprintf(stderr, "bt3: d3d9: PS compile %s: %s\n", name, (const char *)errors->lpVtbl->GetBufferPointer(errors)); errors->lpVtbl->Release(errors); }
        return NULL;
    }
    if (errors) errors->lpVtbl->Release(errors);
    hr = sDevice->lpVtbl->CreatePixelShader(sDevice, (const DWORD *)code->lpVtbl->GetBufferPointer(code), &ps);
    code->lpVtbl->Release(code);
    return FAILED(hr) ? NULL : ps;
}

static const char kD3d9OutlinePsSrc[] =
    "sampler2D aux : register(s0); float4 p : register(c0);"
    "float table(float2 uv) { float id=floor(tex2D(aux,uv).r*255.0+0.5); if(id==255.0)return 0.0; float c=fmod(id*8.0,256.0); return c==0.0?128.0:c; }"
    "float4 main(float2 uv:TEXCOORD0):COLOR0 { float h=table(uv); float r=table(uv+float2(p.x,0)); float d=table(uv+float2(0,p.y)); float l=table(uv-float2(p.x,0)); float u=table(uv-float2(0,p.y)); float e=(h>r||h>d||h>l||h>u)?p.z:0; return float4(e,e,e,0); }";

static void d3d9_gs_ps_init(void) {
    ID3DBlob *code = NULL, *errors = NULL;
    const char *enabled = getenv("BT3_D3D9_PIXEL_SHADER");
    HRESULT hr;
    sGsPs = NULL;
    sGsPsReady = 0;
    /* The shader is the faithful GS path: it preserves the 0x80 alpha convention and the RGB scale used by
       MODULATE/HIGHLIGHT. Keep an explicit BT3_D3D9_PIXEL_SHADER=0 escape hatch for old drivers. */
    if (enabled != NULL && atoi(enabled) == 0) {
        fprintf(stderr, "bt3: d3d9: GS pixel shader disabled (BT3_D3D9_PIXEL_SHADER=0)\n");
        return;
    }
    if (!d3d9_load_compiler()) {
        fprintf(stderr, "bt3: d3d9: GS pixel shader disabled (no D3DCompile)\n");
        return;
    }
    hr = sD3dCompileFn(kGsPsSrc, strlen(kGsPsSrc), "gs_ps", NULL, NULL, "main", "ps_3_0",
                       (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "bt3: d3d9: GS PS compile failed (0x%08lX)", (unsigned long)hr);
        if (errors) {
            fprintf(stderr, ": %s", (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        fprintf(stderr, "; using fixed-function pixel stage\n");
        return;
    }
    if (errors) { errors->lpVtbl->Release(errors); errors = NULL; }
    hr = sDevice->lpVtbl->CreatePixelShader(sDevice,
             (const DWORD *)code->lpVtbl->GetBufferPointer(code), &sGsPs);
    code->lpVtbl->Release(code);
    if (FAILED(hr)) {
        d3d9_log_hr("CreatePixelShader(gs_ps)", hr);
        fprintf(stderr, "bt3: d3d9: GS PS creation failed; using fixed-function pixel stage\n");
        return;
    }
    sGsPsReady = 1;
    fprintf(stderr, "bt3: d3d9: GS pixel shader ready (ps_3_0)\n");
}

static void d3d9_gs_ps_release(void) {
    if (sGsPs) { sGsPs->lpVtbl->Release(sGsPs); sGsPs = NULL; }
    if (sD3d9OutlinePs) { sD3d9OutlinePs->lpVtbl->Release(sD3d9OutlinePs); sD3d9OutlinePs = NULL; }
    sGsPsReady = 0;
}

/* Load PS constants from a GsDraw.  PS constant layout (float4):
     c0 mode: x=textured, y=TFX, z=TCC, w=ATST+1
     c1 misc: x=AREF, y=dest-alpha-test, z=FBA, w=2D-scale
     c2 rect: u0,v0,u1,v1
     c3 orig: xy=origSz, z=maxAlpha, w=smooth
     c4 extra: x=0, y=blendc (extra.y in GLSL), z=0, w=0 */
static void d3d9_gs_ps_set_constants(const GsDraw *draw) {
    float c0[4], c4[4];
    c0[0] = (float)draw->mode[0];
    c0[1] = (float)draw->mode[1];
    c0[2] = (float)draw->mode[2];
    c0[3] = (float)draw->mode[3];
    c4[0] = 0.0f;                /* depth max unused */
    c4[1] = draw->blendc;        /* extra.y: pre-multiply flag */
    c4[2] = 0.0f;
    c4[3] = 0.0f;
    sDevice->lpVtbl->SetPixelShaderConstantF(sDevice, 0, c0,         1);
    sDevice->lpVtbl->SetPixelShaderConstantF(sDevice, 1, draw->misc, 1);
    sDevice->lpVtbl->SetPixelShaderConstantF(sDevice, 2, draw->rect, 1);
    sDevice->lpVtbl->SetPixelShaderConstantF(sDevice, 3, draw->orig, 1);
    sDevice->lpVtbl->SetPixelShaderConstantF(sDevice, 4, c4,         1);
}

/* Bind (or unbind) the GS pixel shader.  When sGsPsReady == 0, NULL is set to restore fixed-function. */
static void d3d9_gs_ps_bind(const GsDraw *draw) {
    if (!sGsPsReady || sGsPs == NULL) {
        sDevice->lpVtbl->SetPixelShader(sDevice, NULL);
        return;
    }
    sDevice->lpVtbl->SetPixelShader(sDevice, sGsPs);
    d3d9_gs_ps_set_constants(draw);
}

/* Fixed-function fallback keeps vertex and texture alpha in GS units. MODULATE4X converts
   the two 0x80=opaque alpha values to ordinary D3D alpha. TCC=0 ignores texture alpha. */
static void d3d9_fixed_texture_stage(const GsDraw *draw) {
    D3DTEXTUREOP colorOp = draw->mode[0] == 0 ? D3DTOP_SELECTARG2 :
                            draw->mode[1] == 1 ? D3DTOP_SELECTARG1 :
                            draw->mode[1] == 2 ? D3DTOP_ADD : D3DTOP_MODULATE;
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_COLOROP, colorOp);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(128, 255, 255, 255));
    if (draw->mode[0] != 0 && draw->mode[2] != 0) {
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAOP,
                                               draw->vu ? D3DTOP_MODULATE4X : D3DTOP_MODULATE2X);
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    } else if (draw->vu) {
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    } else {
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        sDevice->lpVtbl->SetTextureStageState(sDevice, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    }
}

static void d3d9_fixed_alpha_test(const GsDraw *draw) {
    static const D3DCMPFUNC funcs[8] = {
        D3DCMP_NEVER, D3DCMP_ALWAYS, D3DCMP_LESS, D3DCMP_LESSEQUAL,
        D3DCMP_EQUAL, D3DCMP_GREATEREQUAL, D3DCMP_GREATER, D3DCMP_NOTEQUAL
    };
    int f = draw->mode[3] - 1;
    DWORD ref = (DWORD)(draw->misc[0] * 2.0f + 0.5f);
    if (ref > 255) ref = 255;
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ALPHATESTENABLE, draw->mode[3] != 0);
    if (draw->mode[3] != 0) {
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ALPHAFUNC, f >= 0 && f < 8 ? funcs[f] : D3DCMP_ALWAYS);
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ALPHAREF, ref);
    }
}

/* Create the vertex declaration for 12-float VU vertices:
     stream 0, offset  0: float4 POSITION  (inPos)
     stream 0, offset 16: float4 NORMAL    (inNormal / inColor)
     stream 0, offset 32: float4 TEXCOORD0 (inSt / unused)   */
static IDirect3DVertexDeclaration9 *d3d9_make_vu_decl(void) {
    IDirect3DVertexDeclaration9 *decl = NULL;
    static const D3DVERTEXELEMENT9 kElements[] = {
        {0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    HRESULT hr = sDevice->lpVtbl->CreateVertexDeclaration(sDevice, kElements, &decl);
    if (FAILED(hr)) { d3d9_log_hr("CreateVertexDeclaration(VU)", hr); return NULL; }
    return decl;
}

static void d3d9_vu_shaders_init(void) {
    sVuShaderReady = 0;
    if (!d3d9_load_compiler()) return;
    sVuShader[0] = d3d9_compile_vs(kVu0HlslSrc, "vu0");
    sVuShader[1] = d3d9_compile_vs(kVu4HlslSrc, "vu4");
    sVuShader[2] = d3d9_compile_vs(kVu6HlslSrc, "vu6");
    if (sVuShader[0] == NULL || sVuShader[1] == NULL || sVuShader[2] == NULL) {
        fprintf(stderr, "bt3: d3d9: VU shader init failed; falling back to CPU transforms\n");
        return;
    }
    sVuDecl = d3d9_make_vu_decl();
    if (sVuDecl == NULL) return;
    sVuShaderReady = 1;
    fprintf(stderr, "bt3: d3d9: VU GPU shaders ready (sm3_0)\n");
    /* GS pixel shader: compiled here because the VU path also requires it. */
    d3d9_gs_ps_init();
}

static void d3d9_vu_shaders_release(void) {
    int i;
    d3d9_gs_ps_release();
    for (i = 0; i < 3; i++) {
        if (sVuShader[i]) { sVuShader[i]->lpVtbl->Release(sVuShader[i]); sVuShader[i] = NULL; }
    }
    if (sVuDecl)  { sVuDecl->lpVtbl->Release(sVuDecl);   sVuDecl  = NULL; }
    if (sVuVb)    { sVuVb->lpVtbl->Release(sVuVb);       sVuVb    = NULL; sVuVbCapacity = 0; }
    if (sVuIb)    { sVuIb->lpVtbl->Release(sVuIb);       sVuIb    = NULL; sVuIbCapacity = 0; }
    sVuShaderReady = 0;
}

/* Ensure VB has space for `count` VU vertices (each 48 bytes).  Returns 1 on success. */
static int d3d9_vu_vb_ensure(uint32_t count) {
    IDirect3DVertexBuffer9 *newVb = NULL;
    HRESULT hr;
    if (count == 0) return 0;
    if (sVuVb != NULL && count <= sVuVbCapacity) return 1;
    if (sVuVb != NULL) { sVuVb->lpVtbl->Release(sVuVb); sVuVb = NULL; sVuVbCapacity = 0; }
    hr = sDevice->lpVtbl->CreateVertexBuffer(sDevice,
             count * (UINT)D3D9_VU_STRIDE,
             D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &newVb, NULL);
    if (FAILED(hr)) { d3d9_log_hr("CreateVertexBuffer(VU)", hr); return 0; }
    sVuVb = newVb;
    sVuVbCapacity = count;
    return 1;
}

/* Ensure IB has space for `count` 32-bit indices.  Returns 1 on success. */
static int d3d9_vu_ib_ensure(uint32_t count) {
    IDirect3DIndexBuffer9 *newIb = NULL;
    HRESULT hr;
    if (count == 0) return 0;
    if (sVuIb != NULL && count <= sVuIbCapacity) return 1;
    if (sVuIb != NULL) { sVuIb->lpVtbl->Release(sVuIb); sVuIb = NULL; sVuIbCapacity = 0; }
    hr = sDevice->lpVtbl->CreateIndexBuffer(sDevice,
             count * sizeof(DWORD),
             D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &newIb, NULL);
    if (FAILED(hr)) { d3d9_log_hr("CreateIndexBuffer(VU)", hr); return 0; }
    sVuIb = newIb;
    sVuIbCapacity = count;
    return 1;
}

/* Upload gsVuVerts + gsVuIdx into the GPU buffers for this frame. Called once in frame_end. */
static int d3d9_vu_upload_frame(void) {
    void *ptr = NULL;
    HRESULT hr;
    if (!sVuShaderReady || gsVuVertCount == 0 || gsVuIdxCount == 0) return 0;
    if (!d3d9_vu_vb_ensure(gsVuVertCount)) return 0;
    if (!d3d9_vu_ib_ensure(gsVuIdxCount))  return 0;

    hr = sVuVb->lpVtbl->Lock(sVuVb, 0, gsVuVertCount * (UINT)D3D9_VU_STRIDE, &ptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) { d3d9_log_hr("VB Lock", hr); return 0; }
    memcpy(ptr, gsVuVerts, gsVuVertCount * D3D9_VU_STRIDE);
    sVuVb->lpVtbl->Unlock(sVuVb);

    hr = sVuIb->lpVtbl->Lock(sVuIb, 0, gsVuIdxCount * sizeof(DWORD), &ptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) { d3d9_log_hr("IB Lock", hr); return 0; }
    memcpy(ptr, gsVuIdx, gsVuIdxCount * sizeof(DWORD));
    sVuIb->lpVtbl->Unlock(sVuIb);
    return 1;
}

/* Load Vu0Uniform into VS constant registers c0..c18.
   Vu0Uniform is stored as flat float arrays; the std140 layout is:
     boneA[16], boneB[16], pivotA[4], pivotB[4], screen[16],
     light[4], color0[4], color1[4], misc[4], light2[4]                */
static void d3d9_vu_set_constants(const Vu0Uniform *u) {
    /* Each SetVertexShaderConstantF call: register, pointer, count-of-float4s */
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice,  0, u->boneA,  4);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice,  4, u->boneB,  4);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice,  8, u->pivotA, 1);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice,  9, u->pivotB, 1);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice, 10, u->screen, 4);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice, 14, u->light,  1);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice, 15, u->color0, 1);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice, 16, u->color1, 1);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice, 17, u->misc,   1);
    sDevice->lpVtbl->SetVertexShaderConstantF(sDevice, 18, u->light2, 1);
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

static void d3d9_aux_copy_release(void) {
    if (sD3d9AuxCopy != NULL) {
        d3d9_texture_release(sD3d9AuxCopy);
        sD3d9AuxCopy = NULL;
    }
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
    d3d9_aux_copy_release();
    d3d9_texture_release(sFbTex[0]);
    d3d9_texture_release(sFbTex[1]);
    sFbTex[0] = NULL;
    sFbTex[1] = NULL;
    d3d9_vu_shaders_release();
    if (sD3dCompilerDll != NULL) {
        FreeLibrary(sD3dCompilerDll);
        sD3dCompilerDll = NULL;
        sD3dCompileFn = NULL;
    }
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
    if (sGsVb != NULL) {
        sGsVb->lpVtbl->Release(sGsVb);
        sGsVb = NULL;
    }
    if (sDevice != NULL) {
        sDevice->lpVtbl->Release(sDevice);
        sDevice = NULL;
    }
    if (sD3d != NULL) {
        sD3d->lpVtbl->Release(sD3d);
        sD3d = NULL;
    }
    free(sVuVertices);
    sVuVertices = NULL;
    sVuVertexCapacity = 0;
    free(sGsVertices);
    sGsVertices = NULL;
    sGsVertexCapacity = 0;
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
    /* The game clock is a 30 Hz tick, but the renderer must not free-run its presentation. Present interval one
       gives DX9 the same vblank pacing as the other backends; BT3_D3D9_VSYNC=0 remains an explicit low-latency escape. */
    sD3d9Vsync = getenv("BT3_D3D9_VSYNC") == NULL || atoi(getenv("BT3_D3D9_VSYNC")) != 0;
    sPresent.PresentationInterval = sD3d9Vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

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
                                        D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) != D3D_OK &&
        sD3d->lpVtbl->CheckDeviceFormat(sD3d, D3DADAPTER_DEFAULT, type, sPresent.BackBufferFormat,
                                        D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8) == D3D_OK) {
        sRgbaTextureFormat = D3DFMT_X8R8G8B8;
        fprintf(stderr, "bt3: D3D9: using X8R8G8B8 for RGB textures\n");
    }
    fprintf(stderr, "bt3: D3D9 auxiliary target format A8R8G8B8 (alpha snapshots enabled)\n");
    fprintf(stderr, "bt3: D3D9 device ready (%s vertex processing)\n",
            behavior == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software");
    return 1;
}

static void d3d9_draw_gs(const GsDraw *draw);

static int d3d9_gs_stream_ensure(uint32_t count) {
    IDirect3DVertexBuffer9 *buffer;
    D3d9Vertex *vertices;
    uint32_t capacity = sGsVertexCapacity == 0 ? 4096 : sGsVertexCapacity;
    HRESULT hr;
    if (count <= sGsVertexCapacity && sGsVb != NULL) return 1;
    while (capacity < count) {
        if (capacity > 1048576 / 2) { capacity = 1048576; break; }
        capacity *= 2;
    }
    vertices = (D3d9Vertex *)malloc((size_t)capacity * sizeof(*sGsVertices));
    if (vertices == NULL) return 0;
    hr = sDevice->lpVtbl->CreateVertexBuffer(sDevice, capacity * sizeof(*vertices), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                               D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, D3DPOOL_DEFAULT,
                                               &buffer, NULL);
    if (FAILED(hr)) {
        free(vertices);
        return 0;
    }
    if (sGsVb != NULL) sGsVb->lpVtbl->Release(sGsVb);
    free(sGsVertices);
    sGsVertices = vertices;
    sGsVertexCapacity = capacity;
    sGsVb = buffer;
    return 1;
}

static int d3d9_gs_stream_upload(uint32_t count) {
    void *dst;
    HRESULT hr;
    if (!d3d9_gs_stream_ensure(count)) return 0;
    hr = sGsVb->lpVtbl->Lock(sGsVb, 0, count * sizeof(*sGsVertices), &dst, D3DLOCK_DISCARD);
    if (FAILED(hr)) return 0;
    memcpy(dst, sGsVertices, count * sizeof(*sGsVertices));
    sGsVb->lpVtbl->Unlock(sGsVb);
    return 1;
}

static void d3d9_set_viewport(int width, int height) {
    D3DVIEWPORT9 viewport;
    if (sDevice == NULL || width <= 0 || height <= 0) {
        return;
    }
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = (DWORD)width;
    viewport.Height = (DWORD)height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    sDevice->lpVtbl->SetViewport(sDevice, &viewport);
}

static void d3d9_reset_state_cache(void) {
    sD3d9AppliedPipeline = UINT32_MAX;
    sD3d9AppliedBlendConstant = -1.0f;
    sD3d9ScissorValid = 0;
}

static void d3d9_bind_backbuffer(void) {
    int width, height;
    if (sDevice != NULL && sBackbuffer != NULL) {
        sDevice->lpVtbl->SetRenderTarget(sDevice, 0, sBackbuffer);
        sDevice->lpVtbl->SetDepthStencilSurface(sDevice, NULL);
        if (!SDL_GetWindowSizeInPixels(sWindow, &width, &height)) {
            SDL_GetWindowSize(sWindow, &width, &height);
        }
        d3d9_set_viewport(width, height);
    }
}

static void d3d9_present_target(void) {
    IDirect3DSurface9 *source;
    RECT sourceRect, destRect;
    HRESULT hr;
    int width, height, presentWidth, presentHeight, offsetX, offsetY;
    int presentTarget = sCurrentTarget;
    if (presentTarget < 0 || presentTarget >= MAX_TARGETS || sTargets[presentTarget].color == NULL ||
        sTargets[presentTarget].color->surface == NULL || sBackbuffer == NULL) {
        return;
    }
    if (!SDL_GetWindowSizeInPixels(sWindow, &width, &height)) {
        SDL_GetWindowSize(sWindow, &width, &height);
    }
    presentWidth = width;
    presentHeight = height;
    if ((float)presentWidth > (float)presentHeight * (float)Port_PageAspectMilli() / 1000.0f) {
        presentWidth = (int)((float)presentHeight * (float)Port_PageAspectMilli() / 1000.0f + 0.5f);
    } else {
        presentHeight = (int)((float)presentWidth * 1000.0f / (float)Port_PageAspectMilli() + 0.5f);
    }
    offsetX = (width - presentWidth) / 2;
    offsetY = (height - presentHeight) / 2;
    sourceRect.left = 0;
    sourceRect.top = 0;
    sourceRect.right = 512 * SCALE;
    sourceRect.bottom = 448 * SCALE;
    destRect.left = offsetX;
    destRect.top = offsetY;
    destRect.right = offsetX + presentWidth;
    destRect.bottom = offsetY + presentHeight;
    source = sTargets[presentTarget].color->surface;
    hr = sDevice->lpVtbl->StretchRect(sDevice, source, &sourceRect, sBackbuffer, &destRect, D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        d3d9_log_hr("StretchRect(present)", hr);
    }
    {
            extern volatile int gUiPresentX, gUiPresentY, gUiPresentW, gUiPresentH;
        gUiPresentX = offsetX;
        gUiPresentY = height - offsetY - presentHeight;
        gUiPresentW = presentWidth;
        gUiPresentH = presentHeight;
    }
}

static int d3d9_init(void) {
    HRESULT hr;
    static const DWORD white = 0xFFFFFFFFu;
    D3d9Texture *texture;
    D3DLOCKED_RECT locked;

    d3d9_telemetry_init();
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
    /* Locked A8R8G8B8 memory is BGRA on little-endian Windows; white is unchanged, but keep the
       representation explicit because all RGBA texture uploads are swizzled in d3d9_tex_upload. */
    ((uint8_t *)locked.pBits)[0] = 255;
    ((uint8_t *)locked.pBits)[1] = 255;
    ((uint8_t *)locked.pBits)[2] = 255;
    ((uint8_t *)locked.pBits)[3] = 255;
    texture->texture->lpVtbl->UnlockRect(texture->texture, 0);
    sWhite = texture;
    d3d9_vu_shaders_init();
    /* Init GS pixel shader even when VU shaders failed (also used for non-VU draws). */
    if (!sGsPsReady) d3d9_gs_ps_init();
    fprintf(stderr, "bt3: D3D9 bootstrap backend initialized\n");
    return 1;
}

#ifndef D3D9_BACKEND_PROBE
static int d3d9_fb_upload_texture(int index);
#endif

static void d3d9_frame_end(void) {
    HRESULT hr;
    uint32_t i;
    uint64_t start = GetTickCount64();
    d3d9_telemetry_frame_begin();
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
    sCurrentTarget = -1;
    sD3d9LastTarget = UINT32_MAX;
    sD3d9LastPipeline = UINT32_MAX;
    sD3d9LastTexture = 0;
    sD3d9BoundTexture = UINTPTR_MAX;
    sD3d9AppliedPipeline = UINT32_MAX;
    sD3d9AppliedBlendConstant = -1.0f;
    sD3d9ScissorValid = 0;
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
    /* Upload VU vertices + indices once per frame into persistent GPU buffers. */
    d3d9_vu_upload_frame();
#ifndef D3D9_BACKEND_PROBE
    {
        extern unsigned gGsFbUploads;
        for (i = 0; i < 2; i++) {
            if (gGsFbUploads & (1u << i)) {
                d3d9_fb_upload_texture((int)i);
            }
        }
        gGsFbUploads = 0;
    }
#endif
    for (i = 0; i < gsDrawCount; i++) {
        d3d9_draw_gs(&gsDraws[i]);
    }
    if (sD3d9Telemetry.enabled) {
        sD3d9Telemetry.submitNs = d3d9_now_ns() - sD3d9Telemetry.frameSubmitStartNs;
    }
    /* Restore FVF pipeline so the non-VU path works for the next frame. */
    if (sVuShaderReady) {
        sDevice->lpVtbl->SetVertexShader(sDevice, NULL);
        sDevice->lpVtbl->SetStreamSource(sDevice, 0, NULL, 0, 0);
        sDevice->lpVtbl->SetIndices(sDevice, NULL);
    }
    d3d9_present_target();
    sDevice->lpVtbl->EndScene(sDevice);
    {
        uint64_t presentStart = d3d9_now_ns();
        hr = sDevice->lpVtbl->Present(sDevice, NULL, NULL, NULL, NULL);
        if (sD3d9Telemetry.enabled) sD3d9Telemetry.presentNs = d3d9_now_ns() - presentStart;
    }
    /* Present interval one already blocks on the display vblank. Do not add a second software timer here:
       double pacing makes 30 Hz animation ticks land alternately early and late. */
    if (hr == D3DERR_DEVICELOST) {
        sDeviceLost = 1;
    } else if (FAILED(hr)) {
        d3d9_log_hr("Present", hr);
    }
    if (getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "d3d9: frame draws=%u vu_vertices=%u elapsed=%llums\n", sD3d9DrawCount,
                sD3d9VuVertexCount, (unsigned long long)(GetTickCount64() - start));
    }
    d3d9_telemetry_log_frame(d3d9_now_ns() - sD3d9Telemetry.frameStartNs);
    sD3d9DrawCount = 0;
    sD3d9VuVertexCount = 0;
#ifndef D3D9_BACKEND_PROBE
    /* The shared recorder belongs to the backend contract: every completed frame consumes its list. The SDL GPU
       backend performs this reset here as well; without it D3D9 replays all previous draws and the frame cost grows
       continuously during animation and video playback. */
    for (i = 0; i < (uint32_t)gsTargetCount; i++) {
        gsTargets[i].draws = 0;
    }
    gsVertCount = 0;
    gsVuVertCount = 0;
    gsVuIdxCount = 0;
    gsVuUniCount = 0;
    gsDrawCount = 0;
    gsAnchor = 0;
    gsSkipped = 0;
    gsNative = 0;
#endif
}

static void d3d9_scale_changed(void) {
    int i;
    if (sDevice == NULL) {
        return;
    }
    for (i = 0; i < MAX_TARGETS; i++) {
        d3d9_target_release(&sTargets[i]);
    }
    /* VB/IB are POOL_DEFAULT and must be released before a Reset (targets are rebuilt per-scale). */
    if (sVuVb) { sVuVb->lpVtbl->Release(sVuVb); sVuVb = NULL; sVuVbCapacity = 0; }
    if (sVuIb) { sVuIb->lpVtbl->Release(sVuIb); sVuIb = NULL; sVuIbCapacity = 0; }
    if (sGsVb) { sGsVb->lpVtbl->Release(sGsVb); sGsVb = NULL; sGsVertexCapacity = 0; }
    free(sGsVertices);
    sGsVertices = NULL;
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
    default: return sRgbaTextureFormat;
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
    uint64_t uploadStart = d3d9_now_ns();
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
        uint8_t *dst = (uint8_t *)locked.pBits + (size_t)y * locked.Pitch;
        const uint8_t *src = (const uint8_t *)pixels + (size_t)y * rowBytes;
        if (!blockCompressed && texture->format == GS_TEXFMT_RGBA8) {
            uint32_t x;
            /* Shared pixels are RGBA bytes. D3DFMT_A8R8G8B8 is an ARGB DWORD, hence BGRA bytes in
               little-endian D3D9 locked memory. Keeping this conversion here also covers X8R8G8B8. */
            for (x = 0; x < width; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        } else {
            memcpy(dst, src, rowBytes);
        }
    }
    texture->texture->lpVtbl->UnlockRect(texture->texture, level);
    free(ownedPixels);
    if (sD3d9Telemetry.enabled) sD3d9Telemetry.uploadNs += d3d9_now_ns() - uploadStart;
    return 1;
}

#ifndef D3D9_BACKEND_PROBE
static int d3d9_fb_upload_texture(int index) {
    D3d9Texture *texture;
    D3DLOCKED_RECT locked;
    uint32_t x, y;
    uint32_t fbp = index ? 0x70u : 0u;
    uint32_t *pixels;
    if (index < 0 || index > 1 || sDevice == NULL) {
        return 0;
    }
    texture = sFbTex[index];
    if (texture == NULL) {
        texture = (D3d9Texture *)calloc(1, sizeof(*texture));
        if (texture == NULL || FAILED(sDevice->lpVtbl->CreateTexture(sDevice, GS_W, GS_H, 1, 0, D3DFMT_A8R8G8B8,
                                                                      D3DPOOL_MANAGED, &texture->texture, NULL)) ||
            FAILED(texture->texture->lpVtbl->GetSurfaceLevel(texture->texture, 0, &texture->surface))) {
            if (texture != NULL) {
                if (texture->surface != NULL) texture->surface->lpVtbl->Release(texture->surface);
                if (texture->texture != NULL) texture->texture->lpVtbl->Release(texture->texture);
                free(texture);
            }
            return 0;
        }
        texture->width = GS_W;
        texture->height = GS_H;
        texture->format = GS_TEXFMT_RGBA8;
        sFbTex[index] = texture;
    }
    if (FAILED(texture->texture->lpVtbl->LockRect(texture->texture, 0, &locked, NULL, 0))) {
        return 0;
    }
    pixels = (uint32_t *)locked.pBits;
    for (y = 0; y < GS_H; y++) {
        for (x = 0; x < GS_W; x++) {
            uint32_t c = Gs_VramRead(fbp * 32u, 8, 0, x, y) | 0xFF000000u;
            pixels[y * (locked.Pitch / 4u) + x] = ((c & 0x000000FFu) << 16) | (c & 0x0000FF00u) |
                                                  ((c & 0x00FF0000u) >> 16) | (c & 0xFF000000u);
        }
    }
    texture->texture->lpVtbl->UnlockRect(texture->texture, 0);
    return 1;
}
#endif

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
    uint32_t width = GS_W * (uint32_t)SCALE;
    uint32_t height = GS_H * (uint32_t)SCALE;
    D3d9Target *target;
    HRESULT hr;
    if (index < 0 || index >= MAX_TARGETS || sDevice == NULL || !d3d9_texture_size(&width, &height)) {
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
    hr = sDevice->lpVtbl->CreateTexture(sDevice, width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target->color->texture, NULL);
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
    target->color->width = width;
    target->color->height = height;
    target->color->renderTarget = 1;
    hr = sDevice->lpVtbl->CreateTexture(sDevice, width, height, 1, D3DUSAGE_RENDERTARGET, sAuxFormat, D3DPOOL_DEFAULT, &target->aux->texture, NULL);
    if (FAILED(hr)) {
        d3d9_target_release(target);
        return;
    }
    hr = target->aux->texture->lpVtbl->GetSurfaceLevel(target->aux->texture, 0, &target->aux->surface);
    if (FAILED(hr)) {
        d3d9_target_release(target);
        return;
    }
    target->aux->width = width;
    target->aux->height = height;
    target->aux->renderTarget = 1;
    hr = sDevice->lpVtbl->CreateDepthStencilSurface(sDevice, width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &target->depth, NULL);
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
static int d3d9_target_index_for_draw(int target) { return target >= 0 && target < MAX_TARGETS && sTargets[target].color != NULL ? target : -1; }
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
    d3d9_set_viewport((int)sTargets[index].color->width, (int)sTargets[index].color->height);
    if (!sTargetSeen[index]) {
        HRESULT hr = sDevice->lpVtbl->Clear(sDevice, 0, NULL,
                                             D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                                             D3DCOLOR_ARGB(0, 0, 0, 0), 0.0f, 0);
        if (FAILED(hr)) {
            d3d9_log_hr("Clear(target/depth)", hr);
        }
        sTargetSeen[index] = 1;
    }
    if (sD3d9Telemetry.enabled && sD3d9LastTarget != (uint32_t)index) {
        sD3d9Telemetry.targetChanges++;
        sD3d9LastTarget = (uint32_t)index;
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
    if (sD3d9ScissorValid && memcmp(sD3d9Scissor, scissor, sizeof(sD3d9Scissor)) == 0) {
        return;
    }
    sDevice->lpVtbl->SetScissorRect(sDevice, &rect);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SCISSORTESTENABLE, TRUE);
    memcpy(sD3d9Scissor, scissor, sizeof(sD3d9Scissor));
    sD3d9ScissorValid = 1;
}

static D3DBLEND d3d9_gs_c_factor(int blendColor, int inverse) {
    if (blendColor == 1) return inverse ? D3DBLEND_INVDESTALPHA : D3DBLEND_DESTALPHA;
    if (blendColor >= 2) return inverse ? D3DBLEND_INVBLENDFACTOR : D3DBLEND_BLENDFACTOR;
    return inverse ? D3DBLEND_INVSRCALPHA : D3DBLEND_SRCALPHA;
}

static D3DBLEND d3d9_gs_blend_factor(int k, int blendColor) {
    if (k == 0) return D3DBLEND_ZERO;
    if (k == 1 || k == 5) return D3DBLEND_ONE;
    return d3d9_gs_c_factor(blendColor, k == 4);
}

static void d3d9_set_blend_constant(float value) {
    DWORD c;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    c = (DWORD)(value * 255.0f + 0.5f);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_BLENDFACTOR, D3DCOLOR_ARGB(c, c, c, c));
}

static void d3d9_apply_pipeline(uint32_t key, float blendConstant) {
    uint32_t bkey = key & 0x3FF;
    int pipelineChanged = sD3d9AppliedPipeline != key;
    int blendConstantChanged = sD3d9AppliedBlendConstant != blendConstant;
    if (!pipelineChanged && !blendConstantChanged) {
        return;
    }
    int ztst = (int)((key >> 10) & 3), zwrite = (int)((key >> 12) & 1);
    int wmask = (int)((key >> 16) & 15);
    int enabled = (bkey & 1) != 0;
    int src = (int)((bkey >> 1) & 7), dst = (int)((bkey >> 4) & 7);
    int blendColor = (int)((bkey >> 7) & 3);
    if (sD3d9Telemetry.enabled && sD3d9LastPipeline != key) {
        sD3d9Telemetry.pipelineChanges++;
        sD3d9LastPipeline = key;
    }
    D3DBLEND srcBlend = d3d9_gs_blend_factor(src, blendColor);
    D3DBLEND dstBlend = d3d9_gs_blend_factor(dst, blendColor);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ALPHATESTENABLE, FALSE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ALPHABLENDENABLE, enabled);
    if (enabled) {
        if ((bkey & (1u << 9)) != 0) {
            srcBlend = D3DBLEND_ONE;
        }
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SRCBLEND, srcBlend);
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_DESTBLEND, dstBlend);
        sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_BLENDOP,
                                         ((bkey >> 1) & 7) == 3 ? D3DBLENDOP_REVSUBTRACT :
                                         ((bkey >> 4) & 7) == 3 ? D3DBLENDOP_SUBTRACT : D3DBLENDOP_ADD);
    }
    d3d9_set_blend_constant(blendConstant);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ZENABLE, ztst == 0 ? D3DZB_FALSE : D3DZB_TRUE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ZFUNC, ztst == 1 ? D3DCMP_ALWAYS : ztst == 2 ? D3DCMP_GREATEREQUAL : D3DCMP_GREATER);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_ZWRITEENABLE, zwrite != 0);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_COLORWRITEENABLE, wmask & 15);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_LIGHTING, FALSE);
    sD3d9AppliedPipeline = key;
    sD3d9AppliedBlendConstant = blendConstant;
}

/* ---- CPU-side VU vertex transform helpers -------------------------------------------------- */

/* Multiply a 4x4 matrix (column-major, like GLSL mat4) by a vec4 and return result. */
static void d3d9_mat4_vec4(const float m[16], float x, float y, float z, float w,
                            float *ox, float *oy, float *oz, float *ow) {
    /* column-major: m[col*4+row] */
    *ox = m[0]*x + m[4]*y + m[8]*z  + m[12]*w;
    *oy = m[1]*x + m[5]*y + m[9]*z  + m[13]*w;
    *oz = m[2]*x + m[6]*y + m[10]*z + m[14]*w;
    *ow = m[3]*x + m[7]*y + m[11]*z + m[15]*w;
}

/* Apply the common screen-space projection used by all three VU programs.
   Mirrors the gl_Position formula in vu0.vert / vu4.vert / vu6.vert:
     gl_Position.xy = ((s.x - misc.x*s.w)/512 - s.w,  s.w - (s.y - misc.y*s.w)/512)
     gl_Position.z  = s.z / |misc.z|   (whole-number depth: floor(s.z/s.w)+0.5)/misc.z*s.w)
     gl_Position.w  = s.w
   For XYZRHW (pre-projected), we divide by s.w to get NDC then map to pixels. */
static void d3d9_vu_project(float sx, float sy, float sz, float sw,
                             float offx, float offy, float depthMax,
                             float vpW, float vpH,
                             float *px, float *py, float *pz, float *prhw) {
    float nx, ny, nd;
    if (sw <= 0.0f) { *px = *py = 0.0f; *pz = 0.0f; *prhw = 1.0f; return; }
    /* NDC: x in [-1,1] based on GS 512-wide space, y inverted; z in [0,1]. */
    nx = (sx - offx * sw) / (512.0f * sw) - 1.0f;
    ny = 1.0f - (sy - offy * sw) / (512.0f * sw);
    /* depth: same as GL -- floor(s.z/s.w)+0.5 / depthMax, clamped [0,1] */
    if (depthMax > 0.0f) {
        nd = ((float)(int)(sz / sw) + 0.5f) / depthMax;
        if (nd < 0.0f) nd = 0.0f;
        if (nd > 1.0f) nd = 1.0f;
    } else {
        nd = sz / sw;
        if (nd < 0.0f) nd = 0.0f;
        if (nd > 1.0f) nd = 1.0f;
    }
    /* Convert NDC to screen pixels for XYZRHW.
       D3D9 XYZRHW: x,y are already in screen pixels (pre-divided); z is depth in [0,1]; rhw = 1/w. */
    *px = (nx + 1.0f) * 0.5f * vpW - 0.5f;
    *py = (1.0f - ny) * 0.5f * vpH - 0.5f; /* D3D9 Y=0 is top */
    *pz = nd;
    *prhw = 1.0f / sw;
}

/* Maximum count of VU vertices we will emit per draw call (avoid huge allocations). */
#define D3D9_VU_MAX_VERTS 65535u

/* ---- GPU-side VU draw (vertex shader path) -------------------------------------------------- */
/* Emits a VU draw using the GPU vertex shader.  draw->vu selects vu0(1)/vu4(2)/vu6(3).
   gsVuVerts and gsVuIdx have already been uploaded to sVuVb/sVuIb this frame. */
static void d3d9_draw_vu_gpu(const GsDraw *draw) {
    const Vu0Uniform *u;
    D3d9Texture *texture;
    IDirect3DVertexShader9 *vs;
    UINT startIndex, primCount;
    uint32_t pipeKey;

    if (draw->uniform < 0 || (uint32_t)draw->uniform >= gsVuUniCount) return;
    if (draw->count == 0 || draw->first + draw->count > gsVuIdxCount) return;
    if (draw->target < 0 || draw->target >= MAX_TARGETS) return;
    if (sTargets[draw->target].color == NULL) return;
    if (sVuVb == NULL || sVuIb == NULL) return;

    /* Select shader: vu==1 → vu0, vu==2 → vu4, vu==3 → vu6. */
    vs = sVuShader[draw->vu - 1];
    if (vs == NULL) return;

    u       = &gsVuUni[draw->uniform];
    texture = d3d9_texture_from_handle(draw->tex);

    d3d9_set_target(draw->target);
    pipeKey = draw->pipeline >= 0 && draw->pipeline < sPipeCount ? sPipeKeys[draw->pipeline] : 0;
    d3d9_apply_pipeline(pipeKey, draw->blendc);
    d3d9_set_scissor(draw->scissor);

    sDevice->lpVtbl->SetVertexDeclaration(sDevice, sVuDecl);
    sDevice->lpVtbl->SetVertexShader(sDevice, vs);
    sDevice->lpVtbl->SetStreamSource(sDevice, 0, sVuVb, 0, (UINT)D3D9_VU_STRIDE);
    sDevice->lpVtbl->SetIndices(sDevice, sVuIb);
    d3d9_vu_set_constants(u);

    /* Texture + GS pixel shader (or fixed-function fallback when sGsPsReady == 0). */
    {
        uintptr_t textureHandle = texture != NULL ? (uintptr_t)texture->texture : 0;
        if (sD3d9BoundTexture != textureHandle) {
            sDevice->lpVtbl->SetTexture(sDevice, 0, texture != NULL ? (IDirect3DBaseTexture9 *)texture->texture : NULL);
            sD3d9BoundTexture = textureHandle;
        }
    }
    d3d9_gs_ps_bind(draw);
    if (!sGsPsReady) {
        d3d9_fixed_texture_stage(draw);
        d3d9_fixed_alpha_test(draw);
    }
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MINFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MAGFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSU, (draw->sampler & 2) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSV, (draw->sampler & 4) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SCISSORTESTENABLE, TRUE);

    /* DrawIndexedPrimitive: indices are at draw->first in the IB; vertices span 0..gsVuVertCount-1. */
    startIndex = (UINT)draw->first;
    primCount  = (UINT)(draw->count / 3);
    if (primCount == 0) return;
    sDevice->lpVtbl->DrawIndexedPrimitive(sDevice, D3DPT_TRIANGLELIST,
                                          0,           /* BaseVertexIndex */
                                          0,           /* MinVertexIndex  */
                                          (UINT)gsVuVertCount, /* NumVertices  */
                                          startIndex,  /* StartIndex      */
                                          primCount);  /* PrimitiveCount  */
    sD3d9VuVertexCount += draw->count;
}

/* ---- CPU-side VU draw (fallback when GPU shaders are unavailable) ---------------------------- */
/* Draw a VU-program draw using CPU-side transform.  draw->vu selects the program (1=vu0, 2=vu4, 3=vu6).
   The vertices are read from gsVuVerts indexed by gsVuIdx[first..first+count-1]; each vertex is 12 floats
   (inPos xyzw, inNormal/inColor xyzw, inSt xyzw, matching the GLSL layout(location=0..2) inputs). */
static void d3d9_draw_vu(const GsDraw *draw) {
    const Vu0Uniform *u;
    D3d9Texture *texture;
    D3d9Vertex *vertices;
    uint32_t i, count;
    float vpW, vpH;

    if (draw->uniform < 0 || (uint32_t)draw->uniform >= gsVuUniCount) return;
    if (draw->count == 0 || draw->count > D3D9_VU_MAX_VERTS) return;
    if (draw->target < 0 || draw->target >= MAX_TARGETS) return;
    if (sTargets[draw->target].color == NULL) return;

    u = &gsVuUni[draw->uniform];
    texture = d3d9_texture_from_handle(draw->tex);
    count = draw->count;

    vpW = (float)sTargets[draw->target].color->width;
    vpH = (float)sTargets[draw->target].color->height;

    if (sVuVertexCapacity < count) {
        D3d9Vertex *grown = (D3d9Vertex *)realloc(sVuVertices, (size_t)count * sizeof(*sVuVertices));
        if (grown == NULL) return;
        sVuVertices = grown;
        sVuVertexCapacity = count;
    }
    vertices = sVuVertices;
    sD3d9VuVertexCount += count;

    for (i = 0; i < count; i++) {
        /* Look up the vertex through the index buffer. */
        uint32_t idx = gsVuIdx[draw->first + i];
        const float *v = &gsVuVerts[idx * 12]; /* 12 floats = 3 × vec4 */
        float inPx = v[0], inPy = v[1], inPz = v[2], inPw = v[3];
        float inNx = v[4], inNy = v[5], inNz = v[6], inNa = v[7];
        float inSs = v[8], inSt = v[9]; /* inSt.z is usually 1; inSt.w unused */
        float sx, sy, sz, sw;
        float cr, cg, cb, ca;
        float us, ut;

        if (draw->vu == 2) {
            /* vu4.vert: screen = M * inPos; colour from inColor (0..255 floats); tex = inSt.xy */
            d3d9_mat4_vec4(u->screen, inPx, inPy, inPz, inPw, &sx, &sy, &sz, &sw);
            cr = inNx / 255.0f; cg = inNy / 255.0f; cb = inNz / 255.0f; ca = inNa / 255.0f;
            if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
            if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
            if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
            if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
            us = inSs; ut = inSt;
        } else if (draw->vu == 3) {
            /* vu6.vert: screen = M * p; st from shadow camera. Uniform layout matches vu6.vert.
               boneA slot = shadowCam (0..15), screen slot = screen (14..17 = index 56..71 bytes).
               In Vu0Uniform: boneA[16], boneB[16], pivotA[4], pivotB[4], screen[16] ...
               vu6 uses: shadowCam=boneA, scale=pivotA, texScale=pivotB, screen=screen */
            float cx, cy, cz, cw;
            float stx, sty;
            float scale = u->pivotA[0]; /* u->scale.x */
            float texSx = u->pivotB[0], texSy = u->pivotB[1]; /* u->texScale.xy */
            d3d9_mat4_vec4(u->screen, inPx, inPy, inPz, 1.0f, &sx, &sy, &sz, &sw);
            d3d9_mat4_vec4(u->boneA,  inPx, inPy, inPz, 1.0f, &cx, &cy, &cz, &cw);
            stx = (cx * scale + 1.0f) * 0.5f * texSx;
            sty = (cy * scale + 1.0f) * 0.5f * texSy;
            cr = inNx / 255.0f; cg = inNy / 255.0f; cb = inNz / 255.0f; ca = inNa / 255.0f;
            if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
            if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
            if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
            if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
            us = stx; ut = sty;
        } else {
            /* vu0.vert (draw->vu == 1): two-bone skinning.
               misc.w selects the sub-mode (layer / program variant). */
            float miscW = u->misc[3];
            float w = (miscW == 4.0f) ? 1.0f : inPw; /* layer 4: 4th word is a flag, not weight */
            float pax, pay, paz, paw, pbx, pby, pbz, pbw;
            float blendX, blendY, blendZ;
            /* pa = boneA * (pos - pivotA) */
            d3d9_mat4_vec4(u->boneA,
                           inPx - u->pivotA[0], inPy - u->pivotA[1], inPz - u->pivotA[2], 1.0f,
                           &pax, &pay, &paz, &paw);
            /* pb = boneB * (pos - pivotB) */
            d3d9_mat4_vec4(u->boneB,
                           inPx - u->pivotB[0], inPy - u->pivotB[1], inPz - u->pivotB[2], 1.0f,
                           &pbx, &pby, &pbz, &pbw);
            /* blended = mix(pb, pa, w) = pb + w*(pa-pb) */
            blendX = pbx + w * (pax - pbx);
            blendY = pby + w * (pay - pby);
            blendZ = pbz + w * (paz - pbz);
            d3d9_mat4_vec4(u->screen, blendX, blendY, blendZ, 1.0f, &sx, &sy, &sz, &sw);
            /* programs 2a/2b: shadow cam, flip if w < 0 */
            if (miscW == 2.0f && sw < 0.0f) { sx = -sx; sy = -sy; sz = -sz; sw = -sw; }
            /* colour and UV based on miscW sub-mode */
            if (miscW >= 3.0f) {
                /* programs 7 and 8: vertex colour in inNormal slot */
                cr = inNx / 255.0f; cg = inNy / 255.0f; cb = inNz / 255.0f;
                ca = (miscW == 4.0f) ? (u->color0[3] / 255.0f) : (inNa / 255.0f);
                if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
                if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
                if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
                if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
                us = inSs; ut = inSt;
            } else if (miscW == 5.0f) {
                /* program 1 layer 1: toon UV = 0.5 + 0.5*(light dot normal) */
                float n1x = inNx, n1y = inNy, n1z = inNz;
                float dotU = u->light[0]*n1x + u->light[1]*n1y + u->light[2]*n1z + u->light[3];
                float dotV = u->light2[0]*n1x + u->light2[1]*n1y + u->light2[2]*n1z + u->light2[3];
                cr = u->color1[0] / 255.0f; cg = u->color1[1] / 255.0f;
                cb = u->color1[2] / 255.0f; ca = u->color1[3] / 255.0f;
                if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
                if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
                if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
                if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
                us = dotU * 0.5f + 0.5f; ut = dotV * 0.5f + 0.5f;
            } else if (miscW == 1.0f) {
                /* layer 1 toon (single-bone normal dot light) */
                float na, nb;
                /* mat3(boneA) * inNormal */
                float bnx = u->boneA[0]*inNx + u->boneA[4]*inNy + u->boneA[8]*inNz;
                float bny = u->boneA[1]*inNx + u->boneA[5]*inNy + u->boneA[9]*inNz;
                float bnz = u->boneA[2]*inNx + u->boneA[6]*inNy + u->boneA[10]*inNz;
                float anx = u->boneA[0]*inNx + u->boneA[4]*inNy + u->boneA[8]*inNz;
                float any = u->boneA[1]*inNx + u->boneA[5]*inNy + u->boneA[9]*inNz;
                float anz = u->boneA[2]*inNx + u->boneA[6]*inNy + u->boneA[10]*inNz;
                na = u->light[0]*anx + u->light[1]*any + u->light[2]*anz;
                nb = u->light[0]*bnx + u->light[1]*bny + u->light[2]*bnz;
                cr = u->color1[0] / 255.0f; cg = u->color1[1] / 255.0f;
                cb = u->color1[2] / 255.0f; ca = u->color1[3] / 255.0f;
                if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
                if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
                if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
                if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
                us = ((w > 0.5f ? na : nb) + u->light[3]) * 0.5f + 0.5f;
                ut = 0.0f;
            } else {
                /* layer 0: material colour from uniform */
                cr = u->color0[0] / 255.0f; cg = u->color0[1] / 255.0f;
                cb = u->color0[2] / 255.0f; ca = u->color0[3] / 255.0f;
                if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
                if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
                if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
                if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
                us = inSs; ut = inSt;
            }
        }

        /* Project to XYZRHW screen pixels. */
        d3d9_vu_project(sx, sy, sz, sw,
                        u->misc[0], u->misc[1], u->misc[2],
                        vpW, vpH,
                        &vertices[i].x, &vertices[i].y, &vertices[i].z, &vertices[i].rhw);
        vertices[i].color = D3DCOLOR_ARGB(
            (int)(ca * 255.0f + 0.5f),
            (int)(cr * 255.0f + 0.5f),
            (int)(cg * 255.0f + 0.5f),
            (int)(cb * 255.0f + 0.5f));
        vertices[i].u = us;
        vertices[i].v = ut;
    }

    d3d9_set_target(draw->target);
    d3d9_apply_pipeline(draw->pipeline >= 0 && draw->pipeline < sPipeCount ? sPipeKeys[draw->pipeline] : 0, draw->blendc);
    d3d9_set_scissor(draw->scissor);

    sDevice->lpVtbl->SetFVF(sDevice, D3D9_FVF_TEXTURED);
    sDevice->lpVtbl->SetVertexShader(sDevice, NULL); /* FVF path: no VS */
    {
        uintptr_t textureHandle = texture != NULL ? (uintptr_t)texture->texture : 0;
        if (sD3d9BoundTexture != textureHandle) {
            sDevice->lpVtbl->SetTexture(sDevice, 0, texture != NULL ? (IDirect3DBaseTexture9 *)texture->texture : NULL);
            sD3d9BoundTexture = textureHandle;
        }
    }
    d3d9_gs_ps_bind(draw);
    if (!sGsPsReady) {
        d3d9_fixed_texture_stage(draw);
        d3d9_fixed_alpha_test(draw);
    }
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MINFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MAGFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSU, (draw->sampler & 2) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSV, (draw->sampler & 4) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SCISSORTESTENABLE, TRUE);

    /* Emit in batches of D3D9_VU_MAX_VERTS to stay within 16-bit index limits of DrawPrimitiveUP. */
    {
        uint32_t offset = 0;
        while (offset < count) {
            uint32_t batch = count - offset;
            uint32_t triCount;
            if (batch > D3D9_VU_MAX_VERTS) batch = D3D9_VU_MAX_VERTS;
            triCount = batch / 3;
            if (triCount == 0) break;
            sDevice->lpVtbl->DrawPrimitiveUP(sDevice, D3DPT_TRIANGLELIST, triCount,
                                             vertices + offset, sizeof(D3d9Vertex));
            offset += triCount * 3;
        }
    }
}

static void d3d9_draw_gs(const GsDraw *draw) {
    uint32_t i;
    if (sD3d9Telemetry.enabled) {
        sD3d9Telemetry.draws++;
        if (draw->native) sD3d9Telemetry.nativeDraws++;
        else if (draw->vu) sD3d9Telemetry.vuDraws++;
        else sD3d9Telemetry.draw2d++;
        sD3d9Telemetry.vertices += draw->count;
        if (draw->vu) sD3d9Telemetry.indices += draw->count;
        if (sD3d9LastTexture != draw->tex) {
            sD3d9Telemetry.textureChanges++;
            sD3d9LastTexture = draw->tex;
        }
    }
    sD3d9DrawCount++;
    D3d9Texture *texture = d3d9_texture_from_handle(draw->tex);
    D3d9Vertex *vertices;
    uint32_t count = draw->count;
    if (draw->native >= 100) {
        int fb = draw->native - 100;
        if (fb >= 0 && fb < 2 && sFbTex[fb] != NULL && draw->target >= 0 && draw->target < MAX_TARGETS &&
            sTargets[draw->target].color != NULL && sTargets[draw->target].color->surface != NULL) {
            RECT sourceRect = {0, 0, GS_W, GS_H};
            RECT targetRect = {0, 0, (LONG)sTargets[draw->target].color->width, (LONG)sTargets[draw->target].color->height};
            d3d9_set_target(draw->target);
            sDevice->lpVtbl->StretchRect(sDevice, sFbTex[fb]->surface, &sourceRect,
                                         sTargets[draw->target].color->surface, &targetRect, D3DTEXF_LINEAR);
#ifndef D3D9_BACKEND_PROBE
            gsTargets[draw->target].cleared = 1;
#endif
        }
        return;
    }
    if (draw->native || draw->target < 0 || draw->target >= MAX_TARGETS || count == 0) {
        return;
    }
    if (draw->vu) {
        if (sVuShaderReady) {
            d3d9_draw_vu_gpu(draw); /* GPU vertex shader path */
        } else if (count <= D3D9_VU_MAX_VERTS) {
            d3d9_draw_vu(draw);     /* CPU fallback */
        }
        return;
    }
    if (count > 65535) {
        return;
    }
    d3d9_set_target(draw->target);
    d3d9_apply_pipeline(draw->pipeline >= 0 && draw->pipeline < sPipeCount ? sPipeKeys[draw->pipeline] : 0, draw->blendc);
    d3d9_set_scissor(draw->scissor);
    vertices = (D3d9Vertex *)malloc((size_t)count * sizeof(*vertices));
    if (vertices == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        const Vtx *source = &gsVerts[draw->first + i];
        /* GS alpha: 0x80 = opaque (k = 255/128).  Pass the raw [0..128] value scaled to [0..255]
           so the GS pixel shader can apply its own k multiply (a * k in PS).  When the PS is not
           available we clamp the old way. */
        int av;
        if (sGsPsReady) {
            /* Deliver a / 128 into the 0..1 D3D vertex colour alpha so that PS sees vColor.a = a/128. */
            av = (int)(source->a * 2);      /* a/128 * 255 ≈ a * 1.992; close enough, PS multiplies by k */
            if (av > 255) av = 255;
        } else {
            av = source->a > 127 ? 255 : source->a * 2; /* legacy: clamp to 255 at 0x80 */
        }
        vertices[i].x = source->x * (float)SCALE - 0.5f;
        vertices[i].y = source->y * (float)SCALE - 0.5f;
        vertices[i].z = source->z;
        vertices[i].rhw = 1.0f;
        vertices[i].color = D3DCOLOR_ARGB(av, source->r, source->g, source->b);
        vertices[i].u = source->s;
        vertices[i].v = source->t;
    }
    if (!d3d9_gs_stream_ensure(count)) {
        free(vertices);
        return;
    }
    memcpy(sGsVertices, vertices, (size_t)count * sizeof(*vertices));
    free(vertices);
    if (!d3d9_gs_stream_upload(count)) {
        return;
    }
    sDevice->lpVtbl->SetFVF(sDevice, D3D9_FVF_TEXTURED);
    sDevice->lpVtbl->SetVertexShader(sDevice, NULL); /* FVF path */
    sDevice->lpVtbl->SetStreamSource(sDevice, 0, sGsVb, 0, sizeof(*sGsVertices));
    {
        uintptr_t textureHandle = texture != NULL ? (uintptr_t)texture->texture : 0;
        if (sD3d9BoundTexture != textureHandle) {
            sDevice->lpVtbl->SetTexture(sDevice, 0, texture != NULL ? (IDirect3DBaseTexture9 *)texture->texture : NULL);
            sD3d9BoundTexture = textureHandle;
        }
    }
    d3d9_gs_ps_bind(draw);
    if (!sGsPsReady) {
        d3d9_fixed_texture_stage(draw);
        d3d9_fixed_alpha_test(draw);
    }
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MINFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_MAGFILTER, (draw->sampler & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSU, (draw->sampler & 2) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetSamplerState(sDevice, 0, D3DSAMP_ADDRESSV, (draw->sampler & 4) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    sDevice->lpVtbl->SetRenderState(sDevice, D3DRS_SCISSORTESTENABLE, TRUE);
    sDevice->lpVtbl->DrawPrimitive(sDevice, draw->count == 2 ? D3DPT_LINELIST : D3DPT_TRIANGLELIST,
                                   0, draw->count == 2 ? 1 : draw->count / 3);
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
            const uint8_t *src = row + (size_t)(x * SCALE + SCALE / 2) * 4;
            uint8_t *dst = rgba + (y * 512 + x) * 4;
            /* Convert D3D9's little-endian BGRA readback back to the backend contract's RGBA. */
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
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
