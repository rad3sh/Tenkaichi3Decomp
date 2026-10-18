[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'environment.ps1')

$cdb = Get-Bt3Cdb
if ($null -eq $cdb) { throw 'CDB was not found. Install Microsoft.WinDbg first.' }
$probe = Join-Path $Bt3BuildRoot "debug_probe-$Architecture.exe"
$object = Join-Path $Bt3BuildRoot "debug_probe-$Architecture.obj"
$symbols = Join-Path $Bt3BuildRoot "debug_probe-$Architecture.pdb"
$source = Join-Path $Bt3BuildRoot 'debug_probe.c'
$vsdev = Get-Bt3VsDevCmd
if ($null -eq $vsdev) { throw 'VsDevCmd.bat was not found.' }
New-Item -ItemType Directory -Force -Path $Bt3BuildRoot | Out-Null

@'
#include <windows.h>
int main(void) {
    OutputDebugStringA("bt3 debug probe\n");
    DebugBreak();
    return 0;
}
'@ | Set-Content -LiteralPath $source -Encoding ASCII

$buildCommand = "call `"$vsdev`" -arch=$Architecture -host_arch=x64 >nul && cl /nologo /W4 /WX /FS /Zi /Fd:`"$symbols`" /c `"$source`" /Fo:`"$object`" && link /DEBUG /PDB:`"$symbols`" /OUT:`"$probe`" `"$object`""
$buildLog = New-Bt3Log "debug-probe-build-$Architecture"
cmd.exe /d /s /c $buildCommand 2>&1 | Tee-Object -FilePath $buildLog
if ($LASTEXITCODE -ne 0) { throw 'Debug probe compilation failed.' }

$debugLog = New-Bt3Log "debug-probe-cdb-$Architecture"
& $cdb -logo $debugLog -lines -c "g; .lastevent; k; q" $probe
$debugExit = $LASTEXITCODE
Write-Output "CDB log: $debugLog"
if ($debugExit -ne 0) { throw "CDB exited with code $debugExit" }
Write-Output 'CDB debug probe completed.'
