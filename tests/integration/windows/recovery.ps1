# Purpose: Exercises conservative Windows transaction recovery, cup-update
# executable preservation, and journal blockers.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")


function Get-Sha256Lower {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Install-CupAssetsFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CupRoot
    )

    $bin = Join-Path $CupRoot "bin"
    $config = Join-Path $CupRoot "config"
    $helpers = Join-Path $CupRoot "helpers"
    New-Item -ItemType Directory -Force -Path $bin, $config, $helpers | Out-Null

    $binary = Join-Path $bin "cup.exe"
    $helper = Join-Path $helpers "cup-update-helper.exe"
    $uninstall = Join-Path $helpers "uninstall.ps1"
    $packages = Join-Path $config "packages.cfg"
    $installPolicy = Join-Path $config "install.cfg"
    $commonChecksums = Join-Path $config "SHA256SUMS.common"
    $platformChecksums = Join-Path $config "SHA256SUMS.windows-x64"

    Copy-Item -LiteralPath $Script:CupTestExecutable -Destination $binary -Force
    Copy-Item -LiteralPath $Script:CupTestExecutable -Destination $helper -Force
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "config\packages.cfg") `
        -Destination $packages -Force
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "config\install.cfg") `
        -Destination $installPolicy -Force
    Copy-Item -LiteralPath (
        Join-Path $Script:CupTestProjectRoot "scripts\install\uninstall-cup-windows.ps1") `
        -Destination $uninstall -Force

    $installSh = Join-Path $Script:CupTestProjectRoot "scripts\install\install-cup.sh"
    $installPs1 = Join-Path $Script:CupTestProjectRoot "scripts\install\install-cup-windows.ps1"
    Write-Utf8NoBom -Path $commonChecksums -Lines @(
        "$(Get-Sha256Lower -Path $packages)  packages.cfg",
        "$(Get-Sha256Lower -Path $installPolicy)  install.cfg",
        "$(Get-Sha256Lower -Path $installSh)  install.sh",
        "$(Get-Sha256Lower -Path $installPs1)  install.ps1"
    )
    Write-Utf8NoBom -Path $platformChecksums -Lines @(
        "$(Get-Sha256Lower -Path $binary)  cup-windows-x64.exe",
        "$(Get-Sha256Lower -Path $uninstall)  uninstall.ps1",
        ("{0}  release.txt" -f ("0" * 64))
    )
}

function Write-CupUpdateJournal {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$TemporaryName,

        [Parameter(Mandatory = $true)]
        [string]$Token,

        [string]$Phase = "scheduled"
    )

    Write-Utf8NoBom -Path $Path -Lines @(
        "format=1",
        "operation=cup-update",
        "phase=$Phase",
        "temporary_name=$TemporaryName",
        "token=$Token",
        "version=0.0.0",
        "error=0"
    )
}

try {
    Initialize-TestEnvironment -Name "recovery" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $statePath = Join-Path $cupRoot "state.txt"
    $transactionPath = Join-Path $cupRoot "transaction.txt"
    $stagingName = "install-compiler-clang-windows-x64-windows-x64-22.1.5-recovery"
    $stagingPath = Join-Path (Join-Path $cupRoot "staging") $stagingName
    $validState = Get-Content -LiteralPath $statePath

    New-Item -ItemType Directory -Force -Path $stagingPath | Out-Null
    Write-Utf8NoBom -Path $transactionPath -Lines @(
        "format=1",
        "operation=install",
        "component=compiler",
        "tool=clang",
        "host_platform=windows-x64",
        "target_platform=windows-x64",
        "package_version=22.1.5",
        "temporary_name=$stagingName"
    )

    Invoke-Cup -CommandArgs @("help") | Out-Null
    Invoke-Cup -CommandArgs @("--version") | Out-Null
    $blocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $blocked "interrupted package transaction must be repaired first"
    $diagnosis = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $diagnosis "interrupted install transaction detected"

    Write-Utf8NoBom -Path $statePath -Lines @("unexpected.key=value")
    $ambiguous = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $ambiguous "state.txt is missing or invalid while a package transaction is pending"
    Assert-PathExists $transactionPath
    Assert-PathExists $stagingPath
    Write-Utf8NoBom -Path $statePath -Lines $validState
    Remove-Item -LiteralPath $transactionPath -Force
    Remove-Item -LiteralPath $stagingPath -Recurse -Force

    Write-Utf8NoBom -Path $transactionPath -Lines @("not-a-valid-journal")
    $before = (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash
    $invalid = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $invalid "transaction.txt is invalid"
    Assert-PathExists $transactionPath
    Assert-Equals ((Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash) $before
    $stillBlocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $stillBlocked "transaction journal is invalid"
    Remove-Item -LiteralPath $transactionPath -Force

    Install-CupAssetsFixture -CupRoot $cupRoot
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $binaryPath = Join-Path $cupRoot "bin\cup.exe"
    $uninstallPath = Join-Path $cupRoot "helpers\uninstall.ps1"
    $platformChecksumsPath = Join-Path $cupRoot "config\SHA256SUMS.windows-x64"
    $stagingRoot = Join-Path $cupRoot "staging"

    # A crash after the marker but before binary replacement leaves the old
    # cup.exe with partially installed support assets. Repair may roll those
    # assets back only when cup.exe already equals its backup.
    $safeName = "cup-update-safe-rollback-test"
    $safeStaging = Join-Path $stagingRoot $safeName
    New-Item -ItemType Directory -Force -Path $safeStaging | Out-Null
    Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $safeStaging "binary.old")
    Copy-Item -LiteralPath $uninstallPath -Destination (Join-Path $safeStaging "uninstall.old")
    Copy-Item -LiteralPath $platformChecksumsPath `
        -Destination (Join-Path $safeStaging "platform-checksums.old")
    New-Item -ItemType File -Path (Join-Path $safeStaging "committed") | Out-Null
    $safeBinaryHash = Get-Sha256Lower -Path $binaryPath
    $safeUninstallHash = Get-Sha256Lower -Path $uninstallPath
    $safeChecksumsHash = Get-Sha256Lower -Path $platformChecksumsPath

    (Get-Item -LiteralPath $uninstallPath).IsReadOnly = $false
    (Get-Item -LiteralPath $platformChecksumsPath).IsReadOnly = $false
    Write-Utf8NoBom -Path $uninstallPath -Lines @("broken uninstall")
    Write-Utf8NoBom -Path $platformChecksumsPath -Lines @("broken checksums")
    Write-CupUpdateJournal -Path $transactionPath -TemporaryName $safeName `
        -Token "recovery-safe-rollback" -Phase "committing"

    $safeRepair = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $safeRepair "Rolled back interrupted cup update transaction."
    Assert-Equals (Get-Sha256Lower -Path $binaryPath) $safeBinaryHash
    Assert-Equals (Get-Sha256Lower -Path $uninstallPath) $safeUninstallHash
    Assert-Equals (Get-Sha256Lower -Path $platformChecksumsPath) $safeChecksumsHash
    Assert-PathMissing $transactionPath
    Assert-PathMissing $safeStaging
    Assert-CupHealthy

    # A rollback that would replace cup.exe is unsafe inside repair. The command
    # must fail before changing any canonical asset and preserve all evidence.
    $unsafeName = "cup-update-unsafe-rollback-test"
    $unsafeStaging = Join-Path $stagingRoot $unsafeName
    New-Item -ItemType Directory -Force -Path $unsafeStaging | Out-Null
    Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $unsafeStaging "binary.old")
    Copy-Item -LiteralPath $uninstallPath -Destination (Join-Path $unsafeStaging "uninstall.old")
    Copy-Item -LiteralPath $platformChecksumsPath `
        -Destination (Join-Path $unsafeStaging "platform-checksums.old")

    (Get-Item -LiteralPath $uninstallPath).IsReadOnly = $false
    (Get-Item -LiteralPath $platformChecksumsPath).IsReadOnly = $false
    Write-Utf8NoBom -Path $binaryPath -Lines @("broken binary")
    Write-Utf8NoBom -Path $uninstallPath -Lines @("broken uninstall")
    Write-Utf8NoBom -Path $platformChecksumsPath -Lines @("broken checksums")
    $brokenBinaryHash = Get-Sha256Lower -Path $binaryPath
    $brokenUninstallHash = Get-Sha256Lower -Path $uninstallPath
    $brokenChecksumsHash = Get-Sha256Lower -Path $platformChecksumsPath
    Write-CupUpdateJournal -Path $transactionPath -TemporaryName $unsafeName `
        -Token "recovery-unsafe-rollback"

    $unsafeRepair = Invoke-Cup -CommandArgs @("repair") -ExpectFailure
    Assert-Contains $unsafeRepair `
        "interrupted cup update recovery would replace the running executable"
    Assert-Contains $unsafeRepair "interrupted operation cannot be repaired safely"
    Assert-Equals (Get-Sha256Lower -Path $binaryPath) $brokenBinaryHash
    Assert-Equals (Get-Sha256Lower -Path $uninstallPath) $brokenUninstallHash
    Assert-Equals (Get-Sha256Lower -Path $platformChecksumsPath) $brokenChecksumsHash
    Assert-PathExists $transactionPath
    Assert-PathExists (Join-Path $unsafeStaging "binary.old")
    Assert-PathExists (Join-Path $unsafeStaging "uninstall.old")
    Assert-PathExists (Join-Path $unsafeStaging "platform-checksums.old")

    # Reset the isolated fixture after verifying that repair preserved every file.
    Copy-Item -LiteralPath (Join-Path $unsafeStaging "binary.old") `
        -Destination $binaryPath -Force
    Copy-Item -LiteralPath (Join-Path $unsafeStaging "uninstall.old") `
        -Destination $uninstallPath -Force
    Copy-Item -LiteralPath (Join-Path $unsafeStaging "platform-checksums.old") `
        -Destination $platformChecksumsPath -Force
    (Get-Item -LiteralPath $uninstallPath).IsReadOnly = $true
    (Get-Item -LiteralPath $platformChecksumsPath).IsReadOnly = $true
    Remove-Item -LiteralPath $transactionPath -Force
    Remove-Item -LiteralPath $unsafeStaging -Recurse -Force
    Assert-CupHealthy

    # A durable marker may be finalized only when the complete installed
    # generation validates; finalization must not alter cup.exe.
    $committedName = "cup-update-committed-test"
    $committedStaging = Join-Path $stagingRoot $committedName
    New-Item -ItemType Directory -Force -Path $committedStaging | Out-Null
    Copy-Item -LiteralPath $binaryPath `
        -Destination (Join-Path $committedStaging "binary.old")
    Copy-Item -LiteralPath $uninstallPath `
        -Destination (Join-Path $committedStaging "uninstall.old")
    Copy-Item -LiteralPath $platformChecksumsPath `
        -Destination (Join-Path $committedStaging "platform-checksums.old")
    New-Item -ItemType File -Path (Join-Path $committedStaging "committed") | Out-Null
    $committedBinaryHash = Get-Sha256Lower -Path $binaryPath
    Write-CupUpdateJournal -Path $transactionPath -TemporaryName $committedName `
        -Token "recovery-committed" -Phase "committing"

    $committedRepair = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $committedRepair "Completed interrupted cup update transaction."
    Assert-Equals (Get-Sha256Lower -Path $binaryPath) $committedBinaryHash
    Assert-PathMissing $transactionPath
    Assert-PathMissing $committedStaging
    Assert-CupHealthy

    Write-Host "Windows recovery tests passed."
} finally {
    Remove-TestEnvironment
}
