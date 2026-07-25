# Purpose: Exercises conservative Windows recovery, CUP-update executable
# preservation, journal blockers, and foreign-host state/package trees.

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

function Invoke-CupUpdateHelperProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $true)]
        [string]$Token
    )

    $pipe = [System.IO.Pipes.AnonymousPipeServerStream]::new(
        [System.IO.Pipes.PipeDirection]::Out,
        [System.IO.HandleInheritability]::Inheritable)
    $process = New-Object System.Diagnostics.Process
    try {
        $clientHandle = $pipe.GetClientHandleAsString()
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $ExecutablePath
        $startInfo.Arguments = (@(
            "--internal-cup-update-helper",
            $Token,
            $clientHandle
        ) | ForEach-Object { ConvertTo-NativeArgument -Argument $_ }) -join ' '
        $startInfo.WorkingDirectory = $Script:CupTestDevRoot
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $process.StartInfo = $startInfo

        if (-not $process.Start()) {
            Fail-Test "failed to start detached CUP update helper"
        }
        $pipe.DisposeLocalCopyOfClientHandle()
        $pipe.Dispose()
        $pipe = $null

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            try { $process.Kill() } catch { }
            $process.WaitForExit()
            Fail-Test "detached CUP update helper did not exit"
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.Result.TrimEnd([char[]]"`r`n")
        $stderr = $stderrTask.Result.TrimEnd([char[]]"`r`n")
        $parts = [System.Collections.Generic.List[string]]::new()
        if ($stdout.Length -gt 0) { $parts.Add($stdout) }
        if ($stderr.Length -gt 0) { $parts.Add($stderr) }

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = ($parts -join "`n")
        }
    } finally {
        if ($null -ne $pipe) {
            $pipe.Dispose()
        }
        $process.Dispose()
    }
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
    $helperPath = Join-Path $cupRoot "helpers\cup-update-helper.exe"
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

    # The detached native helper runs from a separate process, so it may restore
    # cup.exe. The incomplete new generation forces its real rollback path.
    $helperRollback = Invoke-CupUpdateHelperProcess -ExecutablePath $helperPath `
        -Token "recovery-unsafe-rollback"
    if ($helperRollback.ExitCode -eq 0) {
        Fail-Test "native helper unexpectedly committed an incomplete CUP generation"
    }
    Assert-Contains $helperRollback.Output "Rolled back interrupted cup update transaction."
    Assert-Equals (Get-Sha256Lower -Path $binaryPath) $safeBinaryHash
    Assert-Equals (Get-Sha256Lower -Path $uninstallPath) $safeUninstallHash
    Assert-Equals (Get-Sha256Lower -Path $platformChecksumsPath) $safeChecksumsHash
    Assert-PathMissing $transactionPath
    Assert-PathMissing $unsafeStaging
    $resultPath = Join-Path $cupRoot "cup-update-result.txt"
    Assert-Contains ((Get-Content -LiteralPath $resultPath) -join "`n") "status=failed"
    Remove-Item -LiteralPath $resultPath -Force
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

    # Exercise a complete helper commit through the same inherited-handle
    # handoff used by production Windows updates.
    $helperName = "cup-update-helper-commit-test"
    $helperStaging = Join-Path $stagingRoot $helperName
    New-Item -ItemType Directory -Force -Path $helperStaging | Out-Null
    Copy-Item -LiteralPath $binaryPath `
        -Destination (Join-Path $helperStaging "binary.new")
    Copy-Item -LiteralPath $uninstallPath `
        -Destination (Join-Path $helperStaging "uninstall.new")
    Copy-Item -LiteralPath $platformChecksumsPath `
        -Destination (Join-Path $helperStaging "platform-checksums.new")
    Copy-Item -LiteralPath (Join-Path $cupRoot "config\packages.cfg") `
        -Destination (Join-Path $helperStaging "manifest.new")
    Copy-Item -LiteralPath (Join-Path $cupRoot "config\install.cfg") `
        -Destination (Join-Path $helperStaging "install-config.new")
    Copy-Item -LiteralPath (Join-Path $cupRoot "config\SHA256SUMS.common") `
        -Destination (Join-Path $helperStaging "common-checksums.new")
    $helperBinaryHash = Get-Sha256Lower -Path $binaryPath
    Write-CupUpdateJournal -Path $transactionPath -TemporaryName $helperName `
        -Token "helper-commit"

    $helperCommit = Invoke-CupUpdateHelperProcess -ExecutablePath $helperPath `
        -Token "helper-commit"
    if ($helperCommit.ExitCode -ne 0) {
        Fail-Test "native helper commit failed:`n$($helperCommit.Output)"
    }
    Assert-Equals (Get-Sha256Lower -Path $binaryPath) $helperBinaryHash
    Assert-PathMissing $transactionPath
    Assert-PathMissing $helperStaging
    Assert-Contains ((Get-Content -LiteralPath $resultPath) -join "`n") "status=success"
    Remove-Item -LiteralPath $resultPath -Force
    Assert-CupHealthy

    $foreignHost = "linux-x64"
    $foreignTree = Join-Path $cupRoot "components\compiler\clang\$foreignHost\$foreignHost\22.1.5"
    New-Item -ItemType Directory -Force -Path $foreignTree | Out-Null
    $state = [System.Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $statePath)) { $state.Add($line) }
    $state.Add("installed.compiler.$foreignHost.$foreignHost=clang@22.1.5")
    Write-Utf8NoBom -Path $statePath -Lines $state

    $foreignDoctor = Invoke-Cup -CommandArgs @("doctor") -ExpectFailure
    Assert-Contains $foreignDoctor "record(s) for foreign hosts"
    Assert-Contains $foreignDoctor "foreign-host package tree(s)"
    $repair = Invoke-Cup -CommandArgs @("repair")
    Assert-Contains $repair "Preserved 1 foreign-host package tree(s)"
    Assert-PathExists $foreignTree
    Assert-Contains ((Get-Content -LiteralPath $statePath) -join "`n") `
        "installed.compiler.$foreignHost.$foreignHost=clang@22.1.5"
    $foreignBlocked = Invoke-Cup -CommandArgs @("list") -ExpectFailure
    Assert-Contains $foreignBlocked "foreign host"

    $cleanState = Get-Content -LiteralPath $statePath | Where-Object {
        -not $_.StartsWith("installed.compiler.$foreignHost.$foreignHost=", [StringComparison]::Ordinal)
    }
    Write-Utf8NoBom -Path $statePath -Lines $cleanState
    Remove-Item -LiteralPath (Join-Path $cupRoot "components\compiler\clang\$foreignHost") `
        -Recurse -Force
    Assert-CupHealthy

    Write-Host "Windows recovery tests passed."
} finally {
    Remove-TestEnvironment
}
