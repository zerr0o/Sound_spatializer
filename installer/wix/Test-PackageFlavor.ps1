[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $MsiPath,
    [Parameter(Mandatory)] [ValidateSet('Production', 'Preview')] [string] $ExpectedMode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedMsi = [System.IO.Path]::GetFullPath($MsiPath)
if ([System.IO.Path]::GetExtension($resolvedMsi) -ine '.msi' -or
    -not (Test-Path -LiteralPath $resolvedMsi -PathType Leaf)) {
    throw "Le MSI '$resolvedMsi' est introuvable."
}

$installer = $null
$database = $null
$script:msiTables = @{}

function Invoke-InstallerMember {
    param(
        [Parameter(Mandatory)] [object] $Target,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [System.Reflection.BindingFlags] $Flags,
        [Parameter()] [object[]] $Arguments
    )

    return $Target.GetType().InvokeMember($Name, $Flags, $null, $Target, $Arguments)
}

function Read-MsiQuery {
    param(
        [Parameter(Mandatory)] [string] $Sql,
        [Parameter(Mandatory)] [string[]] $Columns
    )

    $view = Invoke-InstallerMember $database 'OpenView' 'InvokeMethod' @($Sql)
    try {
        [void](Invoke-InstallerMember $view 'Execute' 'InvokeMethod' $null)
        $rows = @()
        while ($true) {
            $record = Invoke-InstallerMember $view 'Fetch' 'InvokeMethod' $null
            if ($null -eq $record) {
                break
            }
            try {
                $row = [ordered]@{}
                for ($index = 0; $index -lt $Columns.Count; ++$index) {
                    $row[$Columns[$index]] = [string](Invoke-InstallerMember $record 'StringData' 'GetProperty' @($index + 1))
                }
                $rows += [pscustomobject]$row
            }
            finally {
                [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
            }
        }
        return $rows
    }
    finally {
        try {
            [void](Invoke-InstallerMember $view 'Close' 'InvokeMethod' $null)
        }
        finally {
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
        }
    }
}

function Read-MsiTable {
    param(
        [Parameter(Mandatory)] [string] $Table,
        [Parameter(Mandatory)] [string[]] $Columns
    )

    if (-not $script:msiTables.ContainsKey($Table)) {
        return @()
    }
    $quote = [char]96
    $columnSql = ($Columns | ForEach-Object { "$quote$_$quote" }) -join ','
    return @(Read-MsiQuery "SELECT $columnSql FROM $quote$Table$quote" $Columns)
}

function Assert-Equal {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [AllowEmptyString()] [string] $Actual,
        [AllowEmptyString()] [string] $Expected
    )
    if ($Actual -cne $Expected) {
        throw "$Name invalide: attendu '$Expected', obtenu '$Actual'."
    }
}

try {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = Invoke-InstallerMember $installer 'OpenDatabase' 'InvokeMethod' @($resolvedMsi, 0)

    $quote = [char]96
    foreach ($table in @(Read-MsiQuery "SELECT ${quote}Name${quote} FROM ${quote}_Tables${quote}" @('Name'))) {
        $script:msiTables[$table.Name] = $true
    }

    $properties = @{}
    foreach ($property in @(Read-MsiTable 'Property' @('Property', 'Value'))) {
        $properties[$property.Property] = $property.Value
    }

    $expectedName = if ($ExpectedMode -eq 'Production') {
        'Sound Spatializer'
    }
    else {
        'Sound Spatializer Preview (sans pilote)'
    }
    $expectedUpgradeCode = if ($ExpectedMode -eq 'Production') {
        '{D587A551-E59B-4DC4-9F25-F87F48B9007E}'
    }
    else {
        '{12A2BF16-B16C-4C32-914E-CBA7A5C5D35B}'
    }
    Assert-Equal 'ProductName' ([string]$properties.ProductName) $expectedName
    Assert-Equal 'UpgradeCode' ([string]$properties.UpgradeCode) $expectedUpgradeCode

    $actions = @(Read-MsiTable 'CustomAction' @('Action', 'Type', 'Source', 'Target'))
    $sequence = @(Read-MsiTable 'InstallExecuteSequence' @('Action', 'Condition', 'Sequence'))
    $uiSequence = @(Read-MsiTable 'InstallUISequence' @('Action', 'Condition', 'Sequence'))
    $files = @(Read-MsiTable 'File' @('File', 'FileName', 'Component_'))
    $registrations = @(Read-MsiTable 'Registry' @('Registry', 'Root', 'Key', 'Name', 'Value'))
    $directories = @(Read-MsiTable 'Directory' @('Directory', 'Directory_Parent', 'DefaultDir'))
    $components = @(Read-MsiTable 'Component' @('Component', 'ComponentId', 'Directory_', 'Attributes', 'Condition', 'KeyPath'))
    $shortcuts = @(Read-MsiTable 'Shortcut' @('Shortcut', 'Directory_', 'Name', 'Component_', 'Target', 'Arguments', 'Description', 'WkDir'))
    $upgrades = @(Read-MsiTable 'Upgrade' @('UpgradeCode', 'VersionMin', 'VersionMax', 'Language', 'Attributes', 'Remove', 'ActionProperty'))

    $expectedComponentGuids = if ($ExpectedMode -eq 'Production') {
        @{
            ApplicationComponent = '{4FE17E2A-6410-47C9-9124-2D9CC41E08FD}'
            EngineComponent = '{49900AC3-B2AC-45FB-A4EA-29504306F322}'
            NoticesComponent = '{D826CD48-C9FC-4FF6-A023-FD4A83C4693C}'
            HrtfMetadataComponent = '{0F641129-7445-49C6-B6EC-C63CD56A6C5C}'
            HrtfDataComponent = '{18295B4E-E371-4FA3-B0C4-F4A3037596D5}'
        }
    }
    else {
        @{
            ApplicationComponent = '{4EC7C582-28C4-47E3-8E89-29F345D71696}'
            EngineComponent = '{B8B780E1-DC13-4430-8EE3-F3DBED1FD86B}'
            NoticesComponent = '{0058CD8A-AC45-44DF-B6F4-2830F5EB35F1}'
            HrtfMetadataComponent = '{5070B91F-57BB-47D8-80BD-77DD65AF729B}'
            HrtfDataComponent = '{852C9FC5-F887-42F5-B614-611CAC182578}'
        }
    }
    foreach ($entry in $expectedComponentGuids.GetEnumerator()) {
        $component = @($components | Where-Object { $_.Component -ceq $entry.Key })
        if ($component.Count -ne 1) {
            throw "Le composant MSI '$($entry.Key)' est absent ou duplique."
        }
        Assert-Equal "ComponentId $($entry.Key)" ([string]$component[0].ComponentId) ([string]$entry.Value)
    }

    $driverFilePattern = '(?i)(SoundSpatializerAudio|DriverCtl|Install-Driver|Uninstall-Driver|Complete-DriverInstall|\.inf(?:\||$)|\.sys(?:\||$)|\.cat(?:\||$))'
    $driverActionPattern = '^(RollbackDriver|InstallDriver|CommitDriver|UninstallDriver)$'
    if ($ExpectedMode -eq 'Preview') {
        $forbiddenActions = @($actions | Where-Object { $_.Action -match $driverActionPattern })
        $forbiddenSequence = @($sequence | Where-Object { $_.Action -match $driverActionPattern })
        $forbiddenFiles = @($files | Where-Object { $_.FileName -match $driverFilePattern })
        $forbiddenRegistry = @($registrations | Where-Object { $_.Name -eq 'SoundSpatializer.Engine' })
        $forbiddenDirectories = @($directories | Where-Object { $_.DefaultDir -match '(?i)(?:^|\|)driver$' })
        if ($forbiddenActions.Count -or $forbiddenSequence.Count -or $forbiddenFiles.Count -or
            $forbiddenRegistry.Count -or $forbiddenDirectories.Count) {
            throw 'Le Preview contient encore un payload, une action PnP ou un autostart réservé à Production.'
        }
        if (-not ($directories | Where-Object { $_.DefaultDir -match '(?:^|\|)Sound Spatializer Preview$' })) {
            throw "Le Preview n'utilise pas son dossier Program Files isolé."
        }

        $previewShortcut = @($shortcuts | Where-Object { $_.Shortcut -ceq 'PreviewStartMenuShortcut' })
        if ($previewShortcut.Count -ne 1) {
            throw 'Le raccourci Menu Démarrer Preview est absent ou dupliqué.'
        }
        Assert-Equal 'Shortcut.Directory' ([string]$previewShortcut[0].Directory_) 'ProgramMenuFolder'
        Assert-Equal 'Shortcut.Component' ([string]$previewShortcut[0].Component_) 'ApplicationComponent'
        if ([string]$previewShortcut[0].Name -notmatch '(?:^|\|)Sound Spatializer Preview$') {
            throw "Shortcut.Name invalide: '$($previewShortcut[0].Name)'."
        }
        Assert-Equal 'Shortcut.Target' ([string]$previewShortcut[0].Target) 'Complete'
        Assert-Equal 'Shortcut.WorkingDirectory' ([string]$previewShortcut[0].WkDir) 'INSTALLFOLDER'

        $launchAction = @($actions | Where-Object { $_.Action -ceq 'LaunchPreview' })
        if ($launchAction.Count -ne 1) {
            throw "L'action de lancement Preview est absente ou dupliquée."
        }
        $launchType = [int]$launchAction[0].Type
        if (($launchType -band 0x3F) -ne 18 -or
            ($launchType -band 0xC0) -ne 0xC0 -or
            ($launchType -band 0x700) -ne 0 -or
            ($launchType -band 0x800) -ne 0) {
            throw "LaunchPreview doit rester une action EXE immédiate, asynchrone sans attente et impersonnée (type obtenu: $launchType)."
        }
        Assert-Equal 'LaunchPreview.Source' ([string]$launchAction[0].Source) 'DesktopExecutable'
        Assert-Equal 'LaunchPreview.Target' ([string]$launchAction[0].Target) ''
        if ($sequence.Action -contains 'LaunchPreview') {
            throw "LaunchPreview ne doit jamais être planifiée dans la séquence d'exécution élevée."
        }
        $launchUiSequence = @($uiSequence | Where-Object { $_.Action -ceq 'LaunchPreview' })
        if ($launchUiSequence.Count -ne 1) {
            throw "LaunchPreview doit être planifiée une seule fois dans la séquence UI."
        }
        Assert-Equal 'LaunchPreview.Condition' ([string]$launchUiSequence[0].Condition) 'NOT Installed AND UILevel >= 4 AND NOT REMOVE~="ALL"'
        $executeAction = @($uiSequence | Where-Object { $_.Action -ceq 'ExecuteAction' })
        if ($executeAction.Count -ne 1 -or [int]$launchUiSequence[0].Sequence -le [int]$executeAction[0].Sequence) {
            throw "LaunchPreview doit s'exécuter uniquement après le retour réussi d'ExecuteAction."
        }

        $sameVersionUpgrade = @($upgrades | Where-Object { $_.ActionProperty -ceq 'WIX_UPGRADE_DETECTED' })
        if ($sameVersionUpgrade.Count -ne 1 -or
            $sameVersionUpgrade[0].VersionMax -cne [string]$properties.ProductVersion -or
            (([int]$sameVersionUpgrade[0].Attributes -band 0x200) -eq 0)) {
            throw 'Le Preview ne détecte pas une installation de même version comme mise à niveau majeure remplaçable.'
        }
    }
    else {
        foreach ($requiredAction in @('RollbackDriver', 'InstallDriver', 'CommitDriver', 'UninstallDriver')) {
            if (-not ($actions.Action -contains $requiredAction) -or -not ($sequence.Action -contains $requiredAction)) {
                throw "L'action pilote de production '$requiredAction' est absente du MSI."
            }
        }
        foreach ($requiredFile in @('SoundSpatializerAudio.inf', 'SoundSpatializerAudio.sys', 'SoundSpatializerAudio.cat')) {
            if (-not ($files.FileName | Where-Object { $_ -match "(?i)(?:^|\|)$([regex]::Escape($requiredFile))$" })) {
                throw "Le payload pilote de production '$requiredFile' est absent du MSI."
            }
        }
        if (-not ($registrations | Where-Object { $_.Name -eq 'SoundSpatializer.Engine' })) {
            throw "L'autostart moteur de production est absent du MSI."
        }
        if (($shortcuts | Where-Object { $_.Shortcut -ceq 'PreviewStartMenuShortcut' }) -or
            ($actions | Where-Object { $_.Action -ceq 'LaunchPreview' }) -or
            ($uiSequence | Where-Object { $_.Action -ceq 'LaunchPreview' })) {
            throw 'Production contient à tort le raccourci ou le lancement post-installation réservé au Preview.'
        }
        $productionUpgrade = @($upgrades | Where-Object { $_.ActionProperty -ceq 'WIX_UPGRADE_DETECTED' })
        if ($productionUpgrade.Count -ne 1 -or (([int]$productionUpgrade[0].Attributes -band 0x200) -ne 0)) {
            throw 'Production ne doit pas autoriser implicitement les mises à niveau de même version.'
        }
    }

    [pscustomobject]@{
        Msi = $resolvedMsi
        Mode = $ExpectedMode
        ProductName = $properties.ProductName
        ProductVersion = $properties.ProductVersion
        ProductCode = $properties.ProductCode
        Files = $files.Count
        CustomActions = $actions.Count
        StartMenuShortcut = ($ExpectedMode -eq 'Preview')
        InteractivePostInstallLaunch = ($ExpectedMode -eq 'Preview')
        SameVersionMajorUpgrade = ($ExpectedMode -eq 'Preview')
        RegistryRows = $registrations.Count
        DriverMutation = ($ExpectedMode -eq 'Production')
    }
}
finally {
    if ($database) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    }
    if ($installer) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    }
}
