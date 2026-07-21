[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

Assert-Administrator
$rollbackStatePath = Get-DriverRollbackStatePath
if (Test-Path -LiteralPath $rollbackStatePath -PathType Leaf) {
    try {
        $rollbackState = Get-Content -LiteralPath $rollbackStatePath -Raw | ConvertFrom-Json
        if ([int]$rollbackState.schemaVersion -ne 1) {
            throw "Version inconnue de l'état de rollback pilote."
        }

        $newPublishedInf = [string]$rollbackState.newPublishedInf
        if ($newPublishedInf -match '^oem\d+\.inf$') {
            # The previous package must remain available throughout the MSI
            # transaction. It becomes obsolete only once this commit action runs.
            $obsoletePackages = @(
                Get-PublishedDriverNames |
                    Where-Object { $_ -ine $newPublishedInf }
            )
            Remove-PublishedDriverPackages -Names $obsoletePackages
        }
        else {
            Write-Warning "Le nouvel oem*.inf n'a pas pu être identifié; les anciens packages sont conservés par sécurité."
        }
    }
    catch {
        # A commit action runs only after the MSI transaction succeeded. Driver
        # store housekeeping must never leave rollback data that a later MSI
        # transaction could mistake for its own snapshot.
        Write-Warning "Nettoyage post-commit incomplet: $($_.Exception.Message)"
    }
    finally {
        Remove-Item -LiteralPath $rollbackStatePath -Force
    }
}
Write-Host "Transaction pilote validée; anciens packages obsolètes nettoyés et état de rollback supprimé."
