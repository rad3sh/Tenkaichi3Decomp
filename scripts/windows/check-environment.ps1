[CmdletBinding()]
param(
    [switch]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'environment.ps1')

$checks = [System.Collections.Generic.List[object]]::new()
function Add-Check([string]$Name, [bool]$Present, [string]$Path, [string]$Purpose) {
    $checks.Add([pscustomobject]@{ Name = $Name; Present = $Present; Path = $Path; Purpose = $Purpose })
}

$vsdev = Get-Bt3VsDevCmd
$sdk = Get-Bt3WindowsSdkVersion
$cdb = Get-Bt3Cdb
$sevenZip = Get-Bt3ToolPath '7z.exe'
$dxc = Get-Bt3ToolPath 'dxc.exe'
$ninja = Get-Bt3ToolPath 'ninja.exe'
$pythonLauncher = Get-Bt3ToolPath 'py.exe'
$sdlSdk = $Bt3Sdl3Sdk

Add-Check 'Visual Studio developer environment' ($null -ne $vsdev) $vsdev 'MSVC environment activation'
Add-Check 'Windows SDK' ($null -ne $sdk) $sdk 'Win32 and Direct3D headers/libraries'
Add-Check 'Direct3D 9 header' (Test-Path "C:\Program Files (x86)\Windows Kits\10\Include\$sdk\shared\d3d9.h") "C:\Program Files (x86)\Windows Kits\10\Include\$sdk\shared\d3d9.h" 'D3D9 API'
Add-Check 'Direct3D 9 x64 library' (Test-Path "C:\Program Files (x86)\Windows Kits\10\Lib\$sdk\um\x64\d3d9.lib") "C:\Program Files (x86)\Windows Kits\10\Lib\$sdk\um\x64\d3d9.lib" 'D3D9 x64 linking'
Add-Check 'Direct3D 9 x86 library' (Test-Path "C:\Program Files (x86)\Windows Kits\10\Lib\$sdk\um\x86\d3d9.lib") "C:\Program Files (x86)\Windows Kits\10\Lib\$sdk\um\x86\d3d9.lib" 'D3D9 x86 linking'
Add-Check 'Ninja' ($null -ne $ninja) $ninja 'Build orchestration'
Add-Check 'Python launcher' ($null -ne $pythonLauncher) $pythonLauncher 'Repository tools'
Add-Check '7-Zip' ($null -ne $sevenZip) $sevenZip 'Disc extraction'
Add-Check 'DirectX Shader Compiler' ($null -ne $dxc) $dxc 'Shader compilation for future backends'
Add-Check 'CDB' ($null -ne $cdb) $cdb 'Command-line native debugging'
Add-Check 'SDL3 runtime' (Test-Path 'C:\Program Files\SDL3\lib\SDL3.dll') 'C:\Program Files\SDL3\lib\SDL3.dll' 'Window/input/audio layer'
Add-Check 'SDL3 development headers' (Test-Path (Join-Path $sdlSdk 'include\SDL3\SDL.h')) (Join-Path $sdlSdk 'include\SDL3\SDL.h') 'Windows compilation'
Add-Check 'SDL3 x64 import library' (Test-Path (Join-Path $sdlSdk 'lib\x64\SDL3.lib')) (Join-Path $sdlSdk 'lib\x64\SDL3.lib') 'x64 linking'
Add-Check 'SDL3 x86 import library' (Test-Path (Join-Path $sdlSdk 'lib\x86\SDL3.lib')) (Join-Path $sdlSdk 'lib\x86\SDL3.lib') 'x86 linking'
Add-Check 'Git' ($null -ne (Get-Command git.exe -ErrorAction SilentlyContinue)) ((Get-Command git.exe -ErrorAction SilentlyContinue).Source) 'Source control'

if ($Json) {
    $checks | ConvertTo-Json -Depth 3
} else {
    $checks | Format-Table -AutoSize
}

$missing = @($checks | Where-Object { -not $_.Present })
if ($missing.Count -gt 0) {
    Write-Error ("Missing prerequisites: " + (($missing | ForEach-Object { $_.Name }) -join ', '))
    exit 1
}
Write-Output 'BT3 Windows development environment: OK'
