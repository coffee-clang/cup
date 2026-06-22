param([Parameter(Mandatory = $true)][string]$CupPath)
. (Join-Path $PSScriptRoot "common.ps1")

Initialize-TestEnvironment -Name "entrypoints" -CupPath $CupPath
try {
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" -Entries @("clang", "clang++")
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@stable") | Out-Null

    Assert-Equals (Invoke-ManagedCommand -Name "clang") "clang-22.1.5-windows-x64:clang"
    $wrapper = Join-Path $Script:TestHome ".cup\bin\clang.cmd"
    Set-Content -LiteralPath $wrapper -Value "@echo altered" -Encoding ascii
    Assert-Contains (Invoke-Cup -CommandArgs @("current") -ExpectFailure) "status: invalid"
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor") -ExpectFailure) "entry point"
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-Equals (Invoke-ManagedCommand -Name "clang") "clang-22.1.5-windows-x64:clang"

    New-TestPackage -Component "linker" -Tool "lld" -Version "22.1.5" -Entries @("cup")
    $reserved = Invoke-Cup -CommandArgs @("install", "linker", "lld@stable") -ExpectFailure
    Assert-Contains $reserved "conflicts with cup itself"

    New-TestPackage -Component "formatter" -Tool "clang-format" -Version "22.1.5" -Entries @("CLANG")
    $collision = Invoke-Cup -CommandArgs @("install", "formatter", "clang-format@stable") -ExpectFailure
    Assert-Contains $collision "declared by more than one default package"

    Invoke-Cup -CommandArgs @("doctor") | Out-Null
    Write-Host "Windows entry point tests passed."
} finally {
    Remove-TestEnvironment
}
