[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ResourcesDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedOutputs = @(
    'sadie-d2-kemar.sofa',
    'sadie-h6.sofa',
    'sadie-h9.sofa',
    'sadie-h10.sofa',
    'sadie-h19.sofa',
    'sadie-h20.sofa'
)

$expectedSubjects = @('D2', 'H6', 'H9', 'H10', 'H19', 'H20')
$hrtfDirectory = [System.IO.Path]::GetFullPath((Join-Path $ResourcesDirectory 'hrtf'))
$manifestPath = Join-Path $hrtfDirectory 'profiles.json'
$noticePath = Join-Path $hrtfDirectory 'NOTICE.md'
$dataDirectory = Join-Path $hrtfDirectory 'data'

foreach ($requiredPath in @($manifestPath, $noticePath, $dataDirectory)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required HRTF payload is missing: $requiredPath"
    }
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ([int] $manifest.schemaVersion -ne 1) {
    throw "Unsupported HRTF manifest schemaVersion '$($manifest.schemaVersion)' in $manifestPath"
}

$profiles = @($manifest.profiles)
if ($profiles.Count -ne $expectedOutputs.Count) {
    throw "Expected exactly $($expectedOutputs.Count) HRTF profiles, found $($profiles.Count) in $manifestPath"
}

$outputs = @($profiles | ForEach-Object { [string] $_.output })
$subjects = @($profiles | ForEach-Object { [string] $_.subject })
if (@($outputs | Select-Object -Unique).Count -ne $outputs.Count) {
    throw "Duplicate HRTF output names are not allowed in $manifestPath"
}

function Assert-ExactSet {
    param(
        [Parameter(Mandatory = $true)] [string] $Label,
        [Parameter(Mandatory = $true)] [string[]] $Expected,
        [Parameter(Mandatory = $true)] [string[]] $Actual
    )

    $difference = @(Compare-Object -ReferenceObject @($Expected | Sort-Object) -DifferenceObject @($Actual | Sort-Object))
    if ($difference.Count -ne 0) {
        $details = ($difference | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join ', '
        throw "$Label does not match the approved set: $details"
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)] [string] $LiteralPath)

    $stream = [System.IO.File]::OpenRead($LiteralPath)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return (($sha256.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '')
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

Assert-ExactSet -Label 'HRTF manifest outputs' -Expected $expectedOutputs -Actual $outputs
Assert-ExactSet -Label 'HRTF manifest subjects' -Expected $expectedSubjects -Actual $subjects

$actualSofaFiles = @(Get-ChildItem -LiteralPath $dataDirectory -File -Filter '*.sofa' | ForEach-Object { $_.Name })
Assert-ExactSet -Label 'Staged HRTF files' -Expected $expectedOutputs -Actual $actualSofaFiles

foreach ($profile in $profiles) {
    $output = [string] $profile.output
    $sofaPath = Join-Path $dataDirectory $output
    $file = Get-Item -LiteralPath $sofaPath
    $expectedBytes = [int64] $profile.sofaBytes
    if ($file.Length -ne $expectedBytes) {
        throw "HRTF size mismatch for ${output}: expected $expectedBytes bytes, found $($file.Length)"
    }

    $expectedSha256 = ([string] $profile.sofaSha256).ToLowerInvariant()
    if ($expectedSha256 -notmatch '^[0-9a-f]{64}$') {
        throw "Invalid SHA-256 in $manifestPath for $output"
    }

    $actualSha256 = Get-Sha256 -LiteralPath $sofaPath
    if ($actualSha256 -ne $expectedSha256) {
        throw "HRTF SHA-256 mismatch for ${output}: expected $expectedSha256, found $actualSha256"
    }
}

Write-Host "Validated $($profiles.Count) HRTF payloads from $manifestPath"
