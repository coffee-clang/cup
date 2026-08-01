# Purpose: Exercises public Windows CLI dispatch, help aliases, and stable exit statuses.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name "cli-contract" -ExecutablePath $CupExecutablePath
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value "21.1.5" `
        -Mode "Prepend"

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Assert-PathMissing $cupRoot

    foreach ($arguments in @(
        @("--version"),
        @("help"),
        @("search", "compiler"),
        @("config"),
        @("list"),
        @("info"),
        @("doctor")
    )) {
        Invoke-Cup -CommandArgs $arguments | Out-Null
        Assert-PathMissing $cupRoot
    }
    Assert-CupStatus -CommandArgs @("inspect", "compiler", "clang@21.1.5") `
        -ExpectedStatus 3 | Out-Null
    Assert-PathMissing $cupRoot

    Assert-CupStatus -CommandArgs @("Unknown") -ExpectedStatus 2 `
        -ExpectedText "unknown command 'Unknown'" | Out-Null
    Assert-CupStatus -CommandArgs @("Help") -ExpectedStatus 2 `
        -ExpectedText "unknown command 'Help'" | Out-Null
    Assert-CupStatus -CommandArgs @("default") -ExpectedStatus 2 `
        -ExpectedText "missing option <component>" | Out-Null
    Assert-PathMissing $cupRoot

    $topHelp = Invoke-Cup -CommandArgs @("--help")
    Assert-Contains $topHelp "Commands:"
    foreach ($command in @(
        "help", "search", "list", "install", "remove", "update", "config",
        "default", "info", "inspect", "doctor", "repair", "uninstall"
    )) {
        Assert-Contains $topHelp ("  {0}" -f $command)
        foreach ($arguments in @(
            @("help", $command),
            @($command, "-h"),
            @($command, "--help")
        )) {
            $help = Invoke-Cup -CommandArgs $arguments
            foreach ($section in @(
                "Usage:", "Description:", "Arguments:", "Options:",
                "Defaults:", "Examples:", "Effects:"
            )) {
                Assert-Contains $help $section
            }
        }
    }

    $installHelp = Invoke-Cup -CommandArgs @("install", "--help")
    Assert-Contains $installHelp "cup install <profile|toolchain> <name>"
    Assert-Contains $installHelp "cup install [<component>] <tool>[@<release>]"
    Assert-NotContains $installHelp $Script:CupTestExecutable
    Assert-Contains $installHelp "Select tar.xz, tar.gz or zip."
    $removeHelp = Invoke-Cup -CommandArgs @("remove", "--help")
    Assert-Contains $removeHelp "cup remove [<component>] <tool>[@<release>]"
    Assert-NotContains $removeHelp "profile"
    Assert-NotContains $removeHelp "toolchain"
    $configHelp = Invoke-Cup -CommandArgs @("help", "config")
    Assert-Contains $configHelp "cup config set <component> <tool>"
    Assert-Contains $configHelp "reset without component clears that scope only"

    $originalProfile = $env:USERPROFILE
    try {
        $foreignHome = Join-Path $Script:CupTestRoot "foreign-root-home"
        $foreignPrimary = Join-Path $foreignHome ".cup"
        New-Item -ItemType Directory -Force -Path $foreignPrimary | Out-Null
        Set-Content -LiteralPath (Join-Path $foreignPrimary "foreign.txt") `
            -Value "unrelated" -Encoding Ascii
        $env:USERPROFILE = $foreignHome
        Invoke-Cup -CommandArgs @("repair") | Out-Null
        Assert-PathExists (Join-Path $foreignPrimary "foreign.txt")
        $foreignMarker = Join-Path $foreignHome ".coffee-cup\root.txt"
        Assert-PathExists $foreignMarker
        Assert-PathExists (Join-Path $foreignHome ".coffee-cup\state.txt")
        $markerLines = @(Get-Content -LiteralPath $foreignMarker)
        Assert-Equals $markerLines.Count 3
        Assert-Equals $markerLines[0] "format=1"
        Assert-Equals $markerLines[1] "product=coffee-clang/cup"
        Assert-Equals $markerLines[2] "layout=1"

        $legacyHome = Join-Path $Script:CupTestRoot "legacy-root-home"
        foreach ($directory in @("components", "staging", "cache")) {
            New-Item -ItemType Directory -Force `
                -Path (Join-Path $legacyHome ".cup\$directory") | Out-Null
        }
        $legacyState = Join-Path $legacyHome ".cup\state.txt"
        Set-Content -LiteralPath $legacyState -Value "format=1" -Encoding Ascii
        $legacyStateHash = (Get-FileHash -LiteralPath $legacyState -Algorithm SHA256).Hash
        $env:USERPROFILE = $legacyHome
        Invoke-Cup -CommandArgs @("repair") | Out-Null
        Assert-Equals (Get-FileHash -LiteralPath $legacyState -Algorithm SHA256).Hash `
            $legacyStateHash
        Assert-PathMissing (Join-Path $legacyHome ".cup\root.txt")
        Assert-PathExists (Join-Path $legacyHome ".coffee-cup\root.txt")

        $verifiedHome = Join-Path $Script:CupTestRoot "verified-legacy-root-home"
        $verifiedRoot = Join-Path $verifiedHome ".cup"
        foreach ($directory in @("bin", "components", "staging", "cache", "config", "helpers")) {
            New-Item -ItemType Directory -Force -Path (Join-Path $verifiedRoot $directory) |
                Out-Null
        }
        $verifiedBinary = Join-Path $verifiedRoot "bin\cup.exe"
        $verifiedHelper = Join-Path $verifiedRoot "helpers\cup-update-helper.exe"
        $verifiedUninstall = Join-Path $verifiedRoot "helpers\uninstall.ps1"
        $verifiedCatalog = Join-Path $verifiedRoot "config\packages.cfg"
        $verifiedPolicy = Join-Path $verifiedRoot "config\install.cfg"
        Copy-Item -LiteralPath $Script:CupTestExecutable -Destination $verifiedBinary
        Copy-Item -LiteralPath $Script:CupTestExecutable -Destination $verifiedHelper
        Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot `
            "scripts\install\uninstall-cup-windows.ps1") -Destination $verifiedUninstall
        Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "config\packages.cfg") `
            -Destination $verifiedCatalog
        Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "config\install.cfg") `
            -Destination $verifiedPolicy
        Set-Content -LiteralPath (Join-Path $verifiedRoot "state.txt") `
            -Value "format=1" -Encoding Ascii
        $binaryHash = (Get-FileHash -LiteralPath $verifiedBinary -Algorithm SHA256).Hash.ToLowerInvariant()
        $uninstallHash = (Get-FileHash -LiteralPath $verifiedUninstall -Algorithm SHA256).Hash.ToLowerInvariant()
        $catalogHash = (Get-FileHash -LiteralPath $verifiedCatalog -Algorithm SHA256).Hash.ToLowerInvariant()
        $policyHash = (Get-FileHash -LiteralPath $verifiedPolicy -Algorithm SHA256).Hash.ToLowerInvariant()
        $zeros = "0" * 64
        Set-Content -LiteralPath (Join-Path $verifiedRoot "config\SHA256SUMS.windows-x64") `
            -Encoding Ascii -Value @(
                "$binaryHash  cup-windows-x64.exe",
                "$uninstallHash  uninstall.ps1",
                "$zeros  release.txt"
            )
        Set-Content -LiteralPath (Join-Path $verifiedRoot "config\SHA256SUMS.common") `
            -Encoding Ascii -Value @(
                "$catalogHash  packages.cfg",
                "$policyHash  install.cfg",
                "$zeros  install.sh",
                "$zeros  install.ps1"
            )
        $env:USERPROFILE = $verifiedHome
        Invoke-Cup -CommandArgs @("repair") | Out-Null
        Assert-PathExists (Join-Path $verifiedRoot "root.txt")
        Assert-PathMissing (Join-Path $verifiedHome ".coffee-cup")

        $lookalikeHome = Join-Path $Script:CupTestRoot "lookalike-root-home"
        foreach ($directory in @("components", "staging", "cache")) {
            New-Item -ItemType Directory -Force `
                -Path (Join-Path $lookalikeHome ".cup\$directory") | Out-Null
        }
        Set-Content -LiteralPath (Join-Path $lookalikeHome ".cup\state.txt") `
            -Value "not-a-cup-state" -Encoding Ascii
        $env:USERPROFILE = $lookalikeHome
        Invoke-Cup -CommandArgs @("repair") | Out-Null
        Assert-PathExists (Join-Path $lookalikeHome ".cup\state.txt")
        Assert-PathExists (Join-Path $lookalikeHome ".coffee-cup\root.txt")
        Assert-PathExists (Join-Path $lookalikeHome ".coffee-cup\state.txt")

        $corruptHome = Join-Path $Script:CupTestRoot "corrupt-root-home"
        $env:USERPROFILE = $corruptHome
        Invoke-Cup -CommandArgs @("repair") | Out-Null
        $corruptRoot = Join-Path $corruptHome ".cup"
        $corruptState = Join-Path $corruptRoot "state.txt"
        $corruptMarker = Join-Path $corruptRoot "root.txt"
        $stateHash = (Get-FileHash -LiteralPath $corruptState -Algorithm SHA256).Hash
        Set-Content -LiteralPath $corruptMarker -Value "corrupt" -Encoding Ascii
        $markerHash = (Get-FileHash -LiteralPath $corruptMarker -Algorithm SHA256).Hash
        $corruptDoctor = Assert-CupStatus -CommandArgs @("doctor") -ExpectedStatus 4
        Assert-Contains $corruptDoctor "cup root marker is invalid for recognized root"
        Assert-Contains $corruptDoctor "neither cup root candidate was selected or modified"
        Assert-Equals (Get-FileHash -LiteralPath $corruptState -Algorithm SHA256).Hash $stateHash
        Assert-Equals (Get-FileHash -LiteralPath $corruptMarker -Algorithm SHA256).Hash $markerHash
        Assert-PathMissing (Join-Path $corruptHome ".coffee-cup")
        Assert-CupStatus -CommandArgs @("repair") -ExpectedStatus 4 | Out-Null
        Assert-PathMissing (Join-Path $corruptHome ".coffee-cup")
    } finally {
        $env:USERPROFILE = $originalProfile
    }

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Set-Content -LiteralPath (Join-Path $cupRoot "state.txt") `
        -Value "not-a-state-record" -Encoding Ascii
    Assert-CupStatus -CommandArgs @("list") -ExpectedStatus 4 | Out-Null

    Write-Host "Windows CLI contract tests passed."
} finally {
    Remove-TestEnvironment
}
