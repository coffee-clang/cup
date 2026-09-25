# Exercises deterministic Windows repair, state reconstruction, quarantine and evidence preservation.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name 'repair' -ExecutablePath $CupExecutablePath
    Ensure-FixtureRuntimeRoot

    $cupRoot = Join-Path $Script:CupTestHome '.cup'
    $stateFile = Join-Path $cupRoot 'state.txt'
    $transactionFile = Join-Path $cupRoot 'transaction.txt'

    # A valid package found on disk is adopted without rewriting producer-owned metadata.
    $packageRoot = New-InstalledPackageFixture -Component compiler -Tool clang `
        -Version '23.1.0' -Entries @('clang')
    $packageMetadata = Join-Path $packageRoot 'info.txt'
    $packageManifest = Join-Path $packageRoot 'manifest.txt'
    $metadataHash = Get-Sha256Lower -Path $packageMetadata
    $manifestHash = Get-Sha256Lower -Path $packageManifest
    $adopt = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $adopt "Prepared state repair: adopt valid package 'compiler:clang@23.1.0'"
    Assert-Contains (Get-Content -LiteralPath $stateFile -Raw) `
        'installed.compiler.windows-x64=clang@23.1.0'
    Assert-Equals (Get-Sha256Lower -Path $packageMetadata) $metadataHash
    Assert-Equals (Get-Sha256Lower -Path $packageManifest) $manifestHash

    # State entries with no package are removed together with their defaults.
    [string[]]$stateLines = Get-Content -LiteralPath $stateFile
    Write-Utf8NoBom -Path $stateFile -Lines ($stateLines + @(
        'installed.debugger.windows-x64=lldb@23.1.0',
        'default.debugger.windows-x64=lldb@23.1.0'
    ))
    $stale = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $stale "Removed stale state record 'debugger:lldb@23.1.0'."
    Assert-NotContains (Get-Content -LiteralPath $stateFile -Raw) 'lldb@23.1.0'

    # Package-shaped invalid data is quarantined; ambiguous higher-level structure is preserved.
    $invalidPackage = Join-Path $cupRoot 'components\debugger\lldb\windows-x64\23.1.0'
    $malformedRoot = Join-Path $cupRoot 'components\unknown-component'
    New-Item -ItemType Directory -Force -Path $invalidPackage, $malformedRoot | Out-Null
    $quarantine = Invoke-Cup -CommandArgs @('repair')
    Assert-ContainsPathText $quarantine "Quarantined invalid package '$invalidPackage'"
    Assert-Contains $quarantine 'unknown component'
    Assert-PathMissing $invalidPackage
    $preserved = Get-ChildItem (Join-Path $cupRoot 'recovery') `
        -Directory -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq 'package' }
    if (@($preserved).Count -eq 0) {
        Fail-Test 'quarantined package was not preserved under recovery'
    }
    Assert-PathExists $malformedRoot
    Remove-Item -LiteralPath $malformedRoot -Recurse -Force
    Assert-PathExists (Join-Path $packageRoot 'info.txt')

    # Invalid state is preserved and rebuilt from complete valid package evidence.
    Write-Utf8NoBom -Path $stateFile -Lines @('unexpected.key=value')
    $rebuilt = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $rebuilt 'Preserved invalid state as'
    Assert-PathExists "$stateFile.invalid"
    Assert-Contains (Get-Content -LiteralPath $stateFile -Raw) 'clang@23.1.0'
    $validState = Join-Path $Script:CupTestRoot 'state.valid'
    Copy-Item -LiteralPath $stateFile -Destination $validState -Force

    # Invalid state is not guessed while a package transaction is pending.
    Write-Utf8NoBom -Path $stateFile -Lines @('unexpected.key=value')
    Write-Utf8NoBom -Path $transactionFile -Lines @(
        'format=2',
        'operation=install',
        'component=compiler',
        'tool=clang',
        'target_platform=windows-x64',
        'package_version=23.1.0',
        'temporary_name=install-compiler-clang-windows-x64-23.1.0-test'
    )
    $invalidStateHash = Get-Sha256Lower -Path $stateFile
    $ambiguousState = Invoke-Cup -CommandArgs @('repair') -ExpectFailure
    Assert-Contains $ambiguousState `
        'interrupted operation cannot be recovered safely'
    Assert-Equals (Get-Sha256Lower -Path $stateFile) $invalidStateHash
    Assert-PathExists $transactionFile
    Remove-Item -LiteralPath $transactionFile -Force
    Copy-Item -LiteralPath $validState -Destination $stateFile -Force
    Remove-Item -LiteralPath "$stateFile.invalid" -Force -ErrorAction SilentlyContinue

    # A malformed generation journal is the first blocker; state/staging evidence stays untouched.
    Write-Utf8NoBom -Path $stateFile -Lines @('unexpected.key=value')
    Write-Utf8NoBom -Path $transactionFile -Lines @(
        'format=2',
        'operation=cup-generation',
        'target_release_sha256=not-a-digest',
        'temporary_name=cup-update-malformed'
    )
    $malformedStaging = Join-Path $cupRoot 'staging\cup-update-malformed'
    New-Item -ItemType Directory -Force -Path $malformedStaging | Out-Null
    $invalidStateHash = Get-Sha256Lower -Path $stateFile
    $invalidGenerationHash = Get-Sha256Lower -Path $transactionFile
    $malformedGeneration = Invoke-Cup -CommandArgs @('repair') -ExpectFailure
    Assert-Contains $malformedGeneration 'interrupted operation cannot be recovered safely'
    Assert-Equals (Get-Sha256Lower -Path $stateFile) $invalidStateHash
    Assert-Equals (Get-Sha256Lower -Path $transactionFile) $invalidGenerationHash
    Assert-PathMissing "$stateFile.invalid"
    Assert-PathExists $malformedStaging
    Remove-Item -LiteralPath $malformedStaging -Recurse -Force
    Remove-Item -LiteralPath $transactionFile -Force
    Copy-Item -LiteralPath $validState -Destination $stateFile -Force

    # A generic malformed journal likewise preserves unrelated state/staging evidence.
    Write-Utf8NoBom -Path $transactionFile -Lines @('not-a-valid-journal')
    $ambiguousStaging = Join-Path $cupRoot 'staging\ambiguous-data'
    New-Item -ItemType Directory -Force -Path $ambiguousStaging | Out-Null
    $stateHash = Get-Sha256Lower -Path $stateFile
    $invalidJournal = Invoke-Cup -CommandArgs @('repair') -ExpectFailure
    Assert-Contains $invalidJournal 'transaction.txt is invalid'
    Assert-PathExists $transactionFile
    Assert-Equals (Get-Sha256Lower -Path $stateFile) $stateHash
    Assert-PathExists $ambiguousStaging
    $invalidJournalHash = Get-Sha256Lower -Path $transactionFile
    Invoke-Cup -CommandArgs @('list') | Out-Null
    Assert-Equals (Get-Sha256Lower -Path $transactionFile) $invalidJournalHash
    Remove-Item -LiteralPath $ambiguousStaging -Recurse -Force
    Remove-Item -LiteralPath $transactionFile -Force

    # Without ambiguous transaction evidence, stale staging is deterministic garbage.
    $staleStaging = Join-Path $cupRoot 'staging\stale-data'
    New-Item -ItemType Directory -Force -Path $staleStaging | Out-Null
    Invoke-Cup -CommandArgs @('repair') | Out-Null
    Assert-PathMissing $staleStaging

    # Host mismatch is metadata corruption, not a second physical host dimension.
    $foreignMetadataRoot = New-InstalledPackageFixture -Component debugger -Tool lldb `
        -Version '23.2.0' -Entries @('lldb')
    $infoPath = Join-Path $foreignMetadataRoot 'info.txt'
    $lines = foreach ($line in Get-Content -LiteralPath $infoPath) {
        if ($line -ceq 'platform.host=windows-x64') { 'platform.host=linux-x64' } else { $line }
    }
    Write-Utf8NoBom -Path $infoPath -Lines $lines
    Write-TestPackageManifest -PackageRoot $foreignMetadataRoot
    $foreignRepair = Invoke-Cup -CommandArgs @('repair')
    Assert-ContainsPathText $foreignRepair "Quarantined invalid package '$foreignMetadataRoot'"
    Assert-PathMissing $foreignMetadataRoot
    Assert-NotContains (Get-Content -LiteralPath $stateFile -Raw) 'lldb@23.2.0'

    # Invalid preferences are preserved; absence afterwards means no user preference.
    Invoke-Cup -CommandArgs @('config', 'set', 'compiler', 'clang') | Out-Null
    $preferences = Join-Path $cupRoot 'config\preferences.txt'
    Write-Utf8NoBom -Path $preferences -Lines @('unexpected.key=value')
    $preferenceRepair = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $preferenceRepair 'Preserved invalid preferences as'
    Assert-PathMissing $preferences
    Assert-PathExists "$preferences.invalid"

    # Development repair preserves a malformed catalog without synthesizing a replacement.
    $catalog = Join-Path $cupRoot 'config\catalog.cfg'
    Write-Utf8NoBom -Path $catalog -Lines @('broken catalog')
    $catalogRepair = Invoke-Cup -CommandArgs @('repair') -ExpectFailure
    Assert-Contains $catalogRepair 'Preserved invalid catalog as'
    Assert-Contains $catalogRepair 'development catalog is unavailable'
    Assert-PathExists "$catalog.invalid"
    Assert-PathMissing $catalog

    # A well-formed future format is not corruption and must not be downgraded to the seed.
    Remove-Item -LiteralPath "$catalog.invalid" -Force
    Write-Utf8NoBom -Path $catalog -Lines @(
        'format=2',
        'revision=999',
        'update_url=https://example.invalid/catalog.cfg'
    )
    $futureCatalogHash = Get-Sha256Lower -Path $catalog
    $futureFailure = Invoke-Cup -CommandArgs @('repair') -ExpectFailure
    Assert-Contains $futureFailure 'newer unsupported format'
    Assert-Equals (Get-Sha256Lower -Path $catalog) $futureCatalogHash
    Assert-PathMissing "$catalog.invalid"
    Copy-Item -LiteralPath (Join-Path $Script:CupTestDevRoot 'config\catalog.cfg') `
        -Destination $catalog -Force

    # The proven uninstall journal lifecycle remains distinct from generation recovery.
    Write-Utf8NoBom -Path $transactionFile -Lines @(
        'format=2',
        'operation=uninstall',
        'phase=scheduled',
        'temporary_name=.cup-uninstall-fixture',
        'token=fixture',
        'error=0'
    )
    $staleHelper = Join-Path (Split-Path -Parent $cupRoot) '.cup-uninstall-helper-fixture.exe'
    Write-Utf8NoBom -Path $staleHelper -Lines @('stale helper')
    $uninstallPending = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $uninstallPending "Cancelled interrupted cup uninstall in phase 'scheduled'."
    Assert-PathMissing $transactionFile
    Assert-PathMissing $staleHelper

    Write-Utf8NoBom -Path $transactionFile -Lines @(
        'format=2',
        'operation=uninstall',
        'phase=failed',
        'temporary_name=.cup-uninstall-fixture',
        'token=fixture',
        'error=6'
    )
    $uninstallFailed = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $uninstallFailed 'Acknowledged failed cup uninstall (error 6).'
    Assert-PathMissing $transactionFile
    $doctor = Invoke-Cup -CommandArgs @('doctor')
    Assert-Contains $doctor "Warning: installed package 'compiler:clang@23.1.0' is not listed by the current catalog."
    Assert-Contains $doctor 'Doctor found 1 warning(s), but no blocking issues.'
    Assert-NotContains $doctor 'Error:'
    Assert-NotContains $doctor 'Issue:'
    Assert-NotContains $doctor 'Incomplete:'

    Write-Host 'Windows repair tests passed.'
} finally {
    Remove-TestEnvironment
}
