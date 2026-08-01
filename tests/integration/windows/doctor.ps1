# Purpose: Exercises read-only Windows diagnosis and proves doctor never repairs observed damage.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name "doctor" -ExecutablePath $CupExecutablePath

    $initial = Invoke-Cup -CommandArgs @("doctor")
    Assert-Contains $initial "development cup assets are available"
    Assert-Contains $initial "cup runtime is not initialized"
    Assert-Contains $initial "Doctor found no issues."
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $statePath = Join-Path $cupRoot "state.txt"
    $transactionPath = Join-Path $cupRoot "transaction.txt"

    $compilerRoot = New-InstalledPackageFixture -Component "compiler" -Tool "clang" `
        -Version "99.0.0" -Entries @("clang")
    $debuggerRoot = New-InstalledPackageFixture -Component "debugger" -Tool "lldb" `
        -Version "22.1.5" -Entries @("lldb")
    $invalidPackage = Join-Path $cupRoot (
        "components\linker\lld\windows-x64\windows-x64\22.1.5")
    New-Item -ItemType Directory -Force -Path $invalidPackage | Out-Null
    Write-Utf8NoBom -Path $statePath -Lines @(
        "format=1",
        "installed.compiler.windows-x64.windows-x64=clang@99.0.0",
        "installed.linter.windows-x64.windows-x64=clang-tidy@22.1.5"
    )
    $leftover = Join-Path $cupRoot "staging\leftover"
    New-Item -ItemType Directory -Force -Path $leftover | Out-Null
    Write-Utf8NoBom -Path $transactionPath -Lines @("invalid journal")
    $stateHash = (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash

    $issues = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $issues "transaction journal is invalid"
    Assert-Contains $issues "installed state record 'linter:clang-tidy@22.1.5' has no valid package"
    Assert-Contains $issues "package metadata for 'compiler:clang@99.0.0' is not read-only"
    Assert-Contains $issues "installed package 'compiler:clang@99.0.0' is not listed"
    Assert-Contains $issues (
        "valid package 'lldb@22.1.5' exists in components but is absent " +
        "from state.txt")
    Assert-ContainsPathText $issues "package path '$invalidPackage' is invalid"
    Assert-Contains $issues "staging directory contains 1 leftover item(s)"
    Assert-Contains $issues "Run 'cup repair' after reviewing them."

    Assert-Equals (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash $stateHash
    Assert-PathExists $invalidPackage
    Assert-PathExists $leftover
    if ((Get-Item -LiteralPath (Join-Path $compilerRoot "info.txt")).IsReadOnly) {
        Fail-Test "doctor changed package metadata permissions"
    }

    Remove-Item -LiteralPath $transactionPath -Force
    Remove-Item -LiteralPath $leftover, $invalidPackage -Recurse -Force
    Write-Utf8NoBom -Path $statePath -Lines @(
        "format=1",
        "installed.compiler.windows-x64.windows-x64=clang@99.0.0",
        "installed.debugger.windows-x64.windows-x64=lldb@22.1.5"
    )
    (Get-Item -LiteralPath (Join-Path $compilerRoot "info.txt")).IsReadOnly = $true
    (Get-Item -LiteralPath (Join-Path $debuggerRoot "info.txt")).IsReadOnly = $true

    $cachePath = Join-Path $cupRoot "cache"
    Remove-Item -LiteralPath $cachePath -Recurse -Force
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "cup runtime structure is incomplete"
    New-Item -ItemType Directory -Force -Path $cachePath | Out-Null

    $lockPath = Join-Path $cupRoot "cup.lock"
    Remove-Item -LiteralPath $lockPath -Force
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "cup lock file is missing"

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    $warningOnly = Invoke-Cup -CommandArgs @("doctor")
    Assert-Contains $warningOnly "Doctor found 1 warning(s), but no blocking issues."
    Assert-Contains $warningOnly "installed package 'compiler:clang@99.0.0' is not listed"

    Write-Utf8NoBom -Path $transactionPath -Lines @(
        "format=1",
        "operation=cup-update",
        "phase=failed",
        "temporary_name=cup-update-test",
        "token=fixture-cup-update-test",
        "version=0.3.0",
        "error=15",
        "recovery=pending"
    )
    $updateJournalHash = (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "the previous cup update to version 0.3.0 failed with error 15; recovery is pending"
    Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
        $updateJournalHash

    # Help, version, typos, parse errors and read-only views never rewrite durable evidence.
    Invoke-Cup -CommandArgs @("help") | Out-Null
    Invoke-Cup -CommandArgs @("--version") | Out-Null
    Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
        $updateJournalHash
    Invoke-Cup -CommandArgs @("not-a-command") -ExpectFailure | Out-Null
    Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
        $updateJournalHash
    Invoke-Cup -CommandArgs @("install") -ExpectFailure | Out-Null
    Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
        $updateJournalHash
    foreach ($readOnlyCommand in @("search", "list", "config", "info", "inspect")) {
        Invoke-Cup -CommandArgs @($readOnlyCommand) -ExpectFailure | Out-Null
        Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
            $updateJournalHash
    }

    Write-Utf8NoBom -Path $transactionPath -Lines @(
        "format=1",
        "operation=cup-update",
        "phase=failed",
        "temporary_name=cup-update-test",
        "token=fixture-cup-update-test",
        "version=NEWER",
        "error=15",
        "recovery=pending"
    )
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "cup update journal is invalid"
    Remove-Item -LiteralPath $transactionPath -Force

    Write-Utf8NoBom -Path $transactionPath -Lines @(
        "format=1",
        "operation=uninstall",
        "phase=failed",
        "temporary_name=.cup-uninstall.fixture",
        "token=fixture",
        "stage=cleanup",
        "error=1"
    )
    $uninstallJournalHash = (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "the previous cup uninstall failed during 'cleanup' with error 1"
    Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
        $uninstallJournalHash
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "the previous cup uninstall failed during 'cleanup' with error 1"
    Assert-Equals (Get-FileHash -LiteralPath $transactionPath -Algorithm SHA256).Hash `
        $uninstallJournalHash
    Remove-Item -LiteralPath $transactionPath -Force

    Write-Host "Windows doctor tests passed."
} finally {
    Remove-TestEnvironment
}
