Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# pnpm can be launched from PowerShell 7 and pass its module directories first
# to a child Windows PowerShell 5.1 process. In that case 5.1 may try to load
# the incompatible PowerShell 7 copy of Microsoft.PowerShell.Security. Keep
# the inbox Windows PowerShell modules first for every driver script.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    $windowsPowerShellModules = Join-Path $PSHOME 'Modules'
    $modulePaths = @(
        $windowsPowerShellModules
        @($env:PSModulePath -split ';') |
            Where-Object { $_ -and -not [string]::Equals($_, $windowsPowerShellModules, [StringComparison]::OrdinalIgnoreCase) }
    )
    $env:PSModulePath = $modulePaths -join ';'
}

$script:SoundSpatializerHardwareId = 'Root\SoundSpatializer_Audio'
$script:SoundSpatializerOriginalInfName = 'SoundSpatializerAudio.inf'
$script:SoundSpatializerTestCertificatePrefix = 'CN=Sound Spatializer Driver Test'
$script:SoundSpatializerWdkVersion = '10.0.26100.0'
$script:SoundSpatializerWdkNuGetVersion = '10.0.26100.6584'
$script:SoundSpatializerWdkVsComponent = 'Component.Microsoft.Windows.DriverKit'
$script:SoundSpatializerSpectreVsComponent = 'Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64.Spectre'

function Get-DriverRoot {
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
}

function Get-RepositoryRoot {
    return [System.IO.Path]::GetFullPath((Join-Path (Get-DriverRoot) '..\..'))
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Cette opération doit être lancée depuis PowerShell « Exécuter en tant qu'administrateur »."
    }
}

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter()] [string[]] $ArgumentList = @(),
        [Parameter()] [string] $FailureMessage = 'La commande native a échoué.'
    )

    $output = @(& $FilePath @ArgumentList 2>&1)
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($exitCode -ne 0) {
        throw "$FailureMessage (code $exitCode): $FilePath"
    }
    return $output
}

function Get-VsWhere {
    $candidate = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw 'vswhere.exe est introuvable. Installez Visual Studio 2022 avec les outils C++.'
    }
    return $candidate
}

function Get-VisualStudio2022InstallationPath {
    $vswhere = Get-VsWhere
    $installPath = (& $vswhere -latest -version '[17.0,18.0)' -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1)
    if (-not $installPath) {
        throw "Aucune installation Visual Studio 2022 contenant MSBuild n'a été trouvée."
    }
    return [System.IO.Path]::GetFullPath([string]$installPath)
}

function Get-MSBuildPath {
    $installPath = Get-VisualStudio2022InstallationPath

    # The WDK NuGet targets only populate several native tool paths
    # (StampInf, TraceWPP, DrvCat...) for AMD64/ARM64 host processes.  Using
    # the 32-bit MSBuild host leaves those properties empty even though the
    # tools are present in the restored package.
    $msbuild64 = Join-Path $installPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild64 -PathType Leaf)) {
        throw "MSBuild x64 est introuvable sous '$installPath'. Le package WDK x64 ne peut pas être compilé avec l'hôte MSBuild 32 bits."
    }
    return $msbuild64
}

function Assert-WdkBuildIntegration {
    $installPath = Get-VisualStudio2022InstallationPath
    $toolset = Join-Path $installPath 'MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0'

    if (-not (Test-Path -LiteralPath $toolset -PathType Container)) {
        $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        throw @"
Le composant Visual Studio 2022 « Windows Driver Kit » est absent. Le projet
restaure les headers, bibliothèques et outils WDK $script:SoundSpatializerWdkNuGetVersion depuis
NuGet, mais l'intégration MSBuild WindowsKernelModeDriver10.0 reste requise.

Commande d'installation explicite (elle modifie Visual Studio et peut demander
une élévation) :
  & '$installer' modify --installPath '$installPath' --add $script:SoundSpatializerWdkVsComponent --passive --norestart
"@
    }

    $msvcRoot = Join-Path $installPath 'VC\Tools\MSVC'
    $spectreLibrary = Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'lib\spectre\x64\libcmt.lib' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $spectreLibrary) {
        $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        throw @"
Les bibliothèques C++ Spectre x64 du toolset v143 sont absentes. Les pilotes
Universal Windows 11 les exigent. Commande d'installation explicite :
  & '$installer' modify --installPath '$installPath' --add $script:SoundSpatializerSpectreVsComponent --passive --norestart
"@
    }
}

