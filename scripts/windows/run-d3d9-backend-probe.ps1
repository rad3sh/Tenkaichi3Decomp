[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'environment.ps1')

$vsdev = Get-Bt3VsDevCmd
if ($null -eq $vsdev) { throw 'VsDevCmd.bat was not found.' }
$sdk = $Bt3Sdl3Sdk
$sdlInclude = Join-Path $sdk 'include'
$sdlLib = Join-Path $sdk "lib\$Architecture"
if (-not (Test-Path (Join-Path $sdlInclude 'SDL3\SDL.h'))) { throw "SDL3 SDK not found at $sdk" }
if (-not (Test-Path (Join-Path $sdlLib 'SDL3.lib'))) { throw "SDL3.lib not found at $sdlLib" }

New-Item -ItemType Directory -Force -Path $Bt3BuildRoot | Out-Null
$source = Join-Path $Bt3BuildRoot 'd3d9_backend_probe.c'
$backendSource = Join-Path $Bt3RepoRoot 'port\src\gs\gs_d3d9.c'
$probe = Join-Path $Bt3BuildRoot "d3d9_backend_probe-$Architecture.exe"
$backendObject = Join-Path $Bt3BuildRoot "gs_d3d9-probe-$Architecture.obj"
$probeObject = Join-Path $Bt3BuildRoot "d3d9_backend_probe-$Architecture.obj"
$symbols = Join-Path $Bt3BuildRoot "d3d9_backend_probe-$Architecture.pdb"
$log = New-Bt3Log "d3d9-backend-probe-$Architecture"

@'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gs_draw.h"

SDL_Window *GsDraw_WindowCreate(int opengl) {
    SDL_PropertiesID props;
    (void)opengl;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return NULL;
    }
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "D3D9 backend probe");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 640);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, false);
    {
        SDL_Window *window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        return window;
    }
}

int gsScale = 1;
int Port_PageAspectMilli(void) { return 1143; }
const char *Port_GetEnv(const char *name) { return getenv(name); }
volatile int gUiPresentX, gUiPresentY, gUiPresentW, gUiPresentH;
GsDraw *gsDraws = NULL;
uint32_t gsDrawCount = 0;
Vtx *gsVerts = NULL;
float *gsVuVerts = NULL;
uint32_t *gsVuIdx = NULL;
Vu0Uniform *gsVuUni = NULL;
uint32_t gsVuUniCount = 0;
uint32_t gsVuVertCount = 0;
uint32_t gsVuIdxCount = 0;

static int write_ppm(const char *path, const unsigned char *rgba, int width, int height) {
    FILE *file = fopen(path, "wb");
    int y, x;
    if (file == NULL) {
        return 0;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            fwrite(rgba + (y * width + x) * 4, 1, 3, file);
        }
    }
    fclose(file);
    return 1;
}

static int near_byte(int actual, int expected) {
    return actual >= expected - 4 && actual <= expected + 4;
}

static int pixel_matches(const unsigned char *rgba, int x, int y, int r, int g, int b) {
    const unsigned char *pixel = rgba + (y * 512 + x) * 4;
    return near_byte(pixel[0], r) && near_byte(pixel[1], g) && near_byte(pixel[2], b);
}

static void fill_texture(unsigned char *pixels, int width, int height) {
    int x, y;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned char *pixel = pixels + (y * width + x) * 4;
            pixel[0] = (unsigned char)(x * 255 / (width - 1));
            pixel[1] = (unsigned char)(y * 255 / (height - 1));
            pixel[2] = 32;
            pixel[3] = 255;
        }
    }
}

static void fill_solid(unsigned char *pixels, int width, int height, int r, int g, int b, int a) {
    int x, y;
    for (y = 0; y < height; y++) for (x = 0; x < width; x++) {
        unsigned char *pixel = pixels + (y * width + x) * 4;
        pixel[0] = (unsigned char)r; pixel[1] = (unsigned char)g;
        pixel[2] = (unsigned char)b; pixel[3] = (unsigned char)a;
    }
}

