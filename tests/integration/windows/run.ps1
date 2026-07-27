# Purpose: Orders all native Windows integration suites for make and CI.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet("development", "debug", "coverage", "sanitizers", "release")]
    [string]$Configuration
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($CupPath)) {
    throw "cup executable path is empty"
}
$resolvedCup = (Resolve-Path -LiteralPath $CupPath).Path
if ([string]::IsNullOrWhiteSpace($resolvedCup)) {
    throw "failed to resolve cup executable path"
}
$env:CUP_TEST_CONFIGURATION = $Configuration
$suites = @(
    "cli-contract.ps1",
    "package-catalog.ps1",
    "package-lifecycle.ps1",
    "install-policy.ps1",
    "state.ps1",
    "wrappers.ps1",
    "filesystem-archives.ps1",
    "recovery.ps1",
    "repair.ps1",
    "doctor.ps1",
    "concurrency.ps1",
    "uninstall.ps1"
)
foreach ($suite in $suites) {
    Write-Host "==> Running Windows $suite"
    & (Join-Path $PSScriptRoot $suite) -CupExecutablePath $resolvedCup
}
Write-Host "All native Windows cup tests passed."
