[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [Parameter()] [switch] $AllowUnsigned,
    [Parameter()] [string] $ExpectedSignerThumbprint
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$package = Get-DriverPackageFiles $PackageDirectory
[void](Assert-SoundSpatializerInfContract $package.Inf)
$infText = Get-Content -LiteralPath $package.Inf -Raw

$requiredPatterns = @(
    'Root\\SoundSpatializer_Audio',
    'WaveSoundSpatializer',
    'TopologySoundSpatializer',
    '\{EF58434D-ADA7-47E2-A2C4-4E8C58BA3E0B\}',
    '\{B01E7F02-85B0-4CF9-B53D-75DFD2B05E07\}',
    'Sound Spatializer'
)
foreach ($pattern in $requiredPatterns) {
    if ($infText -notmatch $pattern) {
        throw "Le contrat attendu '$pattern' est absent de '$($package.Inf)'."
    }
}
if ($infText -match 'KSCATEGORY_CAPTURE') {
    throw "L'INF expose une interface de capture ; le contrat v1 doit être render-only."
}

$infverif = Get-WindowsKitTool -Name 'infverif.exe' -Area 'Tools'
if (-not $infverif) {
    # Some preview/legacy WDK layouts placed the utility below bin.
    $infverif = Get-WindowsKitTool -Name 'infverif.exe' -Area 'bin'
}
if ($infverif) {
    Invoke-CheckedNative $infverif @('/w', '/v', $package.Inf) 'InfVerif a rejeté le package.' | Out-Null
}
else {
    Write-Warning 'InfVerif.exe est absent : validation syntaxique WDK non exécutée.'
}

if ($ExpectedSignerThumbprint -and -not $package.Cat) {
    throw 'Le catalogue signé attendu est absent.'
}

$signature = $null
$signerThumbprint = $null
$cryptographicSignatureValid = $false
$catalogMembersValid = $false
if (-not $package.Cat) {
    if (-not $AllowUnsigned) {
        throw 'Le catalogue SoundSpatializerAudio.cat est absent.'
    }
    Write-Warning 'Catalogue absent : package accepté uniquement parce que -AllowUnsigned est spécifié.'
}
else {
    $signature = Get-AuthenticodeSignature -LiteralPath $package.Cat
    if ($signature.SignerCertificate) {
        $signerThumbprint = $signature.SignerCertificate.Thumbprint
        $catalogIntegrity = Assert-CatalogCryptographicIntegrity `
            -CatalogPath $package.Cat `
            -MemberPaths @($package.Inf, $package.Sys)
        $cryptographicSignatureValid = $true
        $catalogMembersValid = $true

        if (-not $catalogIntegrity.SignerCertificate -or
            $catalogIntegrity.SignerCertificate.Thumbprint -ne $signerThumbprint) {
            throw 'Le signataire CMS du catalogue ne correspond pas au signataire Authenticode.'
        }
    }

    if ($ExpectedSignerThumbprint) {
        $expected = Normalize-CertificateThumbprint $ExpectedSignerThumbprint
        if (-not $signerThumbprint -or $signerThumbprint -ne $expected) {
            throw "Le catalogue n'est pas signé par le certificat demandé '$expected'."
        }
        Assert-SoundSpatializerTestCertificate $signature.SignerCertificate
    }
    elseif ($signature.Status -ne 'Valid') {
        if (-not $AllowUnsigned) {
            throw "Le catalogue n'a pas une signature de confiance (état : $($signature.Status))."
        }
        Write-Warning "Catalogue non approuvé (état : $($signature.Status)); attendu avant test-signing."
    }
}

[pscustomobject]@{
    PackageDirectory = $package.Directory
    Inf = $package.Inf
    Sys = $package.Sys
    Cat = $package.Cat
    InfVerifExecuted = [bool]$infverif
    SignerThumbprint = $signerThumbprint
    SignatureStatus = if ($signature) { [string]$signature.Status } else { 'Absent' }
    SignedAndTrusted = [bool]($signature -and $signature.Status -eq 'Valid')
    CryptographicSignatureValid = $cryptographicSignatureValid
    CatalogMembersValid = $catalogMembersValid
}
