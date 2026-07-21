[CmdletBinding()]
param(
    [Parameter()]
    [string]$Executable = "apps/desktop/src-tauri/target/release/sound-spatializer-desktop.exe",

    [Parameter()]
    [string]$EngineExecutable
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$candidate = if ([System.IO.Path]::IsPathRooted($Executable)) {
    $Executable
} else {
    Join-Path $repositoryRoot $Executable
}

if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
    throw "Executable Tauri introuvable : $candidate"
}

$resolved = (Resolve-Path -LiteralPath $candidate).Path
$bytes = [System.IO.File]::ReadAllBytes($resolved)
if ($bytes.Length -lt 2 -or $bytes[0] -ne [byte][char]'M' -or $bytes[1] -ne [byte][char]'Z') {
    throw "Le payload Tauri n'est pas un executable Windows PE : $resolved"
}

$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
$utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
$forbiddenOrigins = @(
    "http://127.0.0.1:1420",
    "http://localhost:1420",
    "https://127.0.0.1:1420",
    "https://localhost:1420"
)

foreach ($origin in $forbiddenOrigins) {
    if ($ascii.IndexOf($origin, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $utf16.IndexOf($origin, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Build Tauri de developpement detecte dans le payload Release ($origin). Construisez-le avec 'pnpm build:ui'."
    }
}

$engineCandidate = if ($EngineExecutable) {
    if ([System.IO.Path]::IsPathRooted($EngineExecutable)) {
        $EngineExecutable
    } else {
        Join-Path $repositoryRoot $EngineExecutable
    }
} else {
    Join-Path (Split-Path -Parent $resolved) "SoundSpatializer.Engine.exe"
}

if (-not (Test-Path -LiteralPath $engineCandidate -PathType Leaf)) {
    throw "Moteur compagnon introuvable : $engineCandidate"
}

$engineResolved = (Resolve-Path -LiteralPath $engineCandidate).Path
$engineBytes = [System.IO.File]::ReadAllBytes($engineResolved)
if ($engineBytes.Length -lt 2 -or $engineBytes[0] -ne [byte][char]'M' -or $engineBytes[1] -ne [byte][char]'Z') {
    throw "Le moteur compagnon n'est pas un executable Windows PE : $engineResolved"
}

Write-Host "Payload Tauri Release verifie : $resolved"
Write-Host "Moteur compagnon verifie : $engineResolved"
