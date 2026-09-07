# Runs every native Windows integration suite in a stable order.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupPath,

    [Parameter(Mandatory = $true)]
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
        Write-Error $syntaxError -ErrorAction Continue
    }
    throw "$($syntaxErrors.Count) PowerShell syntax error(s) found"
}
Write-Host "PowerShell syntax validation passed."

. (Join-Path $projectRoot "tests\support\windows\common.ps1")
Assert-TestConfiguration -Configuration $Configuration

function Test-FailedInitializationCleanup {
    $savedProfile = Get-Item -LiteralPath Env:USERPROFILE -ErrorAction SilentlyContinue
    $savedAllowInsecure = Get-Item -LiteralPath Env:CUP_INSTALL_ALLOW_INSECURE `
        -ErrorAction SilentlyContinue
    $profileSentinel = $projectRoot
    $allowInsecureSentinel = 'outer-environment-sentinel'
    $missingExecutable = Join-Path $projectRoot (
        'missing-cup-' + [Guid]::NewGuid().ToString('N') + '.exe')

    try {
        $env:USERPROFILE = $profileSentinel
        $env:CUP_INSTALL_ALLOW_INSECURE = $allowInsecureSentinel
        $failed = $false
        try {
            Initialize-TestEnvironment -Name 'initialization-failure' `
                -ExecutablePath $missingExecutable
        } catch {
            $failed = $true
        } finally {
            Remove-TestEnvironment
        }

        if (-not $failed) {
            throw 'Windows test framework accepted a missing CUP executable'
        }
        if ($env:USERPROFILE -cne $profileSentinel) {
            throw 'Failed Windows test initialization did not preserve USERPROFILE'
        }
        if ($env:CUP_INSTALL_ALLOW_INSECURE -cne $allowInsecureSentinel) {
            throw 'Failed Windows test initialization did not preserve managed environment'
        }
    } finally {
        if ($null -eq $savedProfile) {
            Remove-Item -LiteralPath Env:USERPROFILE -ErrorAction SilentlyContinue
        } else {
            $env:USERPROFILE = $savedProfile.Value
        }
        if ($null -eq $savedAllowInsecure) {
            Remove-Item -LiteralPath Env:CUP_INSTALL_ALLOW_INSECURE `
                -ErrorAction SilentlyContinue
        } else {
            $env:CUP_INSTALL_ALLOW_INSECURE = $savedAllowInsecure.Value
        }
    }
}

Test-FailedInitializationCleanup
Write-Host "Windows test-framework failure cleanup passed."
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
$failedSuites = [System.Collections.Generic.List[string]]::new()
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
        $failedSuites.Add("$($suite.Name) [$($result.ExitCode)]")
    }
}

if ($failedSuites.Count -ne 0) {
    $failureText = $failedSuites -join "`n"
    throw "Windows integration suite failures:`n$failureText"
}

Write-Host "All native Windows cup tests passed."
