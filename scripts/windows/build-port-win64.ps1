[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Skeleton
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'environment.ps1')

$msysRoot = 'C:\msys64\ucrt64'
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw "MSYS2 bash not found at $bash" }
$log = New-Bt3Log 'build-port-win64'
$repoForMsys = $Bt3RepoRoot -replace '\\', '/'
$flags = @()
if ($Skeleton) { $flags += 'export BT3_SKELETON=1;' }
if ($Rebuild) { $flags += 'rm -rf port/build/obj_win64 port/build/obj_data_win64 port/build/gen port/build/gen_data_win64;' }
$flags += 'export BT3_CC=win64; python3 port/tools/undefined.py; python3 port/tools/link.py'
$command = ($flags -join ' ')
Write-Output "Build log: $log"
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$output = & $bash -lc "cd '$repoForMsys'; $command" 2>&1
$exitCode = $LASTEXITCODE
$output | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding UTF8
Get-Content -LiteralPath $log
if ($exitCode -ne 0) { throw "win64 port build failed with exit code $exitCode" }
Write-Output "Windows game executable built under port/build/"
