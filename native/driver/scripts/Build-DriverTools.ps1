[CmdletBinding()]
param(
    [Parameter()] [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw 'CMake est requis pour compiler SoundSpatializer.DriverCtl.exe.'
}

$driverRoot = Get-DriverRoot
$source = Join-Path $driverRoot 'tools\DriverCtl'
$build = Join-Path $driverRoot 'build\driverctl'
$artifactDirectory = Join-Path $driverRoot "artifacts\tools\x64\$Configuration"
[void](New-Item -ItemType Directory -Path $artifactDirectory -Force)

Invoke-CheckedNative $cmake.Source @('-S', $source, '-B', $build, '-A', 'x64') 'La configuration de DriverCtl a échoué.' | Out-Null
Invoke-CheckedNative $cmake.Source @('--build', $build, '--config', $Configuration) 'La compilation de DriverCtl a échoué.' | Out-Null

$executable = Join-Path $build "$Configuration\SoundSpatializer.DriverCtl.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "DriverCtl compilé est introuvable sous '$executable'."
}
$destination = Join-Path $artifactDirectory 'SoundSpatializer.DriverCtl.exe'
Copy-Item -LiteralPath $executable -Destination $destination -Force
Write-Output $destination
