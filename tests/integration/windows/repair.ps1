# Exercises deterministic Windows repair, state reconstruction, quarantine,
# journal ambiguity, stale cleanup, and foreign-host preservation.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function Test-PackageAdoption {
    $Script:RepairCompilerRoot = New-InstalledPackageFixture `
        -Component "compiler" -Tool "clang" -Version "22.1.5" -Entries @("clang")

    $adopted = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $adopted "Adopted valid package 'compiler:clang@22.1.5'"
    Assert-Contains $adopted "Restored read-only protection for clang@22.1.5 metadata."
    Assert-Contains ((Get-Content -LiteralPath $Script:RepairStatePath) -join "`n") `
        "installed.compiler.windows-x64.windows-x64=clang@22.1.5"

    $infoPath = Join-Path $Script:RepairCompilerRoot "info.txt"
    if (-not (Get-Item -LiteralPath $infoPath).IsReadOnly) {
        Fail-Test "repair did not protect adopted package metadata"
    }
}

function Test-StaleStateRemoval {
    $state = [System.Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $Script:RepairStatePath)) {
        $state.Add($line)
    }
    $state.Add("installed.debugger.windows-x64.windows-x64=lldb@22.1.5")
    $state.Add("default.debugger.windows-x64.windows-x64=lldb@22.1.5")
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines $state

    $stale = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $stale "Removed stale state record 'debugger:lldb@22.1.5'."
    Assert-NotContains ((Get-Content -LiteralPath $Script:RepairStatePath) -join "`n") `
        "lldb@22.1.5"
}

function Test-InvalidPackageQuarantine {
    $invalidPackage = Join-Path $Script:RepairCupRoot (
        "components\debugger\lldb\windows-x64\windows-x64\22.1.5")
    $malformedRoot = Join-Path $Script:RepairCupRoot "components\unknown-component"
    New-Item -ItemType Directory -Force -Path $invalidPackage, $malformedRoot | Out-Null

    $quarantine = Invoke-Cup -CommandArgs @("repair")
    Assert-ContainsPathText $quarantine "Quarantined invalid package '$invalidPackage'"
    Assert-Contains $quarantine "unknown component"
    Assert-PathMissing $invalidPackage

    $preserved = Get-ChildItem (Join-Path $Script:RepairCupRoot "recovery") `
        -Directory -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq "package" }
    if (@($preserved).Count -eq 0) {
        Fail-Test "quarantined package was not preserved under recovery"
    }

    Assert-PathExists $malformedRoot
    Remove-Item -LiteralPath $malformedRoot -Recurse -Force
    Assert-PathExists (Join-Path $Script:RepairCompilerRoot "info.txt")
}

function Test-InvalidStateRebuild {
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines @("unexpected.key=value")

    $rebuilt = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $rebuilt "Preserved invalid state as"
    Assert-PathExists "$($Script:RepairStatePath).invalid"
    Assert-Contains ((Get-Content -LiteralPath $Script:RepairStatePath) -join "`n") `
        "clang@22.1.5"

    $Script:RepairValidState = @(Get-Content -LiteralPath $Script:RepairStatePath)
}

function Restore-ValidRepairState {
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines $Script:RepairValidState
    Remove-Item -LiteralPath "$($Script:RepairStatePath).invalid" `
        -Force -ErrorAction SilentlyContinue
}

function Test-PendingTransactionBlocksInvalidState {
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines @("unexpected.key=value")
    Write-Utf8NoBom -Path $Script:RepairTransactionPath -Lines @(
        "format=1",
        "operation=install",
        "component=compiler",
        "tool=clang",
        "host_platform=windows-x64",
        "target_platform=windows-x64",
        "package_version=22.1.5",
        "temporary_name=install-compiler-clang-windows-x64-windows-x64-22.1.5-test"
    )

    $failure = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $failure `
        "state.txt is missing or invalid while a state-owning transaction is pending"

    Remove-Item -LiteralPath $Script:RepairTransactionPath -Force
    Restore-ValidRepairState
}

function Test-MalformedUpdateBlocksInvalidState {
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines @("unexpected.key=value")
    Write-Utf8NoBom -Path $Script:RepairTransactionPath -Lines @(
        "format=1",
        "operation=cup-update",
        "phase=failed"
    )

    $stateHash = (Get-FileHash -LiteralPath $Script:RepairStatePath -Algorithm SHA256).Hash
    $journalHash = (
        Get-FileHash -LiteralPath $Script:RepairTransactionPath -Algorithm SHA256).Hash

    $failure = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $failure "cup update journal is invalid"
    Assert-Equals (
        (Get-FileHash -LiteralPath $Script:RepairStatePath -Algorithm SHA256).Hash) `
        $stateHash
    Assert-Equals (
        (Get-FileHash -LiteralPath $Script:RepairTransactionPath -Algorithm SHA256).Hash) `
        $journalHash
    Assert-PathMissing "$($Script:RepairStatePath).invalid"

    Remove-Item -LiteralPath $Script:RepairTransactionPath -Force
    Restore-ValidRepairState
}

function Test-MalformedJournalPreservesEvidence {
    Write-Utf8NoBom -Path $Script:RepairTransactionPath -Lines @("not-a-valid-journal")
    $ambiguousStaging = Join-Path $Script:RepairCupRoot "staging\ambiguous-data"
    New-Item -ItemType Directory -Force -Path $ambiguousStaging | Out-Null
    $stateHash = (Get-FileHash -LiteralPath $Script:RepairStatePath -Algorithm SHA256).Hash

    $failure = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $failure "transaction.txt is invalid"
    Assert-PathExists $Script:RepairTransactionPath
    Assert-Equals (
        (Get-FileHash -LiteralPath $Script:RepairStatePath -Algorithm SHA256).Hash) `
        $stateHash
    Assert-PathExists $ambiguousStaging

    $blocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $blocked "transaction journal is invalid"

    Remove-Item -LiteralPath $ambiguousStaging -Recurse -Force
    Remove-Item -LiteralPath $Script:RepairTransactionPath -Force
}

