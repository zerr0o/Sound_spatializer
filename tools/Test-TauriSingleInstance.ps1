[CmdletBinding()]
param(
    [Parameter()]
    [string]$Executable = "apps/desktop/src-tauri/target/release/sound-spatializer-desktop.exe",

    [Parameter()]
    [ValidateRange(1, 30)]
    [int]$TimeoutSeconds = 10
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$candidate = if ([System.IO.Path]::IsPathRooted($Executable)) {
    $Executable
} else {
    Join-Path $repositoryRoot $Executable
}
$resolved = (Resolve-Path -LiteralPath $candidate).Path

$existing = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq "sound-spatializer-desktop.exe"
})
if ($existing.Count -ne 0) {
    throw "Fermez Sound Spatializer avant le test mono-instance."
}

$first = $null
$second = $null
try {
    $first = Start-Process -FilePath $resolved -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 100
        $first.Refresh()
    } while (-not $first.HasExited -and
        [string]::IsNullOrWhiteSpace($first.MainWindowTitle) -and
        [DateTime]::UtcNow -lt $deadline)

    if ($first.HasExited) {
        throw "La premiere instance Tauri s'est arretee pendant son demarrage."
    }

    $second = Start-Process -FilePath $resolved -PassThru
    if (-not $second.WaitForExit($TimeoutSeconds * 1000)) {
        throw "La seconde instance Tauri ne s'est pas arretee."
    }

    $first.Refresh()
    $instances = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -eq "sound-spatializer-desktop.exe" -and $_.ExecutablePath -eq $resolved
    })
    if (-not $first.Responding -or $first.MainWindowTitle -ne "Sound Spatializer" -or $instances.Count -ne 1) {
        throw "Le contrat mono-instance/focus de la fenetre principale a echoue."
    }

    Write-Host "Protection mono-instance Tauri verifiee."
} finally {
    if ($second -and -not $second.HasExited) {
        Stop-Process -Id $second.Id -Force -ErrorAction SilentlyContinue
    }
    if ($first -and -not $first.HasExited) {
        Stop-Process -Id $first.Id -Force -ErrorAction SilentlyContinue
    }
}
