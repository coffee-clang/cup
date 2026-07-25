# Purpose: Exercises public Windows CLI dispatch, help aliases, and stable exit statuses.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "cli-contract" -ExecutablePath $CupExecutablePath
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version "21.1.5"

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Assert-PathMissing $cupRoot
    Assert-Contains (Invoke-Cup -CommandArgs @("config")) "official default"
    Assert-Contains (Invoke-Cup -CommandArgs @("list")) "No packages installed"
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor")) "runtime is not initialized"
    Assert-PathMissing $cupRoot

    Assert-CupStatus -CommandArgs @("Unknown") -ExpectedStatus 2 `
        -ExpectedText "unknown command 'Unknown'" | Out-Null
    Assert-CupStatus -CommandArgs @("inspect", "compiler", "clang@21.1.5") `
        -ExpectedStatus 3 | Out-Null
    Assert-CupStatus -CommandArgs @("default") -ExpectedStatus 2 `
        -ExpectedText "missing option <component>" | Out-Null
    Assert-PathMissing $cupRoot

    Assert-Contains (Invoke-Cup -CommandArgs @("--help")) "Commands:"
    Assert-Contains (Invoke-Cup -CommandArgs @("help")) "Commands:"
    $installHelp = Invoke-Cup -CommandArgs @("install", "--help")
    Assert-Contains $installHelp "Effects:"
    Assert-Contains $installHelp "Select tar.xz, tar.gz or zip."
    Assert-Contains (Invoke-Cup -CommandArgs @("help", "config")) `
        "reset without component clears that scope only"

    Write-Host "Windows CLI contract tests passed."
} finally {
    Remove-TestEnvironment
}
