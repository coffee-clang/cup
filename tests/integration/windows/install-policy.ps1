# Exercises Windows install-selection defaults and scoped user preferences.

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
    Assert-Contains $initial "analyzer           -                  -                  unavailable"
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
    New-TestPackage -Component "debugger" -Tool "lldb" -Version "22.1.5" `
        -Entries @("lldb")
    New-TestPackage -Component "formatter" -Tool "clang-format" -Version "22.1.5" `
        -Entries @("clang-format")
    New-TestPackage -Component "linter" -Tool "clang-tidy" -Version "22.1.5" `
        -Entries @("clang-tidy")
    New-TestPackage -Component "language-server" -Tool "clangd" -Version "22.1.5" `
        -Entries @("clangd")

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

    $configuredOutput = Invoke-Cup -CommandArgs @("config", "set", "compiler", "gcc")
    Assert-Contains $configuredOutput `
        "Preferred tool for 'compiler' on target 'windows-x64' set to 'gcc'."
    $configured = Invoke-Cup -CommandArgs @("config")
    Assert-Contains $configured "compiler           gcc"
    Assert-Contains $configured "user preference"
    $compilerInstall = Invoke-Cup -CommandArgs @("install", "compiler")
    Assert-Contains $compilerInstall "Installed compiler gcc@16.1.0-rev1"
    Assert-PathExists (Join-Path $Script:CupTestHome `
        ".cup\components\compiler\gcc\windows-x64\windows-x64\16.1.0-rev1\info.txt")

    Invoke-Cup -CommandArgs @(
        "config", "set", "compiler", "gcc", "--target", "linux-x64") | Out-Null
    $preferences = Join-Path $Script:CupTestHome ".cup\config\preferences.txt"
    $preferenceText = Get-Content -LiteralPath $preferences -Raw
    Assert-Contains $preferenceText "preferred.windows-x64.windows-x64.compiler=gcc"
    Assert-Contains $preferenceText "preferred.windows-x64.linux-x64.compiler=gcc"

    $resetCompiler = Invoke-Cup -CommandArgs @("config", "reset", "compiler")
    Assert-Contains $resetCompiler `
        "Preference for 'compiler' on target 'windows-x64' was reset."
    $preferenceText = Get-Content -LiteralPath $preferences -Raw
    Assert-NotContains $preferenceText "preferred.windows-x64.windows-x64.compiler="
    Assert-Contains $preferenceText "preferred.windows-x64.linux-x64.compiler=gcc"

    $resetTarget = Invoke-Cup -CommandArgs @("config", "reset", "--target", "linux-x64")
    Assert-Contains $resetTarget "Reset 1 preference(s) for target 'linux-x64'."
    Assert-PathMissing $preferences
    $reset = Invoke-Cup -CommandArgs @("config")
    Assert-Contains $reset "compiler           clang"
    Assert-Contains $reset "official default"

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $preferences) | Out-Null
    Write-Utf8NoBom -Path $preferences -Lines @("format=broken", "preset=gnu")
    $llvm = Invoke-Cup -CommandArgs @("install", "TOOLCHAIN", "LLVM")
    Assert-Contains $llvm "Installing toolchain 'llvm' (6 packages)"
    Assert-Contains $llvm `
        "Install group 'llvm' completed: 4 package(s) installed, 2 skipped."
    foreach ($relative in @(
        ".cup\components\debugger\lldb\windows-x64\windows-x64\22.1.5\info.txt",
        ".cup\components\formatter\clang-format\windows-x64\windows-x64\22.1.5\info.txt",
        ".cup\components\linter\clang-tidy\windows-x64\windows-x64\22.1.5\info.txt",
        ".cup\components\language-server\clangd\windows-x64\windows-x64\22.1.5\info.txt"
    )) {
        Assert-PathExists (Join-Path $Script:CupTestHome $relative)
    }
    Remove-Item -LiteralPath $preferences -Force

    Assert-CupStatus -CommandArgs @(
        "config", "set", "compiler", "unknown", "--target", "windows-x64") `
        -ExpectedStatus 2 | Out-Null
    Assert-CupHealthy
    Write-Host "Windows install-policy tests passed."
} finally {
    Remove-TestEnvironment
}
