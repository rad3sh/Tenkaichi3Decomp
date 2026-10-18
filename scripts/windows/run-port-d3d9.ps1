[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $repoRoot 'port\build'
$executable = Join-Path $buildDir 'bt3.exe'

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Tenkaichi executable not found: $executable. Build it first with scripts\windows\build-port-win64.ps1."
}

# The MinGW runtime DLLs and SDL3.dll live beside the executable. MSYS2 also supplies the UCRT API DLLs.
$msysBin = 'C:\msys64\ucrt64\bin'
$msysUsrBin = 'C:\msys64\usr\bin'
if (-not (Test-Path -LiteralPath $msysBin -PathType Container)) {
    throw "MSYS2 UCRT64 runtime not found: $msysBin"
}

$env:Path = "$msysBin;$msysUsrBin;$env:Path"
$env:BT3_GPU_API = 'd3d9'
$env:BT3_D3D9_VSYNC = '0'
$env:BT3_SCALE = '1'
$env:BT3_WINDOW = '640x480'

Write-Host 'Starting Tenkaichi3Decomp with D3D9:'
Write-Host "  BT3_GPU_API=$env:BT3_GPU_API"
Write-Host "  BT3_D3D9_VSYNC=$env:BT3_D3D9_VSYNC"
Write-Host "  BT3_SCALE=$env:BT3_SCALE"
Write-Host "  BT3_WINDOW=$env:BT3_WINDOW"

Push-Location $buildDir
try {
    $exitCode = 0
    & $executable
    $lastExitCode = Get-Variable -Name LASTEXITCODE -ValueOnly -ErrorAction SilentlyContinue
    if ($null -ne $lastExitCode) {
        $exitCode = [int]$lastExitCode
    }
    exit $exitCode
}
finally {
    Pop-Location
}
