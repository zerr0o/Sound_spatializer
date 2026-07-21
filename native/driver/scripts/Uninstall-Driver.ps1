[CmdletBinding()]
param(
    [Parameter()] [switch] $RemoveTestCertificate,
    [Parameter()] [string] $CertificateThumbprint,
    [Parameter()] [switch] $Rollback,
    [Parameter()] [string] $DriverToolPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

function Remove-SoundSpatializerDevices {
    foreach ($device in @(Get-SoundSpatializerPnpDevices)) {
        Invoke-CheckedNative 'pnputil.exe' @('/remove-device', [string]$device.InstanceId) "La suppression de '$($device.InstanceId)' a échoué." | Out-Null
    }
}

function Resolve-DriverTool {
    param([Parameter()] [string] $RequestedPath)

    if ($RequestedPath) {
        $resolved = [System.IO.Path]::GetFullPath($RequestedPath)
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "SoundSpatializer.DriverCtl.exe est introuvable sous '$resolved'."
        }
        return $resolved
    }

    $candidates = @(
        (Join-Path $PSScriptRoot 'SoundSpatializer.DriverCtl.exe'),
        (Join-Path (Get-DriverRoot) 'artifacts\tools\x64\Release\SoundSpatializer.DriverCtl.exe'),
        (Join-Path (Get-DriverRoot) 'artifacts\tools\x64\Debug\SoundSpatializer.DriverCtl.exe')
    )
    return $candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}

function Remove-StateFileSafely {
    param([Parameter(Mandatory)] [string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return
    }
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $expectedDirectory = [System.IO.Path]::GetFullPath((Join-Path $env:ProgramData 'SoundSpatializer'))
    if ((Split-Path -Parent $resolved) -ine $expectedDirectory) {
        throw "Refus de supprimer un état hors du répertoire attendu: '$resolved'."
    }
    Remove-Item -LiteralPath $resolved -Force
}

Assert-Administrator
$statePath = Get-DriverInstallStatePath
$state = $null
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
    try {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    }
    catch {
        Write-Warning "L état d installation '$statePath' est illisible; la découverte PnP sera utilisée."
    }
}

if ($Rollback) {
    if ($RemoveTestCertificate -or $CertificateThumbprint) {
        throw 'Le rollback MSI ne modifie jamais les autorités de certificats.'
    }

    $rollbackStatePath = Get-DriverRollbackStatePath
    if (-not (Test-Path -LiteralPath $rollbackStatePath -PathType Leaf)) {
        throw "Etat de rollback pilote absent: '$rollbackStatePath'."
    }
    $rollbackState = Get-Content -LiteralPath $rollbackStatePath -Raw | ConvertFrom-Json
    if ([int]$rollbackState.schemaVersion -ne 1) {
        throw 'Version inconnue de l état de rollback pilote.'
    }

    if ([bool]$rollbackState.hadDevice) {
        $previousPublished = [string]$rollbackState.previousPublishedInf
        if ($previousPublished -notmatch '^oem\d+\.inf$') {
            throw 'Le rollback ne contient pas de précédent package oem*.inf valide.'
        }
        $previousInfPath = Join-Path $env:windir "INF\$previousPublished"
        if (-not (Test-Path -LiteralPath $previousInfPath -PathType Leaf)) {
            throw "Le précédent package '$previousInfPath' n est plus dans le Driver Store."
        }
        [void](Assert-SoundSpatializerInfContract $previousInfPath)

        $driverTool = Resolve-DriverTool $DriverToolPath
        if (-not $driverTool) {
            throw 'SoundSpatializer.DriverCtl.exe est requis pour forcer la restauration du précédent pilote.'
        }
        Invoke-CheckedNative $driverTool @('install', '--inf', $previousInfPath) 'La restauration du précédent pilote a échoué.' | Out-Null

        $obsoletePackages = @(Get-PublishedDriverNames | Where-Object { $_ -ine $previousPublished })
        Remove-PublishedDriverPackages -Names $obsoletePackages -Uninstall

        if ($rollbackState.previousInstallState) {
            Write-JsonAtomically -Value $rollbackState.previousInstallState -Path $statePath
        }
        else {
            Remove-StateFileSafely $statePath
        }
    }
    else {
        Remove-SoundSpatializerDevices
        Remove-PublishedDriverPackages -Names @(Get-PublishedDriverNames) -Uninstall
        Remove-StateFileSafely $statePath
    }

    Remove-StateFileSafely $rollbackStatePath
    Write-Host 'Rollback pilote terminé: le devnode antérieur a été restauré ou la nouvelle instance a été retirée.'
    return
}

$publishedNames = @(Get-PublishedDriverNames)
if ($state -and $state.publishedInf -and [string]$state.publishedInf -match '^oem\d+\.inf$') {
    $publishedNames += ([string]$state.publishedInf).ToLowerInvariant()
}

Remove-SoundSpatializerDevices
if ($publishedNames.Count -gt 0) {
    Remove-PublishedDriverPackages -Names $publishedNames -Uninstall
}
else {
    Write-Warning 'Aucun package SoundSpatializerAudio.inf publié n a été trouvé.'
}

if ($RemoveTestCertificate) {
    if (-not $CertificateThumbprint -and $state -and $state.signerThumbprint) {
        $CertificateThumbprint = [string]$state.signerThumbprint
    }
    if (-not $CertificateThumbprint) {
        throw '-CertificateThumbprint est requis car aucune empreinte fiable n est disponible dans l état d installation.'
    }

    $normalizedThumbprint = Normalize-CertificateThumbprint $CertificateThumbprint
    foreach ($store in @('Root', 'TrustedPublisher')) {
        $certificateStorePath = "Cert:\LocalMachine\$store\$normalizedThumbprint"
        if (Test-Path -LiteralPath $certificateStorePath) {
            $certificate = Get-Item -LiteralPath $certificateStorePath
            Assert-SoundSpatializerTestCertificate $certificate
            Remove-Item -LiteralPath $certificateStorePath -Force
        }
    }
    Write-Host 'Le certificat public de test a été retiré de Root et TrustedPublisher. La clé privée CurrentUser n a pas été supprimée.'
}

Remove-StateFileSafely $statePath
Remove-StateFileSafely (Get-DriverRollbackStatePath)
Write-Host 'Pilote Sound Spatializer désinstallé. Un redémarrage peut être demandé par Windows si le périphérique était encore ouvert.'
