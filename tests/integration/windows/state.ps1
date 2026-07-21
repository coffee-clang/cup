# Purpose: Exercises Windows state persistence and invalid-state handling.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "state" -ExecutablePath $CupExecutablePath
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version "21.1.5"
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    New-TestPackage -Component "compiler" -Tool "clang" -Version "21.1.5" -Entries @("clang")
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" -Entries @("clang")
    New-TestPackage -Component "debugger" -Tool "gdb" -Version "17.1" -Entries @("gdb")
    New-TestPackage -Component "linker" -Tool "lld" -Version "22.1.5" -Entries @("lld")

    Invoke-Cup -CommandArgs @("install", "compiler", "clang@21.1.5") | Out-Null
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@stable") | Out-Null
    Invoke-Cup -CommandArgs @("install", "debugger", "gdb@stable") | Out-Null
    Invoke-Cup -CommandArgs @("install", "linker", "lld@stable") | Out-Null

    $statePath = Join-Path $Script:CupTestHome ".cup\state.txt"
    $state = Get-Content -LiteralPath $statePath
    Assert-Equals $state[0] "format=1"
    $installedCount = ($state | Where-Object { $_.StartsWith("installed.") }).Count
    $defaultCount = ($state | Where-Object { $_.StartsWith("default.") }).Count
    Assert-Equals ([string]$installedCount) "4"
    Assert-Equals ([string]$defaultCount) "3"

    $stateText = $state -join "`n"
    Assert-Contains $stateText "installed.compiler.windows-x64.windows-x64=clang@21.1.5"
    Assert-Contains $stateText "installed.compiler.windows-x64.windows-x64=clang@22.1.5"
    Assert-Contains $stateText "installed.debugger.windows-x64.windows-x64=gdb@17.1"
    Assert-Contains $stateText "installed.linker.windows-x64.windows-x64=lld@22.1.5"
    Assert-Contains $stateText "default.compiler.windows-x64.windows-x64=clang@21.1.5"
    Assert-Contains $stateText "default.debugger.windows-x64.windows-x64=gdb@17.1"
    Assert-Contains $stateText "default.linker.windows-x64.windows-x64=lld@22.1.5"

    Copy-Item $statePath "$statePath.valid"

    function Assert-InvalidStateRejected {
        $failure = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
        Assert-Contains $failure "state.txt"
        Copy-Item "$statePath.valid" $statePath -Force
        Assert-CupHealthy
    }

    $firstInstalled = $state | Where-Object { $_.StartsWith("installed.") } |
        Select-Object -First 1
    Add-Content -LiteralPath $statePath -Value $firstInstalled
    Assert-InvalidStateRejected

    $firstDefault = $state | Where-Object { $_.StartsWith("default.") } |
        Select-Object -First 1
    Add-Content -LiteralPath $statePath -Value $firstDefault
    Assert-InvalidStateRejected

    $orphaned = Get-Content -LiteralPath $statePath | ForEach-Object {
        if ($_.StartsWith("default.compiler.")) {
            $_ -replace "=.*$", "=clang@99.0.0"
        } else {
            $_
        }
    }
    Write-Utf8NoBom -Path $statePath -Lines $orphaned
    Assert-InvalidStateRejected

    Add-Content -LiteralPath $statePath -Value "unexpected.key=value"
    Assert-InvalidStateRejected

    Write-Host "Windows state tests passed."
} finally {
    Remove-TestEnvironment
}
