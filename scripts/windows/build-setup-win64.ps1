[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'environment.ps1')

$msysRoot = 'C:\msys64\ucrt64'
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw "MSYS2 bash not found at $bash" }
$log = New-Bt3Log 'build-setup-win64'
$repoForMsys = $Bt3RepoRoot -replace '\\', '/'
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$output = & $bash -lc "cd '$repoForMsys'; sh port/setup/build.sh win" 2>&1
$exitCode = $LASTEXITCODE
$output | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding UTF8
Get-Content -LiteralPath $log
if ($exitCode -ne 0) { throw "Windows setup build failed with exit code $exitCode" }
Write-Output "Windows setup executable built under port/build/"
