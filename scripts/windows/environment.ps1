[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Bt3WindowsRoot = Split-Path -Parent $PSScriptRoot
$script:Bt3RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:Bt3LogRoot = Join-Path $script:Bt3RepoRoot 'logs'
$script:Bt3BuildRoot = Join-Path $script:Bt3RepoRoot 'build\windows'

function Get-Bt3VsDevCmd {
    $candidates = @(
        'C:\BuildTools\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return $null
}

function Get-Bt3WindowsSdkVersion {
    $includeRoot = 'C:\Program Files (x86)\Windows Kits\10\Include'
    if (-not (Test-Path -LiteralPath $includeRoot)) {
        return $null
    }
    $version = Get-ChildItem -LiteralPath $includeRoot -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($null -eq $version) {
        return $null
    }
    return $version.Name
}

function Get-Bt3Cdb {
    $candidates = @(
        'C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2610.1001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe'
    )
    $windowsApps = 'C:\Program Files\WindowsApps'
    if (Test-Path -LiteralPath $windowsApps) {
        $discovered = Get-ChildItem -LiteralPath $windowsApps -Directory -Filter 'Microsoft.WinDbg_*_x64__8wekyb3d8bbwe' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'amd64\cdb.exe' }
        $candidates += $discovered
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return $null
}

function Get-Bt3ToolPath([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    switch ($Name) {
        '7z.exe' { if (Test-Path 'C:\Program Files\7-Zip\7z.exe') { return 'C:\Program Files\7-Zip\7z.exe' } }
        'dxc.exe' {
            $sdk = Get-Bt3WindowsSdkVersion
            if ($null -ne $sdk) {
                $candidate = "C:\Program Files (x86)\Windows Kits\10\bin\$sdk\x64\dxc.exe"
                if (Test-Path -LiteralPath $candidate) { return $candidate }
            }
        }
    }
    return $null
}

function New-Bt3Log([string]$Prefix) {
    New-Item -ItemType Directory -Force -Path $script:Bt3LogRoot | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    return Join-Path $script:Bt3LogRoot "$Prefix-$stamp.log"
}

function Get-Bt3EnvironmentSummary {
    [pscustomobject]@{
        Repository = $script:Bt3RepoRoot
        VsDevCmd = Get-Bt3VsDevCmd
        WindowsSdk = Get-Bt3WindowsSdkVersion
        Cdb = Get-Bt3Cdb
        Ninja = Get-Bt3ToolPath 'ninja.exe'
        Python = Get-Bt3ToolPath 'py.exe'
        SevenZip = Get-Bt3ToolPath '7z.exe'
        Dxc = Get-Bt3ToolPath 'dxc.exe'
        Sdl3Runtime = Test-Path 'C:\Program Files\SDL3\lib\SDL3.dll'
    }
}
