[CmdletBinding()]
param(
    [string[]]$ProfileId = @(
        'sadie-d2-kemar',
        'sadie-h6',
        'sadie-h9',
        'sadie-h10',
        'sadie-h19',
        'sadie-h20'
    ),
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repoRoot 'resources\hrtf\profiles.json'

if (-not $Destination) {
    $Destination = Join-Path $repoRoot 'resources\hrtf\data'
}

$resolvedDestination = [System.IO.Path]::GetFullPath($Destination)
$resolvedRepoRoot = [System.IO.Path]::GetFullPath($repoRoot)
if (-not $resolvedDestination.StartsWith($resolvedRepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Destination must stay inside the repository.'
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$selected = @($manifest.profiles | Where-Object { $ProfileId -contains $_.id })
$missing = @($ProfileId | Where-Object { $_ -notin $selected.id })
if ($missing.Count -gt 0) {
    throw "Unknown profile id(s): $($missing -join ', ')"
}

New-Item -ItemType Directory -Force -Path $resolvedDestination | Out-Null

function Test-SofaFile {
    param(
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -ne [long]$Profile.sofaBytes) {
        return $false
    }
    $sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    return $sha256 -eq $Profile.sofaSha256.ToLowerInvariant()
}

foreach ($profile in $selected) {
    $outputPath = Join-Path $resolvedDestination $profile.output
    if (Test-SofaFile -Profile $profile -Path $outputPath) {
        Write-Host "Already present and verified: $($profile.id)"
        continue
    }
    if (Test-Path -LiteralPath $outputPath) {
        Remove-Item -LiteralPath $outputPath -Force
    }

    $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sound-spatializer-" + [guid]::NewGuid().ToString('N'))
    $archivePath = Join-Path $tempRoot $profile.archive
    $extractRoot = Join-Path $tempRoot 'extract'
    New-Item -ItemType Directory -Path $tempRoot, $extractRoot | Out-Null

    try {
        Write-Host "Downloading $($profile.id)..."
        Invoke-WebRequest -Uri $profile.source -OutFile $archivePath

        $actualMd5 = (Get-FileHash -LiteralPath $archivePath -Algorithm MD5).Hash.ToLowerInvariant()
        if ($actualMd5 -ne $profile.archiveMd5.ToLowerInvariant()) {
            throw "Checksum mismatch for $($profile.archive): expected $($profile.archiveMd5), got $actualMd5"
        }

        Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot
        $memberPath = Join-Path $extractRoot ($profile.member -replace '/', '\')
        if (-not (Test-Path -LiteralPath $memberPath)) {
            throw "Expected SOFA member not found: $($profile.member)"
        }

        $temporaryOutput = "$outputPath.download"
        Remove-Item -LiteralPath $temporaryOutput -Force -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath $memberPath -Destination $temporaryOutput
        if (-not (Test-SofaFile -Profile $profile -Path $temporaryOutput)) {
            Remove-Item -LiteralPath $temporaryOutput -Force
            throw "Extracted SOFA checksum mismatch for $($profile.id)"
        }
        Move-Item -LiteralPath $temporaryOutput -Destination $outputPath
        Write-Host "Installed $outputPath"
    }
    finally {
        $safeTempRoot = [System.IO.Path]::GetFullPath($tempRoot)
        $systemTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($safeTempRoot.StartsWith($systemTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Test-Path -LiteralPath $safeTempRoot)) {
            Remove-Item -LiteralPath $safeTempRoot -Recurse -Force
        }
    }
}
