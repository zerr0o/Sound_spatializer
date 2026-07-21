[CmdletBinding()]
param(
    [Parameter()]
    [string]$EngineExecutable = "build/engine-mysofa/Release/SoundSpatializer.Engine.exe",

    [Parameter()]
    [string]$DesktopDirectory = "apps/desktop/src-tauri/target/release"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepositoryPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $repositoryRoot $Path
}

$engineSource = Resolve-RepositoryPath $EngineExecutable
$desktopTarget = Resolve-RepositoryPath $DesktopDirectory

if (-not (Test-Path -LiteralPath $engineSource -PathType Leaf)) {
    throw "Moteur Release introuvable : $engineSource. Lancez d'abord 'pnpm build:engine'."
}
if (-not (Test-Path -LiteralPath $desktopTarget -PathType Container)) {
    throw "Dossier Tauri Release introuvable : $desktopTarget"
}

$engineDestination = Join-Path $desktopTarget "SoundSpatializer.Engine.exe"
Copy-Item -LiteralPath $engineSource -Destination $engineDestination -Force

$hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
try {
    $sourceHash = [System.BitConverter]::ToString(
        $hashAlgorithm.ComputeHash([System.IO.File]::ReadAllBytes($engineSource))
    )
    $destinationHash = [System.BitConverter]::ToString(
        $hashAlgorithm.ComputeHash([System.IO.File]::ReadAllBytes($engineDestination))
    )
    if ($sourceHash -ne $destinationHash) {
        throw "La copie du moteur Release n'est pas identique a l'original."
    }
}
finally {
    $hashAlgorithm.Dispose()
}

Write-Host "Moteur Release place a cote de l'interface : $engineDestination"