function Get-NuGetPackageRoot {
    if ($env:NUGET_PACKAGES) {
        return [System.IO.Path]::GetFullPath($env:NUGET_PACKAGES)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $env:USERPROFILE '.nuget\packages'))
}

function Get-WdkNuGetContentRoot {
    $candidate = Join-Path (Get-NuGetPackageRoot) "microsoft.windows.wdk.x64\$script:SoundSpatializerWdkNuGetVersion\c"
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        return $candidate
    }
    return $null
}

function Get-WindowsKitTool {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter()] [ValidateSet('bin', 'Tools')] [string] $Area = 'bin'
    )

    $candidates = @()
    $roots = @()
    $machineRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\$Area"
    if (Test-Path -LiteralPath $machineRoot -PathType Container) {
        $roots += $machineRoot
    }

    $wdkNuGetRoot = Get-WdkNuGetContentRoot
    if ($wdkNuGetRoot) {
        $nugetArea = Join-Path $wdkNuGetRoot $Area
        if (Test-Path -LiteralPath $nugetArea -PathType Container) {
            $roots += $nugetArea
        }
    }
    if ($Area -eq 'bin') {
        $sdkNuGetRoot = Join-Path (Get-NuGetPackageRoot) "microsoft.windows.sdk.cpp\$script:SoundSpatializerWdkNuGetVersion\c\bin"
        if (Test-Path -LiteralPath $sdkNuGetRoot -PathType Container) {
            $roots += $sdkNuGetRoot
        }
    }

    foreach ($kitRoot in @($roots | Sort-Object -Unique)) {
        foreach ($directory in Get-ChildItem -LiteralPath $kitRoot -Directory -ErrorAction SilentlyContinue) {
            $parsed = [version]::new()
            if (-not [version]::TryParse($directory.Name, [ref] $parsed)) {
                continue
            }
            foreach ($architecture in @('x64', 'x86')) {
                $candidate = Join-Path $directory.FullName "$architecture\$Name"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $candidates += [pscustomobject]@{
                        Preferred = [int]($parsed -eq [version]$script:SoundSpatializerWdkVersion)
                        Version = $parsed
                        Architecture = $architecture
                        Path = $candidate
                    }
                }
            }
        }
    }

    foreach ($kitRoot in @($roots | Sort-Object -Unique)) {
        foreach ($architecture in @('x64', 'x86')) {
            $legacy = Join-Path $kitRoot "$architecture\$Name"
            if (Test-Path -LiteralPath $legacy -PathType Leaf) {
                $candidates += [pscustomobject]@{
                    Preferred = 0
                    Version = [version]'0.0'
                    Architecture = $architecture
                    Path = $legacy
                }
            }
        }
    }

    return ($candidates |
        Sort-Object Preferred, Version, @{ Expression = { [int]($_.Architecture -eq 'x64') } } -Descending |
        Select-Object -First 1 -ExpandProperty Path)
}

function Assert-DriverPackageDirectory {
    param([Parameter(Mandatory)] [string] $PackageDirectory)

    $resolved = [System.IO.Path]::GetFullPath($PackageDirectory)
    $root = [System.IO.Path]::GetPathRoot($resolved)
    if ($resolved.TrimEnd('\') -eq $root.TrimEnd('\')) {
        throw "Le chemin de package ne peut pas être la racine '$resolved'."
    }
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "Le répertoire de package '$resolved' n'existe pas."
    }
    return $resolved
}

