[CmdletBinding()]
param(
    [Parameter()] [ValidateRange(1, 3)] [int] $ValidYears = 1,
    [Parameter()] [string] $OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Get-DriverRoot) 'artifacts\test-signing'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
[void](New-Item -ItemType Directory -Path $OutputDirectory -Force)

$subject = "$script:SoundSpatializerTestCertificatePrefix $env:COMPUTERNAME"
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $subject `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable `
    -NotAfter (Get-Date).AddYears($ValidYears)

Assert-SoundSpatializerTestCertificate $certificate
$publicCertificatePath = Join-Path $OutputDirectory "SoundSpatializer.DriverTest.$($certificate.Thumbprint).cer"
[void](Export-Certificate -Cert $certificate -FilePath $publicCertificatePath -Type CERT)

Write-Warning 'Certificat de développement uniquement. Ce script ne modifie ni Secure Boot, ni le mode testsigning, ni les autorités de confiance machine.'
[pscustomobject]@{
    Subject = $certificate.Subject
    Thumbprint = $certificate.Thumbprint
    PublicCertificate = $publicCertificatePath
    PrivateKeyStore = 'Cert:\CurrentUser\My'
    ExpiresUtc = $certificate.NotAfter.ToUniversalTime().ToString('o')
}

