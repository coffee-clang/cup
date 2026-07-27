# Purpose: Exercises Windows install-selection defaults and scoped user preferences.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name "install-policy" -ExecutablePath $CupExecutablePath

    $initial = Invoke-Cup -CommandArgs @("config")
    Assert-Contains $initial "Install selections for host 'windows-x64', target 'windows-x64'"
    Assert-Contains $initial "compiler           clang"
    Assert-Contains $initial "official default"
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup")

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" `
        -Entries @("clang", "clang++")
    New-TestPackage -Component "linker" -Tool "lld" -Version "22.1.5" `
        -Entries @("lld")
    New-TestPackage -Component "compiler" -Tool "gcc" -Version "16.1.0-rev1" `
        -Entries @("gcc", "g++")
    New-TestPackage -Component "debugger" -Tool "gdb" -Version "17.1" `
        -Entries @("gdb")

    $profile = Invoke-Cup -CommandArgs @("install", "PROFILE", "MINIMAL")
    Assert-Contains $profile "Installing profile 'minimal' (2 packages)"
    Assert-Contains $profile `
        "Install group 'minimal' completed: 2 package(s) installed, 0 skipped."
    Assert-PathExists (Join-Path $Script:CupTestHome `
        ".cup\components\compiler\clang\windows-x64\windows-x64\22.1.5\info.txt")
    Assert-PathExists (Join-Path $Script:CupTestHome `
        ".cup\components\linker\lld\windows-x64\windows-x64\22.1.5\info.txt")

    $gnu = Invoke-Cup -CommandArgs @("install", "TOOLCHAIN", "GNU") -ExpectFailure
    Assert-Contains $gnu "Install group 'gnu' cannot be installed"
    Assert-Contains $gnu "ld                 not currently available"
    Assert-Contains $gnu "No packages were installed."
    Assert-PathMissing (Join-Path $Script:CupTestHome `
        ".cup\components\compiler\gcc\windows-x64\windows-x64\16.1.0-rev1")
    Assert-PathMissing (Join-Path $Script:CupTestHome `
        ".cup\components\debugger\gdb\windows-x64\windows-x64\17.1")

    Invoke-Cup -CommandArgs @(
        "config", "set", "compiler", "clang", "--target", "windows-x64") | Out-Null
    $configured = Invoke-Cup -CommandArgs @("config", "--target", "windows-x64")
    Assert-Contains $configured "compiler           clang"
    Assert-Contains $configured "user preference"

    Invoke-Cup -CommandArgs @(
        "config", "reset", "compiler", "--target", "windows-x64") | Out-Null
    $reset = Invoke-Cup -CommandArgs @("config", "--target", "windows-x64")
    Assert-Contains $reset "compiler           clang"
    Assert-Contains $reset "official default"

    Assert-CupStatus -CommandArgs @(
        "config", "set", "compiler", "unknown", "--target", "windows-x64") `
        -ExpectedStatus 2 | Out-Null
    Assert-CupHealthy
    Write-Host "Windows install-policy tests passed."
} finally {
    Remove-TestEnvironment
}
