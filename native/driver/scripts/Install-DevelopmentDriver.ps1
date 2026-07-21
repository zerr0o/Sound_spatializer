[CmdletBinding()]
param(
    [Parameter()]
    [string] $ManifestPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

if (-not $PSBoundParameters.ContainsKey('ManifestPath')) {
    # $PSScriptRoot is not guaranteed to be populated while PowerShell binds
    # parameter default expressions. Resolve the repository-local manifest only
    # after the script body starts.
    $ManifestPath = Join-Path (Join-Path $PSScriptRoot '..') 'artifacts\driver\x64\Debug\development-package.json'
}

function Get-RequiredManifestProperty {
    param(
        [Parameter(Mandatory)] [psobject] $Manifest,
        [Parameter(Mandatory)] [string] $Name
    )

    $property = $Manifest.PSObject.Properties[$Name]
    if (-not $property) {
        throw "Le manifeste de développement ne contient pas la propriété obligatoire '$Name'."
    }
    return $property.Value
}

function Resolve-ManifestAbsolutePath {
    param(
        [Parameter(Mandatory)] [psobject] $Manifest,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [ValidateSet('Leaf', 'Container')] [string] $PathType
    )

    $value = Get-RequiredManifestProperty -Manifest $Manifest -Name $Name
    if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) {
        throw "La propriété '$Name' du manifeste doit être un chemin absolu non vide."
    }
    # Path.IsPathRooted also accepts drive-relative paths such as C:foo and
    # paths rooted only on the current drive such as \foo. Require a drive root
    # or a complete UNC share so manifest paths cannot depend on process state.
    $isDriveAbsolute = $value -match '^[A-Za-z]:[\\/]'
    $isUncAbsolute = $value -match '^\\\\[^\\/]+[\\/][^\\/]+(?:[\\/]|$)'
    if (-not $isDriveAbsolute -and -not $isUncAbsolute) {
        throw "La propriété '$Name' doit être un chemin absolu; valeur reçue: '$value'."
    }

    $resolved = [System.IO.Path]::GetFullPath($value)
    if (-not (Test-Path -LiteralPath $resolved -PathType $PathType)) {
        throw "Le chemin '$Name' du manifeste est introuvable: '$resolved'."
    }
    return $resolved
}

