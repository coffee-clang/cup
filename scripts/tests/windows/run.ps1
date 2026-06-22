param([Parameter(Mandatory = $true)][string]$CupPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedCup = (Resolve-Path $CupPath).Path
$suites = @("commands.ps1", "state.ps1", "entrypoints.ps1")
foreach ($suite in $suites) {
    Write-Host "==> Running Windows $suite"
    & (Join-Path $PSScriptRoot $suite) -CupPath $resolvedCup
}
Write-Host "All native Windows cup tests passed."