function Test-StaleStagingCleanup {
    $stagingLeftover = Join-Path $Script:RepairCupRoot "staging\stale-data"
    New-Item -ItemType Directory -Force -Path $stagingLeftover | Out-Null

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-PathMissing $stagingLeftover
}

function Test-ForeignHostPreservation {
    $foreignHost = "linux-x64"
    $foreignTree = Join-Path $Script:RepairCupRoot (
        "components\compiler\clang\$foreignHost\$foreignHost\22.1.5")
    New-Item -ItemType Directory -Force -Path $foreignTree | Out-Null

    $state = [System.Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $Script:RepairStatePath)) {
        $state.Add($line)
    }
    $state.Add("installed.compiler.$foreignHost.$foreignHost=clang@22.1.5")
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines $state

    $foreignDoctor = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $foreignDoctor "record(s) for foreign hosts"
    Assert-Contains $foreignDoctor "foreign-host package tree(s)"

    $foreignRepair = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $foreignRepair "Preserved 1 foreign-host package tree(s)"
    Assert-PathExists $foreignTree
    Assert-Contains ((Get-Content -LiteralPath $Script:RepairStatePath) -join "`n") `
        "installed.compiler.$foreignHost.$foreignHost=clang@22.1.5"
    Assert-Contains (Invoke-Cup -CommandArgs @("list") -ExpectFailure) "foreign host"

    $cleanState = Get-Content -LiteralPath $Script:RepairStatePath | Where-Object {
        -not $_.StartsWith(
            "installed.compiler.$foreignHost.$foreignHost=",
            [StringComparison]::Ordinal)
    }
    Write-Utf8NoBom -Path $Script:RepairStatePath -Lines $cleanState
    Remove-Item -LiteralPath (
        Join-Path $Script:RepairCupRoot "components\compiler\clang\$foreignHost") `
        -Recurse -Force
}

function Test-UninstallRecovery {
    Write-Utf8NoBom -Path $Script:RepairTransactionPath -Lines @(
        "format=1",
        "operation=uninstall",
        "phase=scheduled",
        "temporary_name=.cup-uninstall.fixture",
        "token=fixture",
        "stage=parent-wait",
        "error=0"
    )
    $pending = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $pending `
        "Cancelled interrupted cup uninstall in phase 'scheduled' during 'parent-wait'."
    Assert-PathMissing $Script:RepairTransactionPath

    Write-Utf8NoBom -Path $Script:RepairTransactionPath -Lines @(
        "format=1",
        "operation=uninstall",
        "phase=failed",
        "temporary_name=.cup-uninstall.fixture",
        "token=fixture",
        "stage=handoff",
        "error=7"
    )
    Assert-Contains (Invoke-Cup -CommandArgs @("repair")) `
        "Acknowledged failed cup uninstall during 'handoff' (error 7)."
    Assert-PathMissing $Script:RepairTransactionPath
    Assert-CupHealthy
}

try {
    Initialize-TestEnvironment -Name "repair" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $Script:RepairCupRoot = Join-Path $Script:CupTestHome ".cup"
    $Script:RepairStatePath = Join-Path $Script:RepairCupRoot "state.txt"
    $Script:RepairTransactionPath = Join-Path $Script:RepairCupRoot "transaction.txt"
    $Script:RepairValidState = @()

    Test-PackageAdoption
    Test-StaleStateRemoval
    Test-InvalidPackageQuarantine
    Test-InvalidStateRebuild
    Test-PendingTransactionBlocksInvalidState
    Test-MalformedUpdateBlocksInvalidState
    Test-MalformedJournalPreservesEvidence
    Test-StaleStagingCleanup
    Test-ForeignHostPreservation
    Test-UninstallRecovery

    Write-Host "Windows repair tests passed."
} finally {
    Remove-TestEnvironment
}
