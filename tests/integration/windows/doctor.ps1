# Purpose: Verifies native Windows doctor diagnostics are read-only and strict.
param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "doctor" -ExecutablePath $CupExecutablePath

    $before = Invoke-Cup -CommandArgs @("doctor")
    Assert-Contains $before "runtime is not initialized"
    Assert-Contains $before "Doctor found no issues."
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-CupHealthy

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $statePath = Join-Path $cupRoot "state.txt"
    $journalPath = Join-Path $cupRoot "transaction.txt"
    $stagingPath = Join-Path $cupRoot "staging\leftover"
    $markerPath = Join-Path $cupRoot "uninstall.pending"

    New-Item -ItemType Directory -Force -Path $stagingPath | Out-Null
    Write-Utf8NoBom -Path $journalPath -Lines @("invalid journal")
    New-Item -ItemType File -Force -Path $markerPath | Out-Null
    $stateHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $statePath).Hash

    $output = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $output "uninstall marker"
    Assert-Contains $output "transaction journal is invalid"
    Assert-Contains $output "staging directory contains"
    Assert-Equals (Get-FileHash -Algorithm SHA256 -LiteralPath $statePath).Hash $stateHash
    Assert-PathExists $journalPath
    Assert-PathExists $stagingPath
    Assert-PathExists $markerPath

    Remove-Item -LiteralPath $journalPath, $markerPath -Force
    Remove-Item -LiteralPath $stagingPath -Recurse -Force
    Assert-CupHealthy
    Write-Host "Windows doctor tests passed."
} finally {
    Remove-TestEnvironment
}
