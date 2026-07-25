# Purpose: Exercises Windows install-selection defaults and scoped user preferences.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "install-policy" -ExecutablePath $CupExecutablePath

    $initial = Invoke-Cup -CommandArgs @("config")
    Assert-Contains $initial "Install selections for host 'windows-x64', target 'windows-x64'"
    Assert-Contains $initial "compiler           clang"
    Assert-Contains $initial "official default"
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Invoke-Cup -CommandArgs @(
        "config", "set", "compiler", "clang", "--target", "windows-x64") | Out-Null
    $configured = Invoke-Cup -CommandArgs @("config", "--target", "windows-x64")
    Assert-Contains $configured "compiler           clang"
    Assert-Contains $configured "user preference"

    Invoke-Cup -CommandArgs @(
        "config", "reset", "compiler", "--target", "windows-x64") | Out-Null
    $reset = Invoke-Cup -CommandArgs @("config", "--target", "windows-x64")
    Assert-Contains $reset "compiler           clang"
    Assert-Contains $reset "official default"

    Assert-CupStatus -CommandArgs @(
        "config", "set", "compiler", "unknown", "--target", "windows-x64") `
        -ExpectedStatus 3 | Out-Null
    Assert-CupHealthy
    Write-Host "Windows install-policy tests passed."
} finally {
    Remove-TestEnvironment
}
