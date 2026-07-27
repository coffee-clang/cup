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
    Assert-NotContains $installHelp $Script:CupTestExecutable
    Assert-Contains $installHelp "Select tar.xz, tar.gz or zip."
    $configHelp = Invoke-Cup -CommandArgs @("help", "config")
    Assert-Contains $configHelp "cup config set <component> <tool>"
    Assert-Contains $configHelp "reset without component clears that scope only"

    Invoke-Cup -CommandArgs @("repair") | Out-Null
    Set-Content -LiteralPath (Join-Path $cupRoot "state.txt") `
        -Value "not-a-state-record" -Encoding Ascii
    Assert-CupStatus -CommandArgs @("list") -ExpectedStatus 4 | Out-Null

    Write-Host "Windows CLI contract tests passed."
} finally {
    Remove-TestEnvironment
}
