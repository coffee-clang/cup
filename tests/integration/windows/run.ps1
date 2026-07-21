# Purpose: Orders all native Windows integration suites for make and CI.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupPath
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
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path

$syntaxErrors = [System.Collections.Generic.List[string]]::new()
foreach ($tree in @("scripts", "tests")) {
    foreach ($file in Get-ChildItem (Join-Path $projectRoot $tree) -Recurse -Filter "*.ps1") {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
        foreach ($parseError in $errors) {
            $syntaxErrors.Add("$($file.FullName):$($parseError.Extent.StartLineNumber): $($parseError.Message)")
        }
    }
}
if ($syntaxErrors.Count -gt 0) {
    $details = $syntaxErrors -join [Environment]::NewLine
    throw "PowerShell syntax validation failed:`n$details"
}
Write-Host "PowerShell syntax validation passed."

$suites = @(
    "harness.ps1",
    "release-metadata.ps1",
    "commands.ps1",
    "filesystem-archives.ps1",
    "state.ps1",
    "wrappers.ps1",
    "recovery.ps1"
)
foreach ($suite in $suites) {
    Write-Host "==> Running Windows $suite"
    & (Join-Path $PSScriptRoot $suite) -CupExecutablePath $resolvedCup
}
Write-Host "All native Windows cup tests passed."
