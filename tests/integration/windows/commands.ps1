# Purpose: Exercises public package and state commands with the native Windows executable.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "commands" -ExecutablePath $CupExecutablePath
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version "21.1.5"

    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")
    Assert-Contains (Invoke-Cup -CommandArgs @("config")) "official default"
    Assert-Contains (Invoke-Cup -CommandArgs @("list")) "No packages installed"
    Assert-Contains (Invoke-Cup -CommandArgs @("doctor")) "runtime is not initialized"
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")
    Assert-CupStatus -CommandArgs @("Unknown") -ExpectedStatus 2 -ExpectedText "unknown command 'Unknown'" | Out-Null
    Assert-CupStatus -CommandArgs @("inspect", "compiler", "clang@21.1.5") -ExpectedStatus 3 | Out-Null
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")

    Assert-Contains (Invoke-Cup -CommandArgs @("--help")) "Commands:"
    Assert-Contains (Invoke-Cup -CommandArgs @("install", "--help")) "Effects:"
    Assert-Contains (Invoke-Cup -CommandArgs @("help", "config")) "reset without component clears that scope only"

    Invoke-Cup -CommandArgs @("repair") | Out-Null

    New-TestPackage -Component "compiler" -Tool "clang" -Version "21.1.5" -Entries @("clang", "clang++")
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" -Entries @("clang", "clang++")
    New-TestPackage -Component "debugger" -Tool "gdb" -Version "17.1" -Entries @("gdb")

    $output = Invoke-Cup -CommandArgs @("install", "compiler", "clang@21.1.5")
    Assert-Contains $output "set it as the first default"
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

    $config = Invoke-Cup -CommandArgs @("config")
    Assert-Contains $config "Install selections for host 'windows-x64', target 'windows-x64'"
    Assert-Contains $config "compiler           clang"
    Assert-Contains $config "official default"
    Invoke-Cup -CommandArgs @("config", "set", "compiler", "clang", "--target", "windows-x64") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("config", "--target", "windows-x64")) "user preference"
    Invoke-Cup -CommandArgs @("config", "reset", "compiler", "--target", "windows-x64") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("config", "--target", "windows-x64")) "official default"

    $catalog = Invoke-Cup -CommandArgs @("search", "compiler")
    Assert-Contains $catalog "Available tools for component 'compiler'"
    Assert-Contains $catalog "clang"

    $installed = Invoke-Cup -CommandArgs @("list", "compiler")
    Assert-Contains $installed "compiler:clang@21.1.5"
    Assert-NotContains $installed "debugger:gdb@17.1"

    Assert-Equals (Invoke-ManagedCommand -Name "clang") "clang-21.1.5-windows-x64:clang"

    $output = Invoke-Cup -CommandArgs @("update", "clang")
    Assert-Contains $output "1 stable package(s) installed, 1 default(s) moved"
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) "compiler [windows-x64]: clang@22.1.5 (stable)"
    Assert-Equals (Invoke-ManagedCommand -Name "clang") "clang-22.1.5-windows-x64:clang"

    $packageInfo = Invoke-Cup -CommandArgs @("inspect", "compiler", "clang@stable")
    Assert-Contains $packageInfo "Package information for compiler clang@stable -> clang@22.1.5"
    Assert-Contains $packageInfo "component          compiler"
    Assert-Contains $packageInfo "version            22.1.5"

    Invoke-Cup -CommandArgs @("default", "compiler", "clang@21.1.5") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) "compiler [windows-x64]: clang@21.1.5"
    Invoke-Cup -CommandArgs @("default", "compiler", "clang@stable") | Out-Null

    $output = Invoke-Cup -CommandArgs @("update", "clang")
    Assert-Contains $output "0 stable package(s) installed, 0 default(s) moved"

    $failure = Invoke-Cup -CommandArgs @("default") -ExpectFailure
    Assert-Contains $failure "missing option <component>"

    Invoke-Cup -CommandArgs @("remove", "compiler", "clang@21.1.5") | Out-Null
    Assert-NotContains (Invoke-Cup -CommandArgs @("list", "compiler")) "compiler:clang@21.1.5"

    Assert-CupHealthy
    Write-Host "Windows command tests passed."
} finally {
    Remove-TestEnvironment
}
