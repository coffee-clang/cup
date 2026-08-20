# Exercises public Windows CLI dispatch, help aliases, root selection,
# parser precedence, and stable exit statuses.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function Test-ReadOnlyNoInitialization {
    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Assert-PathMissing $cupRoot

    foreach ($arguments in @(
        ,@("--version"),
        ,@("help"),
        ,@("search", "compiler"),
        ,@("config"),
        ,@("list"),
        ,@("info"),
        ,@("doctor")
    )) {
        Invoke-Cup -CommandArgs $arguments | Out-Null
        Assert-PathMissing $cupRoot
    }

    Assert-CupStatus -CommandArgs @("inspect", "compiler", "clang@21.1.5") `
        -ExpectedStatus 3 | Out-Null
    Assert-PathMissing $cupRoot
}

function Test-DispatchAndInvalidSyntax {
    $cupRoot = Join-Path $Script:CupTestHome ".cup"

    Assert-CupStatus -CommandArgs @() -ExpectedStatus 2 `
        -ExpectedText "Usage:" | Out-Null
    Assert-CupStatus -CommandArgs @("Unknown") -ExpectedStatus 2 `
        -ExpectedText "unknown command 'Unknown'" | Out-Null
    Assert-CupStatus -CommandArgs @("Help") -ExpectedStatus 2 `
        -ExpectedText "unknown command 'Help'" | Out-Null

    $cases = @(
        @{ Name = "help-extra"; Arguments = @("help", "search", "extra") },
        @{ Name = "search-extra"; Arguments = @("search", "compiler", "extra") },
        @{ Name = "list-extra"; Arguments = @("list", "compiler", "extra") },
        @{ Name = "install-missing"; Arguments = @("install") },
        @{ Name = "install-extra"; Arguments = @("install", "compiler", "clang", "extra") },
        @{ Name = "remove-missing"; Arguments = @("remove") },
        @{ Name = "remove-extra"; Arguments = @("remove", "compiler", "clang", "extra") },
        @{ Name = "update-extra"; Arguments = @("update", "compiler", "extra") },
        @{ Name = "config-extra"; Arguments = @("config", "set", "compiler", "clang", "extra") },
        @{ Name = "default-missing"; Arguments = @("default", "compiler") },
        @{ Name = "default-extra"; Arguments = @("default", "compiler", "clang", "extra") },
        @{ Name = "info-extra"; Arguments = @("info", "compiler", "extra") },
        @{ Name = "inspect-missing"; Arguments = @("inspect", "compiler") },
        @{ Name = "inspect-extra"; Arguments = @("inspect", "compiler", "clang@1", "extra") },
        @{ Name = "doctor-extra"; Arguments = @("doctor", "extra") },
        @{ Name = "repair-extra"; Arguments = @("repair", "extra") },
        @{ Name = "uninstall-extra"; Arguments = @("uninstall", "extra") }
    )
    foreach ($case in $cases) {
        $output = Assert-CupStatus -CommandArgs $case.Arguments -ExpectedStatus 2
        Assert-Contains $output "Usage:"
    }

    Assert-CupStatus -CommandArgs @("help", "unknown-command") -ExpectedStatus 2 `
        -ExpectedText "unknown command 'unknown-command'" | Out-Null
    Assert-CupStatus -CommandArgs @("inspect", "compiler", "@stable") -ExpectedStatus 2 | Out-Null
    Assert-CupStatus -CommandArgs @("default", "compiler", "@stable") -ExpectedStatus 2 | Out-Null
    Assert-CupStatus -CommandArgs @("remove", "@stable") -ExpectedStatus 2 | Out-Null
    Assert-CupStatus -CommandArgs @("remove", "compiler", "@stable") -ExpectedStatus 2 | Out-Null
    Assert-CupStatus -CommandArgs @("inspect", "COMPILER", "CLANG@STABLE") `
        -ExpectedStatus 3 | Out-Null

    Assert-PathMissing $cupRoot
}

