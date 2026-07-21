[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$WithoutMySofa
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = if ($WithoutMySofa) {
    Join-Path $repositoryRoot 'build\engine-dev'
}
else {
    Join-Path $repositoryRoot 'build\engine-mysofa'
}
$arguments = @(
    '--fresh',
    '-S', (Join-Path $repositoryRoot 'native\engine'),
    '-B', $buildDirectory,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    '-DBUILD_TESTING=ON'
)

if ($WithoutMySofa) {
    $arguments += '-DSOUND_SPATIALIZER_ENABLE_MYSOFA=OFF'
}
else {
    $toolchainOutput = @(& (Join-Path $PSScriptRoot 'bootstrap-vcpkg.ps1'))
    $toolchain = $toolchainOutput | Select-Object -Last 1
    if (-not $toolchain -or -not (Test-Path -LiteralPath $toolchain)) {
        throw 'The pinned vcpkg toolchain could not be resolved.'
    }
    $arguments += @(
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_MANIFEST_DIR=$repositoryRoot",
        "-DVCPKG_INSTALLED_DIR=$(Join-Path $repositoryRoot 'vcpkg_installed')",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        '-DSOUND_SPATIALIZER_ENABLE_MYSOFA=ON'
    )
}

& cmake @arguments
if ($LASTEXITCODE -ne 0) {
    throw 'CMake configuration failed.'
}

& cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw 'Engine build failed.'
}

& ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw 'Engine tests failed.'
}
