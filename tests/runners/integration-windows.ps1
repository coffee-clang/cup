# Runs every native Windows integration suite in a stable order.

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
$syntaxErrors = [System.Collections.Generic.List[string]]::new()
foreach ($tree in @("scripts", "tests")) {
    $treeRoot = Join-Path $projectRoot $tree
    foreach ($file in Get-ChildItem -LiteralPath $treeRoot -Recurse -Filter '*.ps1' -File) {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
        foreach ($parseError in $errors) {
            $syntaxErrors.Add(
                "$($file.FullName):$($parseError.Extent.StartLineNumber): $($parseError.Message)")
        }
    }
}
if ($syntaxErrors.Count -ne 0) {
    foreach ($syntaxError in $syntaxErrors) {
        Write-Error $syntaxError
    }
    throw "$($syntaxErrors.Count) PowerShell syntax error(s) found"
}
Write-Host "PowerShell syntax validation passed."

. (Join-Path $projectRoot "tests\support\windows\common.ps1")
$suiteRoot = Join-Path $projectRoot "tests\integration\windows"
$env:CUP_TEST_CONFIGURATION = $Configuration
$suiteTimeout = 300
if (-not [string]::IsNullOrWhiteSpace($env:CUP_TEST_SUITE_TIMEOUT)) {
    if (-not [int]::TryParse($env:CUP_TEST_SUITE_TIMEOUT, [ref]$suiteTimeout) -or
        $suiteTimeout -lt 1) {
        throw "invalid CUP_TEST_SUITE_TIMEOUT: $($env:CUP_TEST_SUITE_TIMEOUT)"
    }
}
$powershellPath = (Get-Process -Id $PID).Path

$suites = @(Get-ChildItem -LiteralPath $suiteRoot -Filter '*.ps1' -File |
    Sort-Object -Property Name)
if ($suites.Count -eq 0) {
    throw "no Windows integration suites were found"
}
foreach ($suite in $suites) {
    $label = $suite.BaseName.Replace('-', ' ')
    Write-Host "==> Testing $label..."
    $result = Invoke-NativeProcess `
        -FilePath $powershellPath `
        -Arguments @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $suite.FullName,
            "-CupExecutablePath", $resolvedCup) `
        -WorkingDirectory $projectRoot `
        -TimeoutSeconds $suiteTimeout
    if (-not [string]::IsNullOrWhiteSpace($result.Output)) {
        Write-Host $result.Output
    }
    if ($result.ExitCode -ne 0) {
        throw "Windows integration suite failed: $($suite.Name) [$($result.ExitCode)]"
    }
}

Write-Host "All native Windows cup tests passed."