function Test-HelpAliases {
    $topHelp = Invoke-Cup -CommandArgs @("--help")
    Assert-Contains $topHelp "Commands:"

    foreach ($command in @(
        "help", "search", "list", "install", "remove", "update", "config",
        "default", "info", "inspect", "doctor", "repair", "uninstall"
    )) {
        Assert-Contains $topHelp ("  {0}" -f $command)
        foreach ($arguments in @(
            ,@("help", $command),
            ,@($command, "-h"),
            ,@($command, "--help")
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

    $updateHelp = Invoke-Cup -CommandArgs @("help", "update")
    Assert-Contains $updateHelp `
        "Without a selector, updates installed tools only; cup itself is not updated."

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
    Assert-Contains $configHelp "reset without component clears preferences for that target"

    $uninstallHelp = Invoke-Cup -CommandArgs @("help", "uninstall")
    Assert-Contains $uninstallHelp "--yes  Skip the confirmation prompt."
}

function Test-ForeignAndLegacyRootSelection {
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

        Test-UnmarkedCupRootPreservation
        Test-LookalikeRootSelection
        Test-CorruptRecognizedRootPreservation
    } finally {
        $env:USERPROFILE = $originalProfile
    }
}

function Test-UnmarkedCupRootPreservation {
    $unmarkedHome = Join-Path $Script:CupTestRoot "unmarked-cup-root-home"
    $unmarkedBinary = Join-Path $unmarkedHome ".cup\bin\cup.exe"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $unmarkedBinary) | Out-Null
    Set-Content -LiteralPath $unmarkedBinary -Value "unmarked-cup-generation" -Encoding Ascii
    $binaryHash = (Get-FileHash -LiteralPath $unmarkedBinary -Algorithm SHA256).Hash

    $env:USERPROFILE = $unmarkedHome
    $failure = Assert-CupStatus -CommandArgs @("repair") -ExpectedStatus 4
    Assert-Contains $failure "unmarked cup-like root"
    Assert-Contains $failure "Move the preserved directory to a backup path"
    Assert-Contains $failure "do not add root.txt manually"
    Assert-Equals (Get-FileHash -LiteralPath $unmarkedBinary -Algorithm SHA256).Hash $binaryHash
    Assert-PathMissing (Join-Path $unmarkedHome ".cup\root.txt")
    Assert-PathMissing (Join-Path $unmarkedHome ".coffee-cup")
}

function Test-LookalikeRootSelection {
    $lookalikeHome = Join-Path $Script:CupTestRoot "lookalike-root-home"
    foreach ($directory in @("components", "staging", "cache")) {
        New-Item -ItemType Directory -Force `
            -Path (Join-Path $lookalikeHome ".cup\$directory") | Out-Null
    }
    $lookalikeState = Join-Path $lookalikeHome ".cup\state.txt"
    Set-Content -LiteralPath $lookalikeState -Value "not-a-cup-state" -Encoding Ascii
    $stateHash = (Get-FileHash -LiteralPath $lookalikeState -Algorithm SHA256).Hash

    $env:USERPROFILE = $lookalikeHome
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Assert-Equals (Get-FileHash -LiteralPath $lookalikeState -Algorithm SHA256).Hash $stateHash
    Assert-PathMissing (Join-Path $lookalikeHome ".cup\root.txt")
    Assert-PathExists (Join-Path $lookalikeHome ".coffee-cup\root.txt")
    Assert-PathExists (Join-Path $lookalikeHome ".coffee-cup\state.txt")
}

function Test-CorruptRecognizedRootPreservation {
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
}

function Test-SyntaxPrecedesRuntimePreflight {
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    $transactionPath = Join-Path $Script:CupTestHome ".cup\transaction.txt"
    Set-Content -LiteralPath $transactionPath -Value "invalid journal" -Encoding Ascii

    $cases = @(
        ,@("install"),
        ,@("install", "compiler", "@stable"),
        ,@("install", "profile"),
        ,@("list", "--target", "windows-arm64"),
        ,@("config", "change", "compiler", "clang"),
        ,@("config", "set", "compiler", "clang@stable"),
        ,@("inspect", "compiler", "clang@RC1"),
        ,@("default", "compiler", "clang@../x"),
        ,@("search", ("x" * 512))
    )
    foreach ($arguments in $cases) {
        $syntax = Assert-CupStatus -CommandArgs $arguments -ExpectedStatus 2 `
            -ExpectedText "Usage:"
        Assert-NotContains $syntax "transaction journal is invalid"
    }

    Remove-Item -LiteralPath $transactionPath -Force
}

function Test-ConfigActionNormalization {
    Assert-CupStatus -CommandArgs @("config", "SET", "COMPILER", "CLANG") `
        -ExpectedStatus 0 -ExpectedText "set to 'clang'" | Out-Null
    Assert-CupStatus -CommandArgs @("config", "RESET", "COMPILER") `
        -ExpectedStatus 0 -ExpectedText "was reset" | Out-Null
}

function Test-StateStatus {
    $statePath = Join-Path $Script:CupTestHome ".cup\state.txt"
    Set-Content -LiteralPath $statePath -Value "not-a-state-record" -Encoding Ascii
    Assert-CupStatus -CommandArgs @("list") -ExpectedStatus 4 | Out-Null
}

function Test-RootHomeRejection {
    $root = [System.IO.Path]::GetPathRoot($Script:CupTestRoot)
    $originalProfile = $env:USERPROFILE
    try {
        $env:USERPROFILE = $root
        $result = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
            -Arguments @("doctor") -WorkingDirectory $Script:CupTestDevRoot
        if ($result.ExitCode -eq 0) {
            Fail-Test "filesystem-root USERPROFILE was accepted"
        }
        Assert-Contains $result.Output "USERPROFILE must be an absolute user directory, not a volume root"
    } finally {
        $env:USERPROFILE = $originalProfile
    }
}

try {
    Initialize-TestEnvironment -Name "cli-contract" -ExecutablePath $CupExecutablePath
    Set-PackageCatalogField -Component "compiler" -Tool "clang" `
        -Field "available_versions" -Value "21.1.5" -Mode "Prepend"

    Test-ReadOnlyNoInitialization
    Test-DispatchAndInvalidSyntax
    Test-HelpAliases
    Test-ForeignAndLegacyRootSelection
    Test-RootHomeRejection
    Test-SyntaxPrecedesRuntimePreflight
    Test-ConfigActionNormalization
    Test-StateStatus

    Write-Host "Windows CLI contract tests passed."
} finally {
    Remove-TestEnvironment
}
