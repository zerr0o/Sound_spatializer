[CmdletBinding(DefaultParameterSetName = 'ExistingCertificate')]
param(
    [Parameter(Mandatory, ParameterSetName = 'ExistingCertificate')]
    [string] $Thumbprint,

    [Parameter(Mandatory, ParameterSetName = 'NewCertificate')]
    [switch] $CreateTestCertificate,

    [Parameter(ParameterSetName = 'ExistingCertificate')]
    [string] $PublicCertificatePath,

    [Parameter()] [switch] $NoRestore,
    [Parameter()] [uri] $TimestampServer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

Assert-WdkBuildIntegration

$driverToolPath = & (Join-Path $PSScriptRoot 'Build-DriverTools.ps1') -Configuration Debug |
    Where-Object { $_ -is [string] -and $_ -like '*.exe' } |
    Select-Object -Last 1

$buildArguments = @{
    Configuration = 'Debug'
}
if ($NoRestore) {
    $buildArguments.NoRestore = $true
}
$package = & (Join-Path $PSScriptRoot 'Build-Driver.ps1') @buildArguments |
    Where-Object { $_.PSObject.Properties.Name -contains 'PackageDirectory' } |
    Select-Object -Last 1
if (-not $package) {
    throw 'La compilation WDK n a retourné aucun package vérifiable.'
}

$certificatePath = $PublicCertificatePath
if ($CreateTestCertificate) {
    $certificate = & (Join-Path $PSScriptRoot 'New-TestCertificate.ps1') |
        Where-Object { $_.PSObject.Properties.Name -contains 'Thumbprint' } |
        Select-Object -Last 1
    if (-not $certificate) {
        throw 'La création du certificat de test n a retourné aucun certificat.'
    }
    $Thumbprint = $certificate.Thumbprint
    $certificatePath = $certificate.PublicCertificate
}
elseif ($certificatePath) {
    $certificatePath = [System.IO.Path]::GetFullPath($certificatePath)
    if ([System.IO.Path]::GetExtension($certificatePath) -ine '.cer' -or
        -not (Test-Path -LiteralPath $certificatePath -PathType Leaf)) {
        throw "Le certificat public '$certificatePath' doit être un fichier .cer existant."
    }
    $publicCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
    Assert-SoundSpatializerTestCertificate $publicCertificate
    if ($publicCertificate.HasPrivateKey) {
        throw 'Le certificat public du manifeste ne doit pas contenir de clé privée.'
    }
    if ($publicCertificate.Thumbprint -ne (Normalize-CertificateThumbprint $Thumbprint)) {
        throw "Le certificat public '$certificatePath' ne correspond pas au certificat de signature demandé."
    }
}

if ($certificatePath) {
    $certificatePath = [System.IO.Path]::GetFullPath($certificatePath)
}

$signArguments = @{
    PackageDirectory = $package.PackageDirectory
    Thumbprint = $Thumbprint
}
if ($TimestampServer) {
    $signArguments.TimestampServer = $TimestampServer
}
$signing = & (Join-Path $PSScriptRoot 'Sign-TestPackage.ps1') @signArguments |
    Where-Object { $_.PSObject.Properties.Name -contains 'SignerThumbprint' } |
    Select-Object -Last 1

$verification = & (Join-Path $PSScriptRoot 'Verify-DriverPackage.ps1') `
    -PackageDirectory $package.PackageDirectory `
    -ExpectedSignerThumbprint $Thumbprint

$manifestPath = Join-Path $package.PackageDirectory 'development-package.json'
$manifest = [ordered]@{
    schemaVersion = 1
    configuration = 'Debug'
    packageDirectory = $package.PackageDirectory
    inf = $verification.Inf
    sys = $verification.Sys
    catalog = $verification.Cat
    driverTool = $driverToolPath
    signerThumbprint = $verification.SignerThumbprint
    publicCertificate = $certificatePath
    secureBootMustBeDisabledForSelfSignedPackage = $true
    testSigningMustBeEnabled = $true
    createdUtc = [DateTime]::UtcNow.ToString('o')
}
Write-JsonAtomically -Value $manifest -Path $manifestPath

Write-Warning 'Package auto-signé de développement créé. Aucun certificat machine, périphérique, réglage BCD ou réglage UEFI n a été modifié.'
[pscustomobject]@{
    PackageDirectory = $package.PackageDirectory
    Inf = $verification.Inf
    Sys = $verification.Sys
    Catalog = $verification.Cat
    DriverTool = $driverToolPath
    SignerThumbprint = $verification.SignerThumbprint
    PublicCertificate = $certificatePath
    Manifest = $manifestPath
    TrustStatus = $signing.TrustStatus
}
