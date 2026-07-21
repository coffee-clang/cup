# Purpose: Exercises conservative Windows recovery, journal blockers, and
# preservation of foreign-host state and package trees.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "recovery" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $statePath = Join-Path $cupRoot "state.txt"
    $transactionPath = Join-Path $cupRoot "transaction.txt"
    $stagingName = "install-compiler-clang-windows-x64-windows-x64-22.1.5-recovery"
    $stagingPath = Join-Path (Join-Path $cupRoot "staging") $stagingName
    $validState = Get-Content -LiteralPath $statePath

    New-Item -ItemType Directory -Force -Path $stagingPath | Out-Null
    Write-Utf8NoBom -Path $transactionPath -Lines @(
        "format=1",
        "operation=install",
        "component=compiler",
        "tool=clang",
        "host_platform=windows-x64",
        "target_platform=windows-x64",
        "package_version=22.1.5",
        "temporary_name=$stagingName"
    )

    Invoke-Cup -CommandArgs @("help") | Out-Null
    Invoke-Cup -CommandArgs @("--version") | Out-Null
    $blocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $blocked "interrupted package transaction must be repaired first"
    $diagnosis = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $diagnosis "interrupted install transaction detected"

    Write-Utf8NoBom -Path $statePath -Lines @("unexpected.key=value")
    $ambiguous = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $ambiguous "state.txt is missing or invalid while a package transaction is pending"
    Assert-PathExists $transactionPath
    Assert-PathExists $stagingPath
    Write-Utf8NoBom -Path $statePath -Lines $validState
    Remove-Item -LiteralPath $transactionPath -Force
    Remove-Item -LiteralPath $stagingPath -Recurse -Force

    Write-Utf8NoBom -Path $transactionPath -Lines @("not-a-valid-journal")
    $before = (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash
    $invalid = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $invalid "transaction.txt is invalid"
    Assert-PathExists $transactionPath
    Assert-Equals ((Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash) $before
    $stillBlocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $stillBlocked "transaction journal is invalid"
    Remove-Item -LiteralPath $transactionPath -Force

    $foreignHost = "linux-x64"
    $foreignTree = Join-Path $cupRoot "components\compiler\clang\$foreignHost\$foreignHost\22.1.5"
    New-Item -ItemType Directory -Force -Path $foreignTree | Out-Null
    $state = [System.Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $statePath)) { $state.Add($line) }
    $state.Add("installed.compiler.$foreignHost.$foreignHost=clang@22.1.5")
    Write-Utf8NoBom -Path $statePath -Lines $state

    $foreignDoctor = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $foreignDoctor "record(s) for foreign hosts"
    Assert-Contains $foreignDoctor "foreign-host package tree(s)"
    $repair = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $repair "Preserved 1 foreign-host package tree(s)"
    Assert-PathExists $foreignTree
    Assert-Contains ((Get-Content -LiteralPath $statePath) -join "`n") `
        "installed.compiler.$foreignHost.$foreignHost=clang@22.1.5"
    $foreignBlocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $foreignBlocked "foreign host"

    $cleanState = Get-Content -LiteralPath $statePath | Where-Object {
        -not $_.StartsWith("installed.compiler.$foreignHost.$foreignHost=", [StringComparison]::Ordinal)
    }
    Write-Utf8NoBom -Path $statePath -Lines $cleanState
    Remove-Item -LiteralPath (Join-Path $cupRoot "components\compiler\clang\$foreignHost") `
        -Recurse -Force
    Assert-CupHealthy

    Write-Host "Windows recovery tests passed."
} finally {
    Remove-TestEnvironment
}
