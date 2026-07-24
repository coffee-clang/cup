# Purpose: Verifies deterministic native Windows repair and cleanup behavior.
param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "repair" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $statePath = Join-Path $cupRoot "state.txt"
    $stagingPath = Join-Path $cupRoot "staging\stale-data"
    New-Item -ItemType Directory -Force -Path $stagingPath | Out-Null

    Write-Utf8NoBom -Path $statePath -Lines @("unexpected.key=value")
    $output = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $output "Preserved invalid state as"
    Assert-PathExists "$statePath.invalid"
    Assert-PathMissing $stagingPath
    Assert-CupHealthy

    $journalPath = Join-Path $cupRoot "transaction.txt"
    Write-Utf8NoBom -Path $journalPath -Lines @("not-a-valid-journal")
    $failure = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $failure "transaction.txt is invalid"
    Assert-PathExists $journalPath

    Remove-Item -LiteralPath $journalPath -Force
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-CupHealthy
    Write-Host "Windows repair tests passed."
} finally {
    Remove-TestEnvironment
}