int main(void) {
    static GsDraw draws[2];
    static Vtx vertices[3] = {
        {120.0f, 120.0f, 0.5f, 255, 80, 80, 255, 0.0f, 0.0f, 1.0f},
        {520.0f, 140.0f, 0.5f, 80, 230, 110, 255, 1.0f, 0.0f, 1.0f},
        {320.0f, 420.0f, 0.5f, 80, 120, 240, 255, 0.5f, 1.0f, 1.0f}
    };
    /* The GS uses 0x80 as the neutral colour scale. The ps_3_0 path applies 255/128 once; this probe
       therefore checks both channel order and the fixed GS brightness convention. */
    static Vtx texturedVertices[3] = {
        {120.0f, 100.0f, 0.4f, 128, 128, 128, 128, 0.0f, 0.0f, 1.0f},
        {520.0f, 100.0f, 0.4f, 128, 128, 128, 128, 1.0f, 0.0f, 1.0f},
        {320.0f, 400.0f, 0.4f, 128, 128, 128, 128, 0.5f, 1.0f, 1.0f}
    };
    unsigned char *readback;
    unsigned char *copyReadback;
    unsigned char *texturePixels;
    unsigned char bc1[8] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    GsTex gradientTexture, compressedTexture, solidTexture;
    int center, corner;
    unsigned char *solidPixels;
    if (!sD3d9Backend.init()) {
        return 1;
    }
    sD3d9Backend.targetEnsure(0);
    memset(&draws[0], 0, sizeof(draws[0]));
    draws[0].first = 0;
    draws[0].count = 3;
    draws[0].target = 0;
    draws[0].pipeline = sD3d9Backend.pipeGet((1u << 10) | (1u << 12) | (15u << 16));
    draws[0].sampler = 1;
    draws[0].tex = sD3d9Backend.whiteTex();
    draws[0].scissor[0] = 0;
    draws[0].scissor[1] = 0;
    draws[0].scissor[2] = 1024;
    draws[0].scissor[3] = 1024;
    gsDraws = draws;
    gsVerts = vertices;
    gsDrawCount = 1;
    sD3d9Backend.frameEnd();
    readback = (unsigned char *)malloc(512 * 448 * 4);
    if (readback == NULL || !sD3d9Backend.targetRead(0, readback)) {
        free(readback);
        return 1;
    }
    fprintf(stderr, "D3D9 first draw center=%u,%u,%u\n", readback[(320 * 512 + 320) * 4], readback[(320 * 512 + 320) * 4 + 1], readback[(320 * 512 + 320) * 4 + 2]);
    free(readback);
    readback = NULL;
    texturePixels = (unsigned char *)malloc(64 * 64 * 4);
    if (texturePixels == NULL) {
        return 1;
    }
    fill_texture(texturePixels, 64, 64);
    gradientTexture = sD3d9Backend.texCreate(64, 64, GS_TEXFMT_RGBA8, 1);
    if (gradientTexture == 0 || !sD3d9Backend.texUpload(gradientTexture, 0, 64, 64, GS_TEXFMT_RGBA8, texturePixels, 64 * 64 * 4)) {
        free(texturePixels);
        return 1;
    }
    memset(&draws[1], 0, sizeof(draws[1]));
    draws[1].first = 0;
    draws[1].count = 3;
    draws[1].target = 0;
    draws[1].pipeline = sD3d9Backend.pipeGet((1u << 10) | (1u << 12) | (15u << 16));
    draws[1].sampler = 1;
    draws[1].tex = gradientTexture;
    draws[1].scissor[0] = 0;
    draws[1].scissor[1] = 0;
    draws[1].scissor[2] = 1024;
    draws[1].scissor[3] = 1024;
    /* mode[0]=1 (textured from GS memory), mode[1]=0 (TFX MODULATE), mode[2]=1 (TCC: use tex alpha),
       mode[3]=0 (no alpha test). Matches what gs_draw.c records for a normal textured primitive. */
    draws[1].mode[0] = 1;
    draws[1].mode[1] = 0;
    draws[1].mode[2] = 1;
    draws[1].mode[3] = 0;
    /* orig[2]=1.0 means max original alpha = 1.0 (standard non-replacement texture). */
    draws[1].orig[2] = 1.0f;
    gsVerts = texturedVertices;
    gsDrawCount = 2;
    sD3d9Backend.frameEnd();
    readback = (unsigned char *)malloc(512 * 448 * 4);
    if (readback == NULL || !sD3d9Backend.targetRead(0, readback)) {
        free(readback);
        return 1;
    }
    center = (320 * 512 + 320) * 4;
    corner = 0;
    if (!pixel_matches(readback, 320, 320, 127, 188, 32) ||
        !near_byte(readback[corner], 0) || !near_byte(readback[corner + 1], 0) || !near_byte(readback[corner + 2], 0)) {
        fprintf(stderr, "D3D9 visual assertions failed: center=%u,%u,%u corner=%u,%u,%u\n",
                readback[center], readback[center + 1], readback[center + 2], readback[corner], readback[corner + 1], readback[corner + 2]);
        free(readback);
        return 1;
    }
    copyReadback = (unsigned char *)malloc(512 * 448 * 4);
    if (copyReadback == NULL) {
        free(readback);
        return 1;
    }
    sD3d9Backend.targetEnsure(1);
    sD3d9Backend.targetCopy(0, 1);
    if (!sD3d9Backend.targetRead(1, copyReadback)) {
        free(copyReadback);
        free(readback);
        return 1;
    }
    if (!pixel_matches(copyReadback, 320, 320, 127, 188, 32)) {
        free(copyReadback);
        free(readback);
        return 1;
    }
    free(copyReadback);
    solidPixels = (unsigned char *)malloc(4 * 4 * 4);
    if (solidPixels == NULL) return 1;
    fill_solid(solidPixels, 4, 4, 255, 220, 0, 255);
    solidTexture = sD3d9Backend.texCreate(4, 4, GS_TEXFMT_RGBA8, 1);
    if (solidTexture == 0 || !sD3d9Backend.texUpload(solidTexture, 0, 4, 4, GS_TEXFMT_RGBA8, solidPixels, 4 * 4 * 4)) return 1;
    fprintf(stderr, "D3D9 solid yellow texture uploaded: handle=%p\\n", (void *)(uintptr_t)solidTexture);
    sD3d9Backend.texDestroy(solidTexture);
    compressedTexture = sD3d9Backend.texCreate(4, 4, GS_TEXFMT_BC1, 1);
    {
        unsigned char *compressedPixels = (unsigned char *)malloc(sizeof(bc1));
        if (compressedPixels == NULL) {
            return 1;
        }
        memcpy(compressedPixels, bc1, sizeof(bc1));
        if (compressedTexture == 0 || !sD3d9Backend.texUpload(compressedTexture, 0, 4, 4, GS_TEXFMT_BC1, compressedPixels, sizeof(bc1))) {
            if (compressedTexture != 0) {
                sD3d9Backend.texDestroy(compressedTexture);
            }
            return 1;
        }
    }
    sD3d9Backend.texDestroy(compressedTexture);
    if (!write_ppm("build/windows/d3d9_backend_probe.ppm", readback, 512, 448)) {
        free(readback);
        return 1;
    }
    printf("D3D9 visual assertions passed: center=%u,%u,%u corner=%u,%u,%u\n",
           readback[center], readback[center + 1], readback[center + 2], readback[corner], readback[corner + 1], readback[corner + 2]);
    free(readback);
    return 0;
}
'@ | Set-Content -LiteralPath $source -Encoding ASCII

