# Purpose: Exercises read-only Windows diagnosis and proves doctor never repairs observed damage.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

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
    $markerPath = Join-Path $cupRoot "uninstall.pending"

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
    New-Item -ItemType File -Path $markerPath | Out-Null
    $stateHash = (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash

    $issues = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $issues "an uninstall marker exists"
    Assert-Contains $issues "transaction journal is invalid"
    Assert-Contains $issues "installed state record 'linter:clang-tidy@22.1.5' has no valid package"
    Assert-Contains $issues "package metadata for 'compiler:clang@99.0.0' is not read-only"
    Assert-Contains $issues "installed package 'compiler:clang@99.0.0' is not listed"
    Assert-Contains $issues "valid package 'lldb@22.1.5' exists in components but is absent from state.txt"
    Assert-Contains $issues "package path '$invalidPackage' is invalid"
    Assert-Contains $issues "staging directory contains 1 leftover item(s)"
    Assert-Contains $issues "Run 'cup repair' after reviewing them."

    Assert-Equals (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash $stateHash
    Assert-PathExists $invalidPackage
    Assert-PathExists $leftover
    Assert-PathExists $markerPath
    if ((Get-Item -LiteralPath (Join-Path $compilerRoot "info.txt")).IsReadOnly) {
        Fail-Test "doctor changed package metadata permissions"
    }

    Remove-Item -LiteralPath $markerPath, $transactionPath -Force
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

    $resultPath = Join-Path $cupRoot "cup-update-result.txt"
    Write-Utf8NoBom -Path $resultPath -Lines @(
        "format=1", "status=failed", "error=15", "version=0.3.0")
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "the previous cup update failed with error 15 at version 0.3.0"

    Write-Utf8NoBom -Path $resultPath -Lines @("invalid update result")
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) `
        "the previous cup update result is invalid"
    Remove-Item -LiteralPath $resultPath -Force

    Write-Host "Windows doctor tests passed."
} finally {
    Remove-TestEnvironment
}
