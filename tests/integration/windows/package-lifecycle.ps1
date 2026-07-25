# Purpose: Exercises the native Windows install, update, default, inspect, and remove lifecycle.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "package-lifecycle" -ExecutablePath $CupExecutablePath
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version "21.1.5"
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    New-TestPackage -Component "compiler" -Tool "clang" -Version "21.1.5" `
        -Entries @("clang", "clang++")
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" `
        -Entries @("clang", "clang++")
    New-TestPackage -Component "debugger" -Tool "gdb" -Version "17.1" -Entries @("gdb")

    $installed = Invoke-Cup -CommandArgs @("install", "compiler", "clang@21.1.5")
    Assert-Contains $installed "set it as the first default"
    Invoke-Cup -CommandArgs @("install", "debugger", "gdb@stable") | Out-Null

    $embeddedVersion = Invoke-Cup -CommandArgs @("--version")
    if ($embeddedVersion -like "*-dev*") {
        $cupUpdateFailure = Invoke-Cup -CommandArgs @("update", "cup") -ExpectFailure
        Assert-Contains $cupUpdateFailure "available only from an official cup release"
    }

    $current = Invoke-Cup -CommandArgs @("info")
    Assert-Contains $current "compiler [windows-x64]: clang@21.1.5"
    Assert-Contains $current "debugger [windows-x64]: gdb@17.1 (stable)"
    Assert-Contains $current "status: active"

    $catalog = Invoke-Cup -CommandArgs @("search", "compiler")
    Assert-Contains $catalog "Available tools for component 'compiler'"
    Assert-Contains $catalog "clang"

    $listed = Invoke-Cup -CommandArgs @("list", "compiler")
    Assert-Contains $listed "compiler:clang@21.1.5"
    Assert-NotContains $listed "debugger:gdb@17.1"
    Assert-Equals (Invoke-ManagedCommand -Name "clang") `
        "clang-21.1.5-windows-x64:clang"

    $updated = Invoke-Cup -CommandArgs @("update", "clang")
    Assert-Contains $updated "1 stable package(s) installed, 1 default(s) moved"
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) `
        "compiler [windows-x64]: clang@22.1.5 (stable)"
    Assert-Equals (Invoke-ManagedCommand -Name "clang") `
        "clang-22.1.5-windows-x64:clang"

    $packageInfo = Invoke-Cup -CommandArgs @("inspect", "compiler", "clang@stable")
    Assert-Contains $packageInfo "Package information for compiler clang@stable -> clang@22.1.5"
    Assert-Contains $packageInfo "component          compiler"
    Assert-Contains $packageInfo "version            22.1.5"

    Invoke-Cup -CommandArgs @("default", "compiler", "clang@21.1.5") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) `
        "compiler [windows-x64]: clang@21.1.5"
    Invoke-Cup -CommandArgs @("default", "compiler", "clang@stable") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("update", "clang")) `
        "0 stable package(s) installed, 0 default(s) moved"

    Invoke-Cup -CommandArgs @("remove", "compiler", "clang@21.1.5") | Out-Null
    Assert-NotContains (Invoke-Cup -CommandArgs @("list", "compiler")) `
        "compiler:clang@21.1.5"
    Assert-CupHealthy
    Write-Host "Windows package lifecycle tests passed."
} finally {
    Remove-TestEnvironment
}