function Assert-SamePath {
    param(
        [Parameter(Mandatory)] [string] $Expected,
        [Parameter(Mandatory)] [string] $Actual,
        [Parameter(Mandatory)] [string] $Description
    )

    if (-not [string]::Equals(
            [System.IO.Path]::GetFullPath($Expected),
            [System.IO.Path]::GetFullPath($Actual),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description ne correspond pas au package vérifié. Attendu '$Expected', reçu '$Actual'."
    }
}

$resolvedManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
if ([System.IO.Path]::GetExtension($resolvedManifestPath) -ine '.json' -or
    -not (Test-Path -LiteralPath $resolvedManifestPath -PathType Leaf)) {
    throw "Le manifeste de développement '$resolvedManifestPath' est introuvable ou n'est pas un fichier JSON."
}

try {
    $manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json -ErrorAction Stop
}
catch {
    throw "Le manifeste de développement '$resolvedManifestPath' est illisible: $($_.Exception.Message)"
}

$schemaVersion = Get-RequiredManifestProperty -Manifest $manifest -Name 'schemaVersion'
if ($schemaVersion -isnot [long] -and $schemaVersion -isnot [int]) {
    throw 'schemaVersion doit être un entier JSON.'
}
if ([long]$schemaVersion -ne 1) {
    throw "Version de manifeste de développement non prise en charge: '$schemaVersion'."
}

$configuration = Get-RequiredManifestProperty -Manifest $manifest -Name 'configuration'
if ($configuration -isnot [string] -or $configuration -cne 'Debug') {
    throw "Seul un package de configuration Debug peut être installé par ce workflow; valeur reçue: '$configuration'."
}

foreach ($policyProperty in @(
        'secureBootMustBeDisabledForSelfSignedPackage',
        'testSigningMustBeEnabled'
    )) {
    $policyValue = Get-RequiredManifestProperty -Manifest $manifest -Name $policyProperty
    if ($policyValue -isnot [bool] -or -not $policyValue) {
        throw "Le manifeste ne déclare pas la contrainte de sécurité obligatoire '$policyProperty'."
    }
}

$createdUtcText = Get-RequiredManifestProperty -Manifest $manifest -Name 'createdUtc'
$createdUtc = [DateTimeOffset]::MinValue
if ($createdUtcText -isnot [string] -or
    -not [DateTimeOffset]::TryParse(
        $createdUtcText,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AssumeUniversal,
        [ref] $createdUtc)) {
    throw "createdUtc n'est pas un horodatage ISO 8601 valide: '$createdUtcText'."
}
if ($createdUtc -gt [DateTimeOffset]::UtcNow.AddMinutes(5)) {
    throw "createdUtc est dans le futur: '$createdUtcText'."
}

$packageDirectory = Resolve-ManifestAbsolutePath -Manifest $manifest -Name 'packageDirectory' -PathType Container
$manifestDirectory = [System.IO.Path]::GetDirectoryName($resolvedManifestPath)
Assert-SamePath -Expected $packageDirectory -Actual $manifestDirectory -Description 'Le répertoire du manifeste'
$expectedPackageDirectory = Join-Path (Get-DriverRoot) 'artifacts\driver\x64\Debug'
Assert-SamePath -Expected $expectedPackageDirectory -Actual $packageDirectory -Description 'Le répertoire du package Debug'

$manifestInf = Resolve-ManifestAbsolutePath -Manifest $manifest -Name 'inf' -PathType Leaf
$manifestSys = Resolve-ManifestAbsolutePath -Manifest $manifest -Name 'sys' -PathType Leaf
$manifestCatalog = Resolve-ManifestAbsolutePath -Manifest $manifest -Name 'catalog' -PathType Leaf
$driverTool = Resolve-ManifestAbsolutePath -Manifest $manifest -Name 'driverTool' -PathType Leaf
$publicCertificatePath = Resolve-ManifestAbsolutePath -Manifest $manifest -Name 'publicCertificate' -PathType Leaf

if ([System.IO.Path]::GetFileName($driverTool) -ine 'SoundSpatializer.DriverCtl.exe') {
    throw "driverTool ne référence pas SoundSpatializer.DriverCtl.exe: '$driverTool'."
}
$expectedDriverTool = Join-Path (Get-DriverRoot) 'artifacts\tools\x64\Debug\SoundSpatializer.DriverCtl.exe'
Assert-SamePath -Expected $expectedDriverTool -Actual $driverTool -Description 'Le helper DriverCtl Debug'
if ([System.IO.Path]::GetExtension($publicCertificatePath) -ine '.cer') {
    throw "publicCertificate doit référencer un certificat public .cer: '$publicCertificatePath'."
}

$package = Get-DriverPackageFiles -PackageDirectory $packageDirectory
if (-not $package.Cat) {
    throw "Le package '$packageDirectory' ne contient pas de catalogue."
}
Assert-SamePath -Expected $package.Inf -Actual $manifestInf -Description 'Le chemin INF du manifeste'
Assert-SamePath -Expected $package.Sys -Actual $manifestSys -Description 'Le chemin SYS du manifeste'
Assert-SamePath -Expected $package.Cat -Actual $manifestCatalog -Description 'Le chemin catalogue du manifeste'
[void](Assert-SoundSpatializerInfContract -InfPath $package.Inf)

$manifestThumbprintValue = Get-RequiredManifestProperty -Manifest $manifest -Name 'signerThumbprint'
if ($manifestThumbprintValue -isnot [string]) {
    throw 'signerThumbprint doit être une chaîne contenant une empreinte SHA-1 Windows.'
}
$manifestThumbprint = Normalize-CertificateThumbprint $manifestThumbprintValue
if ($manifestThumbprintValue -cne $manifestThumbprint) {
    throw 'signerThumbprint doit être normalisée: exactement 40 caractères hexadécimaux majuscules, sans séparateur.'
}

$publicCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($publicCertificatePath)
Assert-SoundSpatializerTestCertificate $publicCertificate
if ($publicCertificate.Subject -ne $publicCertificate.Issuer) {
    throw 'Le certificat de développement doit être auto-signé.'
}
if ($publicCertificate.HasPrivateKey) {
    throw 'publicCertificate contient une clé privée; seul un fichier .cer public est accepté.'
}
if ([DateTime]::UtcNow -lt $publicCertificate.NotBefore.ToUniversalTime() -or
    [DateTime]::UtcNow -gt $publicCertificate.NotAfter.ToUniversalTime()) {
    throw 'Le certificat public de test est hors de sa période de validité.'
}
if ($publicCertificate.Thumbprint -ne $manifestThumbprint) {
    throw 'Le certificat public ne correspond pas à signerThumbprint dans le manifeste.'
}

$catalogSignature = Get-AuthenticodeSignature -LiteralPath $package.Cat
if (-not $catalogSignature.SignerCertificate -or
    $catalogSignature.SignerCertificate.Thumbprint -ne $manifestThumbprint) {
    throw 'Le catalogue n est pas signé par le certificat déclaré dans le manifeste.'
}
[void](& (Join-Path $PSScriptRoot 'Verify-DriverPackage.ps1') `
    -PackageDirectory $packageDirectory `
    -ExpectedSignerThumbprint $manifestThumbprint)

# Every check above is read-only. From this point onward the operation is
# privileged, but the load-policy guard still runs before certificate trust or
# PnP/Driver Store state can be changed.
Assert-Administrator
[void](Assert-TestSignedDriverLoadPolicy)

& (Join-Path $PSScriptRoot 'Install-Driver.ps1') `
    -PackageDirectory $packageDirectory `
    -TrustTestCertificate `
    -CertificatePath $publicCertificatePath `
    -DriverToolPath $driverTool `
    -Transactional
