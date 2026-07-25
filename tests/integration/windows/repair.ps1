# Purpose: Exercises deterministic Windows repair, state reconstruction, quarantine,
# stale cleanup, and foreign-host preservation.

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

    $compilerRoot = New-InstalledPackageFixture -Component "compiler" -Tool "clang" `
        -Version "22.1.5" -Entries @("clang")
    $adopted = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $adopted "Adopted valid package 'compiler:clang@22.1.5'"
    Assert-Contains $adopted "Restored read-only protection for clang@22.1.5 metadata."
    Assert-Contains ((Get-Content -LiteralPath $statePath) -join "`n") `
        "installed.compiler.windows-x64.windows-x64=clang@22.1.5"
    if (-not (Get-Item -LiteralPath (Join-Path $compilerRoot "info.txt")).IsReadOnly) {
        Fail-Test "repair did not protect adopted package metadata"
    }

    $state = [System.Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $statePath)) { $state.Add($line) }
    $state.Add("installed.debugger.windows-x64.windows-x64=lldb@22.1.5")
    $state.Add("default.debugger.windows-x64.windows-x64=lldb@22.1.5")
    Write-Utf8NoBom -Path $statePath -Lines $state
    $stale = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $stale "Removed stale state record 'debugger:lldb@22.1.5'."
    Assert-NotContains ((Get-Content -LiteralPath $statePath) -join "`n") "lldb@22.1.5"

    $invalidPackage = Join-Path $cupRoot (
        "components\debugger\lldb\windows-x64\windows-x64\22.1.5")
    $malformedRoot = Join-Path $cupRoot "components\unknown-component"
    New-Item -ItemType Directory -Force -Path $invalidPackage, $malformedRoot | Out-Null
    $quarantine = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $quarantine "Quarantined invalid package '$invalidPackage'"
    Assert-Contains $quarantine "unknown component"
    Assert-PathMissing $invalidPackage
    $preserved = Get-ChildItem (Join-Path $cupRoot "recovery") -Directory -Recurse `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq "package" }
    if (@($preserved).Count -eq 0) {
        Fail-Test "quarantined package was not preserved under recovery"
    }
    Assert-PathExists $malformedRoot
    Remove-Item -LiteralPath $malformedRoot -Recurse -Force
    Assert-PathExists (Join-Path $compilerRoot "info.txt")

    Write-Utf8NoBom -Path $statePath -Lines @("unexpected.key=value")
    $rebuilt = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $rebuilt "Preserved invalid state as"
    Assert-PathExists "$statePath.invalid"
    Assert-Contains ((Get-Content -LiteralPath $statePath) -join "`n") "clang@22.1.5"

    $stagingLeftover = Join-Path $cupRoot "staging\stale-data"
    New-Item -ItemType Directory -Force -Path $stagingLeftover | Out-Null
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-PathMissing $stagingLeftover

    $foreignHost = "linux-x64"
    $foreignTree = Join-Path $cupRoot (
        "components\compiler\clang\$foreignHost\$foreignHost\22.1.5")
    New-Item -ItemType Directory -Force -Path $foreignTree | Out-Null
    $state = [System.Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $statePath)) { $state.Add($line) }
    $state.Add("installed.compiler.$foreignHost.$foreignHost=clang@22.1.5")
    Write-Utf8NoBom -Path $statePath -Lines $state

    $foreignDoctor = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $foreignDoctor "record(s) for foreign hosts"
    Assert-Contains $foreignDoctor "foreign-host package tree(s)"
    $foreignRepair = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $foreignRepair "Preserved 1 foreign-host package tree(s)"
    Assert-PathExists $foreignTree
    Assert-Contains ((Get-Content -LiteralPath $statePath) -join "`n") `
        "installed.compiler.$foreignHost.$foreignHost=clang@22.1.5"
    Assert-Contains (Invoke-Cup -CommandArgs @("list") -ExpectFailure) "foreign host"

    $cleanState = Get-Content -LiteralPath $statePath | Where-Object {
        -not $_.StartsWith(
            "installed.compiler.$foreignHost.$foreignHost=",
            [StringComparison]::Ordinal)
    }
    Write-Utf8NoBom -Path $statePath -Lines $cleanState
    Remove-Item -LiteralPath (Join-Path $cupRoot "components\compiler\clang\$foreignHost") `
        -Recurse -Force
    Assert-CupHealthy

    Write-Host "Windows repair tests passed."
} finally {
    Remove-TestEnvironment
}
