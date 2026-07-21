[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [Parameter(Mandatory)] [string] $Thumbprint,
    [Parameter()] [uri] $TimestampServer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$package = Get-DriverPackageFiles $PackageDirectory
[void](Assert-SoundSpatializerInfContract $package.Inf)
if (-not $package.Cat) {
    throw 'Le catalogue du package est absent. Compilez d abord le package avec le WDK/Inf2Cat.'
}

$normalizedThumbprint = Normalize-CertificateThumbprint $Thumbprint
$certificatePath = "Cert:\CurrentUser\My\$normalizedThumbprint"
$certificate = Get-Item -LiteralPath $certificatePath -ErrorAction Stop
Assert-SoundSpatializerTestCertificate $certificate
if (-not $certificate.HasPrivateKey) {
    throw "Le certificat '$normalizedThumbprint' ne possède pas de clé privée dans CurrentUser\My."
}

$signTool = Get-WindowsKitTool -Name 'signtool.exe' -Area 'bin'
if (-not $signTool) {
    throw 'signtool.exe est introuvable. Installez le Windows SDK/WDK.'
}

$arguments = @('sign', '/v', '/fd', 'SHA256', '/s', 'My', '/sha1', $normalizedThumbprint)
if ($TimestampServer) {
    $arguments += @('/tr', $TimestampServer.AbsoluteUri, '/td', 'SHA256')
}
$arguments += $package.Cat
Invoke-CheckedNative $signTool $arguments 'La signature du catalogue a échoué.' | Out-Null

$signature = Get-AuthenticodeSignature -LiteralPath $package.Cat
if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $normalizedThumbprint) {
    throw 'Le catalogue ne porte pas la signature du certificat demandé après SignTool.'
}

$verification = & (Join-Path $PSScriptRoot 'Verify-DriverPackage.ps1') `
    -PackageDirectory $package.Directory `
    -ExpectedSignerThumbprint $normalizedThumbprint

Write-Warning "Signature de test appliquée. Etat de confiance local: $($signature.Status). La publication reste bloquée tant que Microsoft n'a pas signé le pilote."
[pscustomobject]@{
    Catalog = $package.Cat
    SignerThumbprint = $signature.SignerCertificate.Thumbprint
    TrustStatus = [string]$signature.Status
    InfVerifExecuted = [bool]$verification.InfVerifExecuted
}
