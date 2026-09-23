# Exercises read-only Windows diagnosis and proves doctor never repairs observed damage.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name 'doctor' -ExecutablePath $CupExecutablePath

    $initial = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $initial 'cup runtime is not installed'
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup')

    Ensure-FixtureRuntimeRoot
    $cupRoot = Join-Path $Script:CupTestHome '.cup'
    $pathWarning = Invoke-Cup -CommandArgs @('doctor')
    Assert-Contains $pathWarning 'current CUP command directory is not in PATH'
    $binPath = Join-Path $cupRoot 'bin'
    Assert-ContainsPathText $pathWarning (
        "PowerShell:     `$env:Path = '$binPath' + ';' + `$env:Path")
    Assert-ContainsPathText $pathWarning ('Command Prompt: set "PATH=' + $binPath + ';%PATH%"')

    $savedTestHome = $Script:CupTestHome
    $savedUserProfile = $env:USERPROFILE
    try {
        $Script:CupTestHome = Join-Path $Script:CupTestRoot 'home;separator'
        $env:USERPROFILE = $Script:CupTestHome
        New-Item -ItemType Directory -Force -Path $Script:CupTestHome | Out-Null
        Ensure-FixtureRuntimeRoot
        $separatorWarning = Invoke-Cup -CommandArgs @('doctor')
        Assert-Contains $separatorWarning 'current CUP command directory is not in PATH'
        Assert-Contains $separatorWarning "contains ';' and cannot be represented as one PATH entry"
        Assert-NotContains $separatorWarning 'PowerShell:'
        Assert-NotContains $separatorWarning 'Command Prompt:'

        $Script:CupTestHome = Join-Path $Script:CupTestRoot "home'quote"
        $env:USERPROFILE = $Script:CupTestHome
        New-Item -ItemType Directory -Force -Path $Script:CupTestHome | Out-Null
        Ensure-FixtureRuntimeRoot
        $quoteWarning = Invoke-Cup -CommandArgs @('doctor')
        $quoteBin = Join-Path $Script:CupTestHome '.cup\bin'
        $escapedQuoteBin = $quoteBin.Replace("'", "''")
        Assert-ContainsPathText $quoteWarning (
            "PowerShell:     `$env:Path = '$escapedQuoteBin' + ';' + `$env:Path")

        $Script:CupTestHome = Join-Path $Script:CupTestRoot 'home%literal%'
        $env:USERPROFILE = $Script:CupTestHome
        New-Item -ItemType Directory -Force -Path $Script:CupTestHome | Out-Null
        Ensure-FixtureRuntimeRoot
        $percentWarning = Invoke-Cup -CommandArgs @('doctor')
        Assert-Contains $percentWarning 'PowerShell:'
        Assert-Contains $percentWarning 'Command Prompt: use the PowerShell command above'
        Assert-NotContains $percentWarning ('set "PATH=' + (Join-Path $Script:CupTestHome '.cup\bin'))
    } finally {
        $Script:CupTestHome = $savedTestHome
        $env:USERPROFILE = $savedUserProfile
    }

    $env:Path = "$binPath;$env:Path"
    $statePath = Join-Path $cupRoot 'state.txt'
    $transactionPath = Join-Path $cupRoot 'transaction.txt'

    $compilerRoot = New-InstalledPackageFixture -Component 'compiler' -Tool 'clang' `
        -Version '99.0.0' -Entries @('clang')
    [void](New-InstalledPackageFixture -Component 'debugger' -Tool 'lldb' `
        -Version '23.1.0' -Entries @('lldb'))
    $compilerInfo = Join-Path $compilerRoot 'info.txt'
    $compilerInfoLines = @(Get-Content -LiteralPath $compilerInfo)
    Write-Utf8NoBom -Path $compilerInfo -Lines @(
        $compilerInfoLines + 'fixture.note=changed-after-manifest')
    $compilerInfoHash = Get-Sha256Lower -Path $compilerInfo
    $invalidPackage = Join-Path $cupRoot 'components\linker\lld\windows-x64\22.1.5'
    New-Item -ItemType Directory -Force -Path $invalidPackage | Out-Null
    Write-Utf8NoBom -Path $statePath -Lines @(
        'format=2',
        'installed.compiler.windows-x64=clang@99.0.0',
        'installed.linter.windows-x64=clang-tidy@22.1.5'
    )
    $leftover = Join-Path $cupRoot 'staging\leftover'
    New-Item -ItemType Directory -Force -Path $leftover | Out-Null
    Write-Utf8NoBom -Path $transactionPath -Lines @('invalid journal')
    $stateHash = Get-Sha256Lower -Path $statePath

    $issues = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $issues 'transaction journal is invalid'
    Assert-Contains $issues "installed state record 'linter:clang-tidy@22.1.5' has no valid package"
    Assert-Contains $issues "installed package 'compiler:clang@99.0.0' is not listed"
    Assert-ContainsPathText $issues "package path '$compilerRoot' is invalid"
    Assert-Contains $issues (
        "valid package 'lldb@23.1.0' exists in components but is absent " +
        'from state.txt')
    Assert-ContainsPathText $issues "package path '$invalidPackage' is invalid"
    Assert-Contains $issues 'staging directory contains 1 leftover item(s)'
    Assert-Contains $issues "Run 'cup repair' after reviewing them."

    Assert-Equals (Get-Sha256Lower -Path $statePath) $stateHash
    Assert-PathExists $invalidPackage
    Assert-PathExists $leftover
    Assert-Equals (Get-Sha256Lower -Path $compilerInfo) $compilerInfoHash

    Remove-Item -LiteralPath $transactionPath -Force
    Remove-Item -LiteralPath $leftover, $invalidPackage -Recurse -Force
    [void](New-InstalledPackageFixture -Component 'compiler' -Tool 'clang' `
        -Version '99.0.0' -Entries @('clang'))
    Write-Utf8NoBom -Path $statePath -Lines @(
        'format=2',
        'installed.compiler.windows-x64=clang@99.0.0',
        'installed.debugger.windows-x64=lldb@23.1.0'
    )

    # Cache is lazy and may be absent. A missing lock, however, prevents a coherent snapshot.
    Remove-Item -LiteralPath (Join-Path $cupRoot 'cache') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $cupRoot 'cup.lock') -Force
    $missingLock = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $missingLock 'cup lock file is missing'

    Ensure-FixtureRuntimeRoot
    $warningOnly = Invoke-Cup -CommandArgs @('doctor')
    Assert-Contains $warningOnly 'Doctor found 2 warning(s), but no blocking issues.'
    Assert-Contains $warningOnly "installed package 'compiler:clang@99.0.0' is not listed"
    Assert-Contains $warningOnly "installed package 'debugger:lldb@23.1.0' is not listed"

    Write-Utf8NoBom -Path $transactionPath -Lines @(
        'format=2',
        'operation=cup-generation',
        ('target_release_sha256=' + ('0' * 64)),
        'temporary_name=cup-update-test'
    )
    $generationJournalHash = Get-Sha256Lower -Path $transactionPath
    $generationPending = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $generationPending `
        "interrupted CUP generation transaction detected in workspace 'cup-update-test'"
    Assert-Equals (Get-Sha256Lower -Path $transactionPath) $generationJournalHash

    # Help, version, typos, parse errors and read-only views never rewrite durable evidence.
    Invoke-Cup -CommandArgs @('help') | Out-Null
    Invoke-Cup -CommandArgs @('--version') | Out-Null
    Assert-Equals (Get-Sha256Lower -Path $transactionPath) $generationJournalHash
    Invoke-Cup -CommandArgs @('not-a-command') -ExpectFailure | Out-Null
    Assert-Equals (Get-Sha256Lower -Path $transactionPath) $generationJournalHash
    Invoke-Cup -CommandArgs @('install') -ExpectFailure | Out-Null
    Assert-Equals (Get-Sha256Lower -Path $transactionPath) $generationJournalHash
    foreach ($readOnlyCommand in @('search', 'list', 'config', 'info', 'inspect')) {
        Invoke-Cup -CommandArgs @($readOnlyCommand) -ExpectFailure | Out-Null
        Assert-Equals (Get-Sha256Lower -Path $transactionPath) $generationJournalHash
    }

    Write-Utf8NoBom -Path $transactionPath -Lines @(
        'format=2',
        'operation=cup-generation',
        'target_release_sha256=not-a-digest',
        'temporary_name=cup-update-test'
    )
    $invalidGeneration = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $invalidGeneration 'CUP generation transaction journal is invalid'
    Remove-Item -LiteralPath $transactionPath -Force

    Write-Utf8NoBom -Path $transactionPath -Lines @(
        'format=2',
        'operation=uninstall',
        'phase=failed',
        'temporary_name=.cup-uninstall-fixture',
        'token=fixture',
        'error=6'
    )
    $uninstallJournalHash = Get-Sha256Lower -Path $transactionPath
    $uninstallFailure = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $uninstallFailure 'the previous cup uninstall failed with error 6'
    Assert-Equals (Get-Sha256Lower -Path $transactionPath) $uninstallJournalHash
    $uninstallFailureAgain = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $uninstallFailureAgain 'the previous cup uninstall failed with error 6'
    Assert-Equals (Get-Sha256Lower -Path $transactionPath) $uninstallJournalHash
    Remove-Item -LiteralPath $transactionPath -Force

    Write-Host 'Windows doctor tests passed.'
} finally {
    Remove-TestEnvironment
}
