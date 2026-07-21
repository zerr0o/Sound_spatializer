[CmdletBinding()]
param(
    [Parameter()] [switch] $RequireBuildReady
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$visualStudioPath = $null
$msbuildPath = $null
$visualStudioError = $null
try {
    $visualStudioPath = Get-VisualStudio2022InstallationPath
    $msbuildPath = Get-MSBuildPath
}
catch {
    $visualStudioError = $_.Exception.Message
}

$toolsetPath = if ($visualStudioPath) {
    Join-Path $visualStudioPath 'MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0'
}
else {
    $null
}
$toolsetReady = [bool]($toolsetPath -and (Test-Path -LiteralPath $toolsetPath -PathType Container))
$spectreLibrary = if ($visualStudioPath) {
    Get-ChildItem -LiteralPath (Join-Path $visualStudioPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'lib\spectre\x64\libcmt.lib' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
else {
    $null
}
$nugetContentRoot = Get-WdkNuGetContentRoot
$policy = Get-TestDriverLoadPolicy

$result = [pscustomobject]@{
    VisualStudio2022 = $visualStudioPath
    MSBuild = $msbuildPath
    VisualStudioError = $visualStudioError
    WdkVsIntegration = $toolsetPath
    WdkVsIntegrationReady = $toolsetReady
    MsvcSpectreLibrary = $spectreLibrary
    MsvcSpectreReady = [bool]$spectreLibrary
    WdkNuGetVersion = $script:SoundSpatializerWdkNuGetVersion
    WdkNuGetCached = [bool]$nugetContentRoot
    WdkNuGetContentRoot = $nugetContentRoot
    InfVerif = Get-WindowsKitTool -Name 'infverif.exe' -Area 'Tools'
    SignTool = Get-WindowsKitTool -Name 'signtool.exe' -Area 'bin'
    SecureBootEnabled = $policy.SecureBootEnabled
    TestSigningEnabled = $policy.TestSigningEnabled
    ReadyToBuild = [bool]($msbuildPath -and $toolsetReady -and $spectreLibrary)
    ReadyToLoadSelfSignedDriver = [bool]($policy.SecureBootEnabled -eq $false -and $policy.TestSigningEnabled -eq $true)
}

$result
if ($RequireBuildReady -and -not $result.ReadyToBuild) {
    Assert-WdkBuildIntegration
    throw 'Environnement WDK incomplet.'
}
