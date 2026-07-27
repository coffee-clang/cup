# Purpose: Runs every native Windows integration suite in a stable order.

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

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$suiteRoot = Join-Path $projectRoot "tests\integration\windows"
$env:CUP_TEST_CONFIGURATION = $Configuration

$suites = @(Get-ChildItem -LiteralPath $suiteRoot -Filter '*.ps1' -File |
    Sort-Object -Property Name)
if ($suites.Count -eq 0) {
    throw "no Windows integration suites were found"
}
foreach ($suite in $suites) {
    $label = $suite.BaseName.Replace('-', ' ')
    Write-Host "==> Testing $label..."
    & $suite.FullName -CupExecutablePath $resolvedCup
}

Write-Host "All native Windows cup tests passed."
