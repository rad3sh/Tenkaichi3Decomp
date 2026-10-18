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
New-Item -ItemType Directory -Force -Path $Bt3BuildRoot | Out-Null
$log = New-Bt3Log "build-probe-$Architecture"
$source = Join-Path $Bt3BuildRoot 'd3d9_probe.c'
$object = Join-Path $Bt3BuildRoot "d3d9_probe-$Architecture.obj"
$symbols = Join-Path $Bt3BuildRoot "d3d9_probe-$Architecture.pdb"
$output = Join-Path $Bt3BuildRoot "d3d9_probe-$Architecture.exe"

@'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

int main(void) {
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d != NULL) d3d->lpVtbl->Release(d3d);
    return d3d != NULL ? 0 : 1;
}
'@ | Set-Content -LiteralPath $source -Encoding ASCII

$command = "call `"$vsdev`" -arch=$Architecture -host_arch=x64 >nul && cl /nologo /W4 /WX /FS /Zi /Fd:`"$symbols`" /c `"$source`" /Fo:`"$object`" && link /DEBUG /PDB:`"$symbols`" /OUT:`"$output`" `"$object`" d3d9.lib"
Write-Output "Build log: $log"
cmd.exe /d /s /c $command 2>&1 | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0) { throw "D3D9 probe compilation failed with exit code $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $output)) { throw 'D3D9 probe output was not created.' }
& $output
if ($LASTEXITCODE -ne 0) { throw "D3D9 probe returned exit code $LASTEXITCODE" }
Write-Output "D3D9 $Architecture probe passed: $output"
