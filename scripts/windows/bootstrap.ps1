[CmdletBinding()]
param(
    [switch]$SkipPackages
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
    & (Join-Path $PSScriptRoot 'check-environment.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Environment validation failed.' }
    Write-Output 'Bootstrap completed successfully.'
} finally {
    Stop-Transcript | Out-Null
}