function Get-DriverPackageFiles {
    param([Parameter(Mandatory)] [string] $PackageDirectory)

    $resolved = Assert-DriverPackageDirectory $PackageDirectory
    $inf = @(Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter 'SoundSpatializerAudio.inf')
    $sys = @(Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter 'SoundSpatializerAudio.sys')
    $cat = @(Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter 'SoundSpatializerAudio.cat')

    if ($inf.Count -ne 1 -or $sys.Count -ne 1) {
        throw "Le package doit contenir exactement un INF et un SYS SoundSpatializerAudio (INF=$($inf.Count), SYS=$($sys.Count))."
    }
    if ($cat.Count -gt 1) {
        throw "Plusieurs catalogues SoundSpatializerAudio.cat ont été trouvés dans '$resolved'."
    }

    return [pscustomobject]@{
        Directory = $resolved
        Inf = $inf[0].FullName
        Sys = $sys[0].FullName
        Cat = if ($cat.Count -eq 1) { $cat[0].FullName } else { $null }
    }
}

function Get-CatalogMemberHash {
    param([Parameter(Mandatory)] [string] $Path)

    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "Le membre de catalogue '$resolved' est introuvable."
    }

    if (-not ('SoundSpatializerCatalogNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SoundSpatializerCatalogNative
{
    [DllImport("wintrust.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CryptCATAdminCalcHashFromFileHandle(
        IntPtr fileHandle,
        ref uint hashSize,
        byte[] hash,
        uint flags);
}
'@
    }

    $stream = [System.IO.File]::Open(
        $resolved,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    try {
        [uint32] $hashSize = 0
        [void][SoundSpatializerCatalogNative]::CryptCATAdminCalcHashFromFileHandle(
            $stream.SafeFileHandle.DangerousGetHandle(),
            [ref] $hashSize,
            $null,
            0
        )
        if ($hashSize -eq 0) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Windows n'a pas pu déterminer la taille du hash catalogue pour '$resolved' (erreur $errorCode)."
        }

        $hash = [byte[]]::new($hashSize)
        if (-not [SoundSpatializerCatalogNative]::CryptCATAdminCalcHashFromFileHandle(
            $stream.SafeFileHandle.DangerousGetHandle(),
            [ref] $hashSize,
            $hash,
            0
        )) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Le hash catalogue de '$resolved' n'a pas pu être calculé (erreur $errorCode)."
        }
        return $hash
    }
    finally {
        $stream.Dispose()
    }
}

function Test-ByteSequence {
    param(
        [Parameter(Mandatory)] [byte[]] $Haystack,
        [Parameter(Mandatory)] [byte[]] $Needle
    )

    if ($Needle.Length -eq 0 -or $Needle.Length -gt $Haystack.Length) {
        return $false
    }
    for ($offset = 0; $offset -le ($Haystack.Length - $Needle.Length); $offset++) {
        $matches = $true
        for ($index = 0; $index -lt $Needle.Length; $index++) {
            if ($Haystack[$offset + $index] -ne $Needle[$index]) {
                $matches = $false
                break
            }
        }
        if ($matches) {
            return $true
        }
    }
    return $false
}

