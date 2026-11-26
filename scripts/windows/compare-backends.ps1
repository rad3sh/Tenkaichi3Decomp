[CmdletBinding()]
param(
    # Back ends to run, in order. The first one is the reference for the diff images.
    [string[]]$Backends = @('gl', 'd3d9'),
    # How long each run lasts before it is stopped.
    [int]$Seconds = 45,
    # BT3_SHOT: a screenshot every n frames.
    [int]$Every = 150,
    [int]$From = 0,
    [int]$To = 0,
    [int]$Scale = 1,
    # Extra environment for every run, as NAME=VALUE (for example BT3_CHARA=13; several separated by ';').
    [string[]]$Env = @(),
    # Run the real menus instead of the attract-demo battle (BT3_DEMO=1).
    [switch]$Menu,
    [string]$OutDir
)

# Runs the same scene through several GPU back ends and writes each one's screenshots, a side-by-side image
# per frame and a difference image, so a D3D9 rendering gap shows up next to the OpenGL picture of that frame.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# (-File passes a list as one string: 'gl,d3d9')
$Backends = @($Backends | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Env = @($Env | ForEach-Object { $_ -split ';' } | Where-Object { $_ }) # (';' between several: BT3_PAD takes commas)

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $repoRoot 'port\build'
$executable = Join-Path $buildDir 'bt3.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable. Build it first with scripts\windows\build-port-win64.ps1."
}
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'build\windows\compare' }
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = [IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir)) }
$python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:Path"

$keep = @{}
foreach ($name in 'BT3_GPU_API', 'BT3_SCALE', 'BT3_DEMO', 'BT3_SHOT', 'BT3_SHOT_DIR', 'BT3_SHOT_FROM', 'BT3_SHOT_TO') {
    $keep[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    foreach ($backend in $Backends) {
        $dir = Join-Path $OutDir $backend
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Get-ChildItem -LiteralPath $dir -File | Remove-Item -Force
        $env:BT3_GPU_API = $backend
        $env:BT3_SCALE = "$Scale"
        $env:BT3_SHOT = "$Every"
        $env:BT3_SHOT_DIR = $dir -replace '\\', '/'
        $env:BT3_SHOT_FROM = if ($From -gt 0) { "$From" } else { $null }
        $env:BT3_SHOT_TO = if ($To -gt 0) { "$To" } else { $null }
        $env:BT3_DEMO = if ($Menu) { $null } else { '1' }
        foreach ($pair in $Env) {
            $k, $v = $pair -split '=', 2
            [Environment]::SetEnvironmentVariable($k, $v)
        }
        Write-Output "== $backend ($Seconds s, a shot every $Every frames) -> $dir"
        $proc = Start-Process -FilePath $executable -WorkingDirectory $buildDir -PassThru `
            -RedirectStandardError (Join-Path $dir 'stderr.log') -RedirectStandardOutput (Join-Path $dir 'stdout.log')
        $exited = $proc.WaitForExit($Seconds * 1000)
        if (-not $exited) { Stop-Process -Id $proc.Id -Force }
        $shots = @(Get-ChildItem -LiteralPath $dir -Filter 'gpu_*.ppm')
        Write-Output "   $($shots.Count) screenshot(s)$(if ($exited) { ', the game exited by itself' })"
    }
}
finally {
    foreach ($name in $keep.Keys) { [Environment]::SetEnvironmentVariable($name, $keep[$name]) }
}

& $python (Join-Path $PSScriptRoot 'compare_shots.py') $OutDir @Backends
