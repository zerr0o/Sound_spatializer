[CmdletBinding()]
param(
    [Parameter()] [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$upstreamUrl = 'https://github.com/microsoft/Windows-driver-samples.git'
$pinnedCommit = '1fd430c78971c31b624b0773bbea825d8b480d55'
$driverRoot = Get-DriverRoot

if (-not $Destination) {
    $Destination = Join-Path $driverRoot 'build\_deps\windows-driver-samples'
}
$Destination = [System.IO.Path]::GetFullPath($Destination)

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) {
    throw 'git.exe est requis pour restaurer la dépendance SysVAD épinglée.'
}

if (-not (Test-Path -LiteralPath $Destination)) {
    $parent = Split-Path -Parent $Destination
    [void](New-Item -ItemType Directory -Path $parent -Force)
    Invoke-CheckedNative $git.Source @('clone', '--filter=blob:none', '--no-checkout', $upstreamUrl, $Destination) 'Le clonage de Windows-driver-samples a échoué.' | Out-Null
    Invoke-CheckedNative $git.Source @('-C', $Destination, 'fetch', '--depth=1', 'origin', $pinnedCommit) "Le commit SysVAD épinglé n'a pas pu être récupéré." | Out-Null
    Invoke-CheckedNative $git.Source @('-C', $Destination, 'checkout', '--detach', $pinnedCommit) 'Le checkout SysVAD épinglé a échoué.' | Out-Null
}

$gitDirectory = Join-Path $Destination '.git'
if (-not (Test-Path -LiteralPath $gitDirectory -PathType Container)) {
    throw "'$Destination' existe mais n'est pas le clone Git attendu. Aucun fichier n'a été supprimé."
}

$head = (& $git.Source -C $Destination rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -ne $pinnedCommit) {
    throw "La dépendance existante est au commit '$head', attendu '$pinnedCommit'. Supprimez-la manuellement après vérification ou choisissez une autre destination."
}

$sysvadRoot = Join-Path $Destination 'audio\sysvad'
if (-not (Test-Path -LiteralPath (Join-Path $sysvadRoot 'adapter.cpp') -PathType Leaf)) {
    throw "Le commit restauré ne contient pas le dossier SysVAD attendu sous '$sysvadRoot'."
}

$patches = @(Get-ChildItem -LiteralPath (Join-Path $driverRoot 'patches') -File -Filter '*.patch' | Sort-Object Name)
foreach ($patch in $patches) {
    & $git.Source -C $Destination apply --check -- $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Invoke-CheckedNative $git.Source @('-C', $Destination, 'apply', '--whitespace=error-all', '--', $patch.FullName) "Le patch '$($patch.Name)' n'a pas pu être appliqué." | Out-Null
        continue
    }

    & $git.Source -C $Destination apply --reverse --check -- $patch.FullName 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Le patch '$($patch.Name)' ne correspond plus au commit épinglé et n'était pas déjà appliqué."
    }
}

Write-Host "SysVAD prêt : $sysvadRoot"
Write-Output $sysvadRoot
