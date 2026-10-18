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
if (-not (Test-Path (Join-Path $Bt3Sdl3Sdk 'include\SDL3\SDL.h'))) {
    throw "SDL3 development SDK not found at $Bt3Sdl3Sdk. Run bootstrap.ps1 first."
}
$source = Join-Path $Bt3RepoRoot 'port\src\gs\gs_d3d9.c'
$object = Join-Path $Bt3BuildRoot "gs_d3d9-$Architecture.obj"
$log = New-Bt3Log "build-d3d9-backend-$Architecture"
New-Item -ItemType Directory -Force -Path $Bt3BuildRoot | Out-Null

$sdlInclude = Join-Path $Bt3Sdl3Sdk 'include'
$command = "call `"$vsdev`" -arch=$Architecture -host_arch=x64 >nul && cl /nologo /W4 /WX /D_WIN32 /I`"$sdlInclude`" /c `"$source`" /Fo:`"$object`""
Write-Output "Build log: $log"
$buildOutput = cmd.exe /d /s /c $command 2>&1
$buildExit = $LASTEXITCODE
$buildOutput | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding UTF8
Get-Content -LiteralPath $log
if ($buildExit -ne 0) { throw "D3D9 backend compilation failed with exit code $buildExit" }
Write-Output "D3D9 backend object built: $object"