function Assert-CatalogCryptographicIntegrity {
    param(
        [Parameter(Mandatory)] [string] $CatalogPath,
        [Parameter(Mandatory)] [string[]] $MemberPaths
    )

    $resolvedCatalog = [System.IO.Path]::GetFullPath($CatalogPath)
    if (-not (Test-Path -LiteralPath $resolvedCatalog -PathType Leaf)) {
        throw "Le catalogue '$resolvedCatalog' est introuvable."
    }

    Add-Type -AssemblyName System.Security
    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    try {
        $cms.Decode([System.IO.File]::ReadAllBytes($resolvedCatalog))
        if ($cms.SignerInfos.Count -ne 1) {
            throw "Le catalogue doit contenir exactement une signature CMS (trouvé: $($cms.SignerInfos.Count))."
        }
        # verifySignatureOnly=true validates the CMS signature without requiring
        # the self-signed development root to be trusted on this machine yet.
        $cms.CheckSignature($true)
    }
    catch {
        throw "La signature cryptographique du catalogue '$resolvedCatalog' est invalide: $($_.Exception.Message)"
    }

    foreach ($memberPath in $MemberPaths) {
        $memberHash = Get-CatalogMemberHash $memberPath
        if (-not (Test-ByteSequence -Haystack $cms.ContentInfo.Content -Needle $memberHash)) {
            $hashText = ([BitConverter]::ToString($memberHash)).Replace('-', '')
            throw "Le fichier '$memberPath' n'appartient pas au catalogue signé (hash $hashText absent)."
        }
    }

    return [pscustomobject]@{
        Catalog = $resolvedCatalog
        SignerCertificate = $cms.SignerInfos[0].Certificate
        Members = @($MemberPaths | ForEach-Object { [System.IO.Path]::GetFullPath($_) })
    }
}

function Assert-SoundSpatializerInfContract {
    param([Parameter(Mandatory)] [string] $InfPath)

    $resolved = [System.IO.Path]::GetFullPath($InfPath)
    if ([System.IO.Path]::GetExtension($resolved) -ine '.inf' -or
        -not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "L'INF Sound Spatializer '$resolved' est introuvable."
    }

    $text = Get-Content -LiteralPath $resolved -Raw
    $requiredPatterns = @(
        '(?im)^\s*Class\s*=\s*MEDIA\s*$',
        'Root\\SoundSpatializer_Audio',
        'SoundSpatializerAudio\.sys',
        'WaveSoundSpatializer',
        'TopologySoundSpatializer',
        '\{EF58434D-ADA7-47E2-A2C4-4E8C58BA3E0B\}',
        '\{B01E7F02-85B0-4CF9-B53D-75DFD2B05E07\}'
    )
    foreach ($pattern in $requiredPatterns) {
        if ($text -notmatch $pattern) {
            throw "L'INF '$resolved' ne respecte pas le contrat Sound Spatializer ('$pattern' absent)."
        }
    }
    if ($text -match 'KSCATEGORY_CAPTURE') {
        throw "L'INF '$resolved' expose une interface de capture interdite par le contrat v1."
    }
    return $resolved
}

function Test-SoundSpatializerPublishedInf {
    param([Parameter(Mandatory)] [string] $PublishedName)

    if ($PublishedName -notmatch '^oem\d+\.inf$') {
        return $false
    }
    $expectedDirectory = [System.IO.Path]::GetFullPath((Join-Path $env:windir 'INF'))
    $path = [System.IO.Path]::GetFullPath((Join-Path $expectedDirectory $PublishedName))
    if ((Split-Path -Parent $path) -ine $expectedDirectory) {
        return $false
    }
    try {
        [void](Assert-SoundSpatializerInfContract $path)
        return $true
    }
    catch {
        return $false
    }
}

function Get-PublishedDriverNames {
    $csvLines = @(& pnputil.exe /enum-drivers /class Media /format csv 2>$null)
    if ($LASTEXITCODE -ne 0 -or $csvLines.Count -lt 2) {
        return @()
    }

    $matches = @()
    foreach ($row in ($csvLines | ConvertFrom-Csv)) {
        $properties = @($row.PSObject.Properties)
        $original = $properties | Where-Object { $_.Name -eq 'OriginalName' } | Select-Object -First 1 -ExpandProperty Value
        $published = $properties | Where-Object { $_.Name -eq 'DriverName' } | Select-Object -First 1 -ExpandProperty Value

        if (-not $original) {
            $original = $properties.Value | Where-Object { $_ -ieq $script:SoundSpatializerOriginalInfName } | Select-Object -First 1
        }
        if (-not $published) {
            $published = $properties.Value | Where-Object { $_ -match '^oem\d+\.inf$' } | Select-Object -First 1
        }

        if ($original -ieq $script:SoundSpatializerOriginalInfName -and
            $published -match '^oem\d+\.inf$' -and
            (Test-SoundSpatializerPublishedInf $published)) {
            $matches += $published.ToLowerInvariant()
        }
    }
    return @($matches | Sort-Object -Unique)
}

