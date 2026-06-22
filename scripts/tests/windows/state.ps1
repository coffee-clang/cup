param([Parameter(Mandatory = $true)][string]$CupPath)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "state" -CupPath $CupPath
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

    $statePath = Join-Path $Script:TestHome ".cup\state.txt"
    $state = Get-Content -LiteralPath $statePath
    $installedCount = ($state | Where-Object { $_.StartsWith("installed.") }).Count
    $defaultCount = ($state | Where-Object { $_.StartsWith("default.") }).Count
    Assert-Equals ([string]$installedCount) "4"
    Assert-Equals ([string]$defaultCount) "3"

    $installed = Invoke-Cup -CommandArgs @("list")
    Assert-Contains $installed "compiler:clang@21.1.5"
    Assert-Contains $installed "compiler:clang@22.1.5"
    Assert-Contains $installed "debugger:gdb@17.1"
    Assert-Contains $installed "linker:lld@22.1.5"

    Copy-Item $statePath "$statePath.valid"
    $first = $state | Where-Object { $_.StartsWith("installed.") } | Select-Object -First 1
    Add-Content -LiteralPath $statePath -Value $first
    $failure = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $failure "state.txt"
    Copy-Item "$statePath.valid" $statePath -Force
    Invoke-Cup -CommandArgs @("doctor") | Out-Null

    Write-Host "Windows state tests passed."
} finally {
    Remove-TestEnvironment
}
