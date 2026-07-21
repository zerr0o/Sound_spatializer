[CmdletBinding()]
param(
    [Parameter()] [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug',
    [Parameter()] [ValidateSet('x64')] [string] $Platform = 'x64',
    [Parameter()] [switch] $NoRestore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$driverRoot = Get-DriverRoot
$dependencyRoot = Join-Path $driverRoot 'build\_deps\windows-driver-samples'

if (-not $NoRestore) {
    $sysvadRoot = (& (Join-Path $PSScriptRoot 'Prepare-SysVad.ps1') -Destination $dependencyRoot | Select-Object -Last 1)
}
else {
    $sysvadRoot = Join-Path $dependencyRoot 'audio\sysvad'
}
$sysvadRoot = [System.IO.Path]::GetFullPath([string]$sysvadRoot)

Assert-WdkBuildIntegration
$msbuild = Get-MSBuildPath
$solution = Join-Path $driverRoot 'SoundSpatializerDriver.sln'
$targetPlatformVersion = $script:SoundSpatializerWdkVersion

$arguments = @(
    $solution,
    '/m',
    '/t:Rebuild',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:WindowsTargetPlatformVersion=$targetPlatformVersion",
    "/p:SysvadRoot=$sysvadRoot",
    '/p:SignMode=Off',
    '/verbosity:minimal',
    '/nologo'
)
if (-not $NoRestore) {
    # The official WDK/SDK packages are pinned in both vcxproj files. MSBuild
    # restore is required here; dotnet build is not supported for WDK projects.
    $arguments += '/restore'
}
Invoke-CheckedNative $msbuild $arguments 'La compilation WDK a échoué.' | Out-Null

$packageRoot = Join-Path $driverRoot "artifacts\driver\$Platform\$Configuration"
if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "MSBuild a réussi mais le package attendu sous '$packageRoot' est introuvable."
}

$verification = & (Join-Path $PSScriptRoot 'Verify-DriverPackage.ps1') -PackageDirectory $packageRoot -AllowUnsigned
if (-not $verification.Cat) {
    throw "MSBuild n'a pas produit SoundSpatializerAudio.cat sous '$packageRoot'. Le package Debug n'est pas installable."
}
Write-Host "Package WDK non signé prêt : $($verification.PackageDirectory)"
$verification
