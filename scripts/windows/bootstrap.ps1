[CmdletBinding()]
param(
    [switch]$SkipPackages,
    [switch]$SkipSdlSdk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'environment.ps1')

$log = New-Bt3Log 'bootstrap'
Start-Transcript -Path $log -Append | Out-Null
try {
    Write-Output "Repository: $Bt3RepoRoot"
    Write-Output "Bootstrap log: $log"
    if (-not $SkipPackages) {
        $packages = @(
            'Microsoft.VisualStudio.2022.BuildTools',
            'Ninja-build.Ninja',
            'Python.Python.3.12',
            '7zip.7zip',
            'Microsoft.WinDbg',
            'RobedPixel.SDL3',
            'Microsoft.DirectX.ShaderCompiler'
        )
        foreach ($package in $packages) {
            Write-Output "Checking package: $package"
            winget install --id $package --exact --silent --accept-source-agreements --accept-package-agreements
            if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
                throw "winget failed for $package with exit code $LASTEXITCODE"
            }
        }
    } else {
        Write-Output 'Skipping package installation by request.'
    }
    if (-not $SkipSdlSdk) {
        $sdlCache = Join-Path $Bt3RepoRoot '.cache\SDL3'
        $sdlZip = Join-Path $sdlCache 'SDL3-devel-3.4.14-VC.zip'
        $sdlExtract = Join-Path $sdlCache 'sdk'
        New-Item -ItemType Directory -Force -Path $sdlCache | Out-Null
        if (-not (Test-Path (Join-Path $Bt3Sdl3Sdk 'include\SDL3\SDL.h'))) {
            Write-Output 'Downloading official SDL3 VC development archive.'
            Invoke-WebRequest -Uri 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.14/SDL3-devel-3.4.14-VC.zip' -OutFile $sdlZip
            $expected = '2FE279E70D426E9C644B625ACB3083EB3CFB263A92F2C5718AFF18D24A8B6E96'
            $actual = (Get-FileHash -LiteralPath $sdlZip -Algorithm SHA256).Hash
            if ($actual -ne $expected) { throw "SDL3 archive hash mismatch: $actual" }
            if (Test-Path $sdlExtract) { Remove-Item -Recurse -Force $sdlExtract }
            Expand-Archive -LiteralPath $sdlZip -DestinationPath $sdlExtract
        }
    } else {
        Write-Output 'Skipping SDL3 SDK download by request.'
    }
    & (Join-Path $PSScriptRoot 'check-environment.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Environment validation failed.' }
    Write-Output 'Bootstrap completed successfully.'
} finally {
    Stop-Transcript | Out-Null
}