function Get-PublishedDriverName {
    return @(Get-PublishedDriverNames) | Select-Object -First 1
}

function Remove-PublishedDriverPackages {
    param(
        [Parameter()] [string[]] $Names,
        [Parameter()] [switch] $Uninstall
    )

    foreach ($name in @($Names | Where-Object { $_ } | Sort-Object -Unique)) {
        if ($name -notmatch '^oem\d+\.inf$') {
            throw "Nom de pilote publié non sûr: '$name'."
        }

        $publishedInfPath = Join-Path $env:windir "INF\$name"
        if (-not (Test-Path -LiteralPath $publishedInfPath -PathType Leaf)) {
            Write-Warning "Le package '$name' n'est déjà plus présent dans le Driver Store."
            continue
        }
        if (-not (Test-SoundSpatializerPublishedInf $name)) {
            throw "Refus de supprimer '$name': son INF ne respecte pas le contrat Sound Spatializer."
        }

        $arguments = @('/delete-driver', $name)
        if ($Uninstall) {
            $arguments += '/uninstall'
        }
        Invoke-CheckedNative 'pnputil.exe' $arguments "La suppression de '$name' du Driver Store a échoué." | Out-Null
    }
}

function Get-ActivePublishedDriverName {
    $propertyCommand = Get-Command Get-PnpDeviceProperty -ErrorAction SilentlyContinue
    if (-not $propertyCommand) {
        return Get-PublishedDriverName
    }

    foreach ($device in @(Get-SoundSpatializerPnpDevices)) {
        $property = Get-PnpDeviceProperty `
            -InstanceId $device.InstanceId `
            -KeyName 'DEVPKEY_Device_DriverInfPath' `
            -ErrorAction SilentlyContinue
        if ($property -and $property.Data -and [string]$property.Data -match '^oem\d+\.inf$') {
            $candidate = ([string]$property.Data).ToLowerInvariant()
            if (Test-SoundSpatializerPublishedInf $candidate) {
                return $candidate
            }
        }
    }
    return Get-PublishedDriverName
}

function Get-DriverInstallStatePath {
    $stateDirectory = Join-Path $env:ProgramData 'SoundSpatializer'
    return Join-Path $stateDirectory 'driver-install.json'
}

function Get-DriverRollbackStatePath {
    $stateDirectory = Join-Path $env:ProgramData 'SoundSpatializer'
    return Join-Path $stateDirectory 'driver-rollback.json'
}

function Write-JsonAtomically {
    param(
        [Parameter(Mandatory)] [object] $Value,
        [Parameter(Mandatory)] [string] $Path
    )

    $resolved = [System.IO.Path]::GetFullPath($Path)
    $directory = Split-Path -Parent $resolved
    [void](New-Item -ItemType Directory -Path $directory -Force)

    $temporary = Join-Path $directory ('.' + [System.IO.Path]::GetFileName($resolved) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $json = $Value | ConvertTo-Json -Depth 8
        [System.IO.File]::WriteAllText($temporary, $json, [System.Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $resolved -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Normalize-CertificateThumbprint {
    param([Parameter(Mandatory)] [string] $Thumbprint)

    $normalized = ($Thumbprint -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
    # Windows certificate-store paths and SignTool /sha1 both consume the
    # certificate's SHA-1 thumbprint, even when the signature digest is SHA-256.
    if ($normalized.Length -ne 40) {
        throw "L'empreinte de certificat '$Thumbprint' n'est pas une empreinte SHA-1 Windows valide."
    }
    return $normalized
}

function Assert-SoundSpatializerTestCertificate {
    param([Parameter(Mandatory)] [System.Security.Cryptography.X509Certificates.X509Certificate2] $Certificate)

    if (-not $Certificate.Subject.StartsWith($script:SoundSpatializerTestCertificatePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Le certificat '$($Certificate.Subject)' n'appartient pas au périmètre de test Sound Spatializer."
    }

    $codeSigningOid = '1.3.6.1.5.5.7.3.3'
    $hasCodeSigning = $false
    foreach ($extension in $Certificate.Extensions) {
        if ($extension -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]) {
            foreach ($oid in $extension.EnhancedKeyUsages) {
                if ($oid.Value -eq $codeSigningOid) {
                    $hasCodeSigning = $true
                }
            }
        }
    }
    if (-not $hasCodeSigning) {
        throw "Le certificat '$($Certificate.Subject)' ne contient pas l'EKU Code Signing."
    }
}

function Test-SoundSpatializerTestCertificate {
    param([Parameter(Mandatory)] [System.Security.Cryptography.X509Certificates.X509Certificate2] $Certificate)

    try {
        Assert-SoundSpatializerTestCertificate $Certificate
        return $true
    }
    catch {
        return $false
    }
}

function Get-TestDriverLoadPolicy {
    $secureBootEnabled = $null
    $secureBootState = Get-ItemProperty `
        -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' `
        -Name UEFISecureBootEnabled `
        -ErrorAction SilentlyContinue
    if ($null -ne $secureBootState) {
        $secureBootEnabled = ([int]$secureBootState.UEFISecureBootEnabled -eq 1)
    }

    $testSigningEnabled = $null
    $bcdEdit = Join-Path $env:windir 'System32\bcdedit.exe'
    if (Test-Path -LiteralPath $bcdEdit -PathType Leaf) {
        $bcdOutput = @(& $bcdEdit /enum 2>&1)
        $bcdExitCode = $LASTEXITCODE
        if ($bcdExitCode -eq 0) {
            $testSigningEnabled = $false
            $text = $bcdOutput -join "`n"
            $match = [regex]::Match($text, '(?im)^\s*testsigning\s+(?<value>\S+)\s*$')
            if ($match.Success) {
                $value = $match.Groups['value'].Value
                $testSigningEnabled = $value -match '^(?i:yes|oui|on|true|1)$'
            }
        }
    }

    [pscustomobject]@{
        SecureBootEnabled = $secureBootEnabled
        TestSigningEnabled = $testSigningEnabled
    }
}