$command = "call `"$vsdev`" -arch=$Architecture -host_arch=x64 >nul && cl /nologo /W4 /WX /D_WIN32 /D_CRT_SECURE_NO_WARNINGS /I`"$sdlInclude`" /I`"$Bt3RepoRoot\port\src\gs`" /Zi /FS /c `"$backendSource`" /Fo:`"$backendObject`" && cl /nologo /W4 /WX /D_WIN32 /D_CRT_SECURE_NO_WARNINGS /I`"$sdlInclude`" /I`"$Bt3RepoRoot\port\src\gs`" /Zi /FS /c `"$source`" /Fo:`"$probeObject`" && link /DEBUG /PDB:`"$symbols`" /OUT:`"$probe`" `"$backendObject`" `"$probeObject`" d3d9.lib user32.lib gdi32.lib dxguid.lib SDL3.lib /LIBPATH:`"$sdlLib`""
Write-Output "Probe log: $log"
$buildOutput = cmd.exe /d /s /c $command 2>&1
$buildExit = $LASTEXITCODE
$buildOutput | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding UTF8
Get-Content -LiteralPath $log
if ($buildExit -ne 0) { throw "D3D9 backend probe compilation failed with exit code $buildExit" }
Copy-Item -LiteralPath (Join-Path $sdlLib 'SDL3.dll') -Destination (Join-Path $Bt3BuildRoot 'SDL3.dll') -Force
$runCommand = "`"$probe`" >> `"$log`" 2>&1"
cmd.exe /d /s /c $runCommand
if ($LASTEXITCODE -ne 0) { throw "D3D9 backend probe returned exit code $LASTEXITCODE" }
Get-Content -LiteralPath $log -Tail 20
Write-Output "D3D9 backend probe passed: $probe"
