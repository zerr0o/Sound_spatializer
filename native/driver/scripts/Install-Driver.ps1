[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [Parameter()] [switch] $TrustTestCertificate,
    [Parameter()] [string] $CertificatePath,
    [Parameter()] [string] $DriverToolPath,
    [Parameter()] [switch] $Transactional
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

Assert-Administrator
$package = Get-DriverPackageFiles $PackageDirectory
[void](Assert-SoundSpatializerInfContract $package.Inf)
if (-not $package.Cat) {
    throw 'Le package ne contient pas de catalogue. Une installation sans catalogue est refusée.'
}
$catalogSignatureBeforeTrust = Get-AuthenticodeSignature -LiteralPath $package.Cat
if (-not $catalogSignatureBeforeTrust.SignerCertificate) {
    throw 'Le catalogue ne contient aucune signature Authenticode exploitable.'
}
[void](& (Join-Path $PSScriptRoot 'Verify-DriverPackage.ps1') `
    -PackageDirectory $package.Directory `
    -ExpectedSignerThumbprint $catalogSignatureBeforeTrust.SignerCertificate.Thumbprint)

$isTestSigned = Test-SoundSpatializerTestCertificate $catalogSignatureBeforeTrust.SignerCertificate
if ($isTestSigned) {
    # Fail before importing a certificate or touching PnP. A trusted test
    # certificate alone is insufficient when Windows code integrity refuses to
    # load test-signed kernel code.
    [void](Assert-TestSignedDriverLoadPolicy)
}

$trustedTestThumbprint = $null
$rootCertificateImported = $false
$publisherCertificateImported = $false
$rootCertificatePath = $null
$publisherCertificatePath = $null
$installationSucceeded = $false

try {
if ($TrustTestCertificate) {
    if (-not $CertificatePath) {
        throw '-CertificatePath vers le certificat public .cer est requis avec -TrustTestCertificate.'
    }

    $resolvedCertificatePath = [System.IO.Path]::GetFullPath($CertificatePath)
    if ([System.IO.Path]::GetExtension($resolvedCertificatePath) -ine '.cer' -or
        -not (Test-Path -LiteralPath $resolvedCertificatePath -PathType Leaf)) {
        throw "Le certificat public '$resolvedCertificatePath' doit être un fichier .cer existant."
    }

    $testCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($resolvedCertificatePath)
    Assert-SoundSpatializerTestCertificate $testCertificate
    if ($testCertificate.HasPrivateKey) {
        throw 'Le fichier de confiance ne doit jamais contenir de clé privée.'
    }
    if ((Get-Date) -lt $testCertificate.NotBefore -or (Get-Date) -gt $testCertificate.NotAfter) {
        throw 'Le certificat de test est hors de sa période de validité.'
    }
    if ($catalogSignatureBeforeTrust.SignerCertificate.Thumbprint -ne $testCertificate.Thumbprint) {
        throw 'Le catalogue n est pas signé par le certificat public proposé; aucune autorité de confiance n a été modifiée.'
    }

    $rootCertificatePath = "Cert:\LocalMachine\Root\$($testCertificate.Thumbprint)"
    if (-not (Test-Path -LiteralPath $rootCertificatePath)) {
        [void](Import-Certificate -FilePath $resolvedCertificatePath -CertStoreLocation 'Cert:\LocalMachine\Root')
        $rootCertificateImported = $true
    }
    $publisherCertificatePath = "Cert:\LocalMachine\TrustedPublisher\$($testCertificate.Thumbprint)"
    if (-not (Test-Path -LiteralPath $publisherCertificatePath)) {
        [void](Import-Certificate -FilePath $resolvedCertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher')
        $publisherCertificateImported = $true
    }
    $trustedTestThumbprint = $testCertificate.Thumbprint
    Write-Warning 'Un certificat auto-signé de développement vient d être ajouté à Root et TrustedPublisher (machine locale).'
}
elseif ($CertificatePath) {
    throw '-CertificatePath est accepté uniquement avec -TrustTestCertificate.'
}

$signature = Get-AuthenticodeSignature -LiteralPath $package.Cat
if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
    throw "Le catalogue n'est pas signé par une chaîne approuvée (état: $($signature.Status))."
}
if ($trustedTestThumbprint -and $signature.SignerCertificate.Thumbprint -ne $trustedTestThumbprint) {
    throw 'Le catalogue n est pas signé par le certificat de test explicitement approuvé.'
}

$devicesBefore = @(Get-SoundSpatializerPnpDevices)
$previousPublishedName = Get-ActivePublishedDriverName
$rollbackState = $null
if ($Transactional) {
    $previousInstallState = $null
    $installStatePath = Get-DriverInstallStatePath
    if (Test-Path -LiteralPath $installStatePath -PathType Leaf) {
        $previousInstallState = Get-Content -LiteralPath $installStatePath -Raw | ConvertFrom-Json
    }
    $rollbackState = [ordered]@{
        schemaVersion = 1
        hadDevice = [bool]($devicesBefore.Count -gt 0)
        previousPublishedInf = $previousPublishedName
        newPublishedInf = $null
        previousInstallState = $previousInstallState
        createdUtc = [DateTime]::UtcNow.ToString('o')
    }
    Write-JsonAtomically -Value $rollbackState -Path (Get-DriverRollbackStatePath)
}
if (-not $DriverToolPath) {
    $driverToolCandidates = @(
        (Join-Path $PSScriptRoot 'SoundSpatializer.DriverCtl.exe'),
        (Join-Path (Get-DriverRoot) 'artifacts\tools\x64\Release\SoundSpatializer.DriverCtl.exe'),
        (Join-Path (Get-DriverRoot) 'artifacts\tools\x64\Debug\SoundSpatializer.DriverCtl.exe')
    )
    $DriverToolPath = $driverToolCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}

if ($DriverToolPath) {
    $DriverToolPath = [System.IO.Path]::GetFullPath($DriverToolPath)
    if (-not (Test-Path -LiteralPath $DriverToolPath -PathType Leaf)) {
        throw "SoundSpatializer.DriverCtl.exe est introuvable sous '$DriverToolPath'."
    }
    Invoke-CheckedNative $DriverToolPath @('install', '--inf', $package.Inf) 'La création ou mise à jour du devnode audio a échoué.' | Out-Null
}
elseif ($devicesBefore.Count -eq 0) {
    throw 'SoundSpatializer.DriverCtl.exe est requis pour créer la première instance root. Compilez Build-DriverTools.ps1 ou passez -DriverToolPath.'
}
else {
    Write-Warning 'DriverCtl absent: repli de développement sur PnPUtil pour une instance déjà existante.'
    Invoke-CheckedNative 'pnputil.exe' @('/add-driver', $package.Inf, '/install') 'La mise à jour du pilote audio virtuel a échoué.' | Out-Null
}

$devicesAfter = @(Get-SoundSpatializerPnpDevices)
if ($devicesAfter.Count -eq 0) {
    throw 'PnP n expose aucune instance Sound Spatializer après installation.'
}

$publishedName = Get-ActivePublishedDriverName
if (-not $publishedName) {
    Write-Warning 'Le nom oem*.inf publié n a pas pu être déterminé; la désinstallation le recherchera à nouveau.'
}
if ($Transactional) {
    $rollbackState.newPublishedInf = $publishedName
    Write-JsonAtomically -Value $rollbackState -Path (Get-DriverRollbackStatePath)
}

$state = [ordered]@{
    schemaVersion = 1
    hardwareId = $script:SoundSpatializerHardwareId
    originalInf = $script:SoundSpatializerOriginalInfName
    publishedInf = $publishedName
    signerThumbprint = $signature.SignerCertificate.Thumbprint
    trustedTestCertificate = [bool]$trustedTestThumbprint
    installedUtc = [DateTime]::UtcNow.ToString('o')
}
Write-JsonAtomically -Value $state -Path (Get-DriverInstallStatePath)

Write-Host 'Pilote installé. La sortie audio Windows par défaut n a pas été modifiée.'
$result = [pscustomobject]@{
    Devices = @($devicesAfter | ForEach-Object InstanceId)
    PublishedInf = $publishedName
    SignerThumbprint = $signature.SignerCertificate.Thumbprint
    TrustedTestCertificate = [bool]$trustedTestThumbprint
}
$installationSucceeded = $true
$result
}
finally {
    if (-not $installationSucceeded) {
        if ($publisherCertificateImported -and $publisherCertificatePath -and
            (Test-Path -LiteralPath $publisherCertificatePath)) {
            Remove-Item -LiteralPath $publisherCertificatePath -Force -ErrorAction SilentlyContinue
        }
        if ($rootCertificateImported -and $rootCertificatePath -and
            (Test-Path -LiteralPath $rootCertificatePath)) {
            Remove-Item -LiteralPath $rootCertificatePath -Force -ErrorAction SilentlyContinue
        }
        if ($publisherCertificateImported -or $rootCertificateImported) {
            Write-Warning 'Installation interrompue : les certificats de test ajoutés par cette tentative ont été retirés.'
        }
    }
}