function Assert-TestSignedDriverLoadPolicy {
    $policy = Get-TestDriverLoadPolicy
    if ($policy.SecureBootEnabled -ne $false) {
        $state = if ($policy.SecureBootEnabled -eq $true) { 'activé' } else { 'indéterminé' }
        throw @"
Le package est auto-signé pour le développement, mais Secure Boot est $state.
Windows 11 ne chargera pas ce pilote dans cette configuration. Utilisez une
machine de test explicitement préparée, ou un package signé par Microsoft.
Ce dépôt ne modifie ni l'UEFI, ni BitLocker, ni le BCD.
"@
    }
    if ($policy.TestSigningEnabled -ne $true) {
        throw @"
Le mode Windows TESTSIGNING n'est pas activé (ou son état n'a pas pu être
vérifié). Un catalogue auto-signé peut être copié dans le Driver Store mais le
pilote kernel ne se chargera pas. Préparez explicitement une machine de test,
redémarrez-la, puis relancez l'installation. Aucun script du dépôt ne change le
BCD.
"@
    }
    return $policy
}

function Get-SoundSpatializerPnpDevices {
    $command = Get-Command Get-PnpDevice -ErrorAction SilentlyContinue
    if (-not $command) {
        throw 'Le module Windows PnpDevice est requis pour identifier le périphérique sans ambiguïté.'
    }

    $prefix = $script:SoundSpatializerHardwareId + '\'
    return @(
        Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object {
                $_.InstanceId -and (
                    $_.InstanceId -ieq $script:SoundSpatializerHardwareId -or
                    $_.InstanceId.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
                )
            }
    )
}
