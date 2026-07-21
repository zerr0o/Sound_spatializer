[CmdletBinding()]
param(
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$pinnedCommit = '3ddaad9be959816602453ecb05533f8732464ef4'

if (-not $Destination) {
    $Destination = Join-Path $repositoryRoot 'build\_deps\vcpkg'
}

$resolvedRepository = [System.IO.Path]::GetFullPath($repositoryRoot)
$resolvedDestination = [System.IO.Path]::GetFullPath($Destination)
if (-not $resolvedDestination.StartsWith($resolvedRepository, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The vcpkg checkout must stay inside the repository build directory.'
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git.exe is required to restore the pinned vcpkg dependency.'
}

if (-not (Test-Path -LiteralPath $resolvedDestination)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedDestination) | Out-Null
    git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git $resolvedDestination | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to clone vcpkg.'
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $resolvedDestination '.git'))) {
    throw "'$resolvedDestination' exists but is not a vcpkg Git checkout. It was left untouched."
}

git -C $resolvedDestination fetch --depth 1 origin $pinnedCommit | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Unable to fetch pinned vcpkg commit $pinnedCommit."
}
git -C $resolvedDestination checkout --detach $pinnedCommit | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Unable to check out pinned vcpkg commit $pinnedCommit."
}

$bootstrap = Join-Path $resolvedDestination 'bootstrap-vcpkg.bat'
& $bootstrap -disableMetrics | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw 'vcpkg bootstrap failed.'
}

$toolchain = Join-Path $resolvedDestination 'scripts\buildsystems\vcpkg.cmake'
Write-Host "Pinned vcpkg is ready."
Write-Output $toolchain
