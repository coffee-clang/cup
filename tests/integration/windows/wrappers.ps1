# Purpose: Exercises Windows .cmd managed wrappers and native quoting behavior.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "wrappers" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" -Entries @("clang", "clang++")
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@stable") | Out-Null

    Assert-Equals (Invoke-ManagedCommand -Name "clang") "clang-22.1.5-windows-x64:clang"
    $wrapper = Join-Path $Script:CupTestHome ".cup\bin\clang.cmd"
    Set-Content -LiteralPath $wrapper -Value "@echo altered" -Encoding ascii
    Assert-Contains (Invoke-Cup -CommandArgs @("info") -ExpectFailure) "status: invalid"
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) "wrapper"
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-Equals (Invoke-ManagedCommand -Name "clang") "clang-22.1.5-windows-x64:clang"

    $stale = Join-Path $Script:CupTestHome ".cup\bin\stale-command.cmd"
    Set-Content -LiteralPath $stale -Value "@echo off`r`nexit /b 0`r`n" -Encoding ascii -NoNewline
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) "stale or unmanaged wrapper"
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-PathMissing $stale

    New-TestPackage -Component "linker" -Tool "lld" -Version "22.1.5" -Entries @("cup")
    $reserved = Invoke-Cup -CommandArgs @("install", "linker", "lld@stable") -ExpectFailure
    Assert-Contains $reserved "conflicts with cup itself"

    New-TestPackage -Component "formatter" -Tool "clang-format" -Version "22.1.5" -Entries @("CLANG")
    $collision = Invoke-Cup -CommandArgs @("install", "formatter", "clang-format@stable") -ExpectFailure
    Assert-Contains $collision "declared by more than one default package"

    Assert-CupHealthy
    Write-Host "Windows wrapper tests passed."
} finally {
    Remove-TestEnvironment
}
