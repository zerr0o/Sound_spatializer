[CmdletBinding()]
param(
    [Parameter()] [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$ctest = Get-Command ctest.exe -ErrorAction SilentlyContinue
if (-not $cmake -or -not $ctest) {
    throw 'CMake et CTest sont requis pour le test déterministe du transport render-loopback.'
}

$driverRoot = Get-DriverRoot
$source = Join-Path $driverRoot 'transport'
$build = Join-Path $driverRoot 'build\transport-tests'
Invoke-CheckedNative $cmake.Source @('-S', $source, '-B', $build, '-A', 'x64') 'La configuration CMake du test transport a échoué.' | Out-Null
Invoke-CheckedNative $cmake.Source @('--build', $build, '--config', $Configuration) 'La compilation du test transport a échoué.' | Out-Null
Invoke-CheckedNative $ctest.Source @('--test-dir', $build, '-C', $Configuration, '--output-on-failure') 'Le test transport render-loopback a échoué.' | Out-Null

Write-Host 'Transport render-loopback: intégrité, silence, overrun, protection DRM et stress concurrent validés.'
