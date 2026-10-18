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
if (-not (Test-Path (Join-Path $sdk 'include\SDL3\SDL.h'))) {
    throw "SDL3 development SDK not found at $sdk. Run bootstrap or download the official VC archive."
}
$source = Join-Path $Bt3BuildRoot 'd3d9_bootstrap.c'
$output = Join-Path $Bt3BuildRoot "d3d9_bootstrap-$Architecture.exe"
$object = Join-Path $Bt3BuildRoot "d3d9_bootstrap-$Architecture.obj"
$symbols = Join-Path $Bt3BuildRoot "d3d9_bootstrap-$Architecture.pdb"
$log = New-Bt3Log "build-d3d9-bootstrap-$Architecture"
New-Item -ItemType Directory -Force -Path $Bt3BuildRoot | Out-Null

@'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <SDL3/SDL.h>
#include <stdio.h>

int main(void) {
    SDL_Window *window;
    SDL_PropertiesID props;
    void *hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS present;
    HRESULT hr;
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "D3D9 bootstrap");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 640);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window == NULL) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (hwnd == NULL || d3d == NULL) { fprintf(stderr, "D3D9 bootstrap handle/device API unavailable\n"); if (d3d) d3d->lpVtbl->Release(d3d); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    ZeroMemory(&present, sizeof(present));
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_X8R8G8B8;
    present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &device);
    if (FAILED(hr)) { fprintf(stderr, "CreateDevice failed: 0x%08lX\n", (unsigned long)hr); d3d->lpVtbl->Release(d3d); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    device->lpVtbl->Clear(device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(12, 20, 32), 1.0f, 0);
    hr = device->lpVtbl->Present(device, NULL, NULL, NULL, NULL);
    printf("D3D9 bootstrap presented successfully (hr=0x%08lX)\n", (unsigned long)hr);
    device->lpVtbl->Release(device);
    d3d->lpVtbl->Release(d3d);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return SUCCEEDED(hr) ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ASCII

$sdlInclude = Join-Path $sdk 'include'
$sdlLib = Join-Path $sdk "lib\$Architecture"
$sdlRuntime = Join-Path $sdlLib 'SDL3.dll'
if (-not (Test-Path (Join-Path $sdlLib 'SDL3.lib'))) { throw "SDL3.lib not found at $sdlLib" }
$runtimeOutput = Join-Path $Bt3BuildRoot 'SDL3.dll'
$command = "call `"$vsdev`" -arch=$Architecture -host_arch=x64 >nul && cl /nologo /W4 /WX /FS /Zi /EHsc /I`"$sdlInclude`" /Fd:`"$symbols`" /c `"$source`" /Fo:`"$object`" && link /DEBUG /PDB:`"$symbols`" /OUT:`"$output`" `"$object`" d3d9.lib user32.lib gdi32.lib dxguid.lib SDL3.lib /LIBPATH:`"$sdlLib`""
Write-Output "Build log: $log"
$buildOutput = cmd.exe /d /s /c $command 2>&1
$buildExit = $LASTEXITCODE
$buildOutput | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding UTF8
Get-Content -LiteralPath $log
if ($buildExit -ne 0) { throw "D3D9 bootstrap compilation failed with exit code $buildExit" }
Copy-Item -LiteralPath $sdlRuntime -Destination $runtimeOutput -Force
Write-Output "D3D9 bootstrap built: $output"
