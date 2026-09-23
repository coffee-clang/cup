# Exercises a synchronized overlapping Windows install and verifies
# that an active operation blocks a second mutation without corrupting state.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function Start-CupCapture {
    param([string[]]$Arguments)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Script:CupTestExecutable
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        ConvertTo-NativeArgument -Argument $_
    }) -join ' ')
    $startInfo.WorkingDirectory = $Script:CupTestDevRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        $process.Dispose()
        Fail-Test 'failed to start concurrent cup process'
    }
    return [pscustomobject]@{
        Process = $process
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
    }
}

function Complete-CupCapture {
    param($Capture)

    try {
        if (-not $Capture.Process.WaitForExit(30000)) {
            Stop-TestProcessTree -Process $Capture.Process
            Fail-Test 'concurrent cup process did not exit'
        }
        $stdout = $Capture.Stdout.Result.TrimEnd([char[]]"`r`n")
        $stderr = $Capture.Stderr.Result.TrimEnd([char[]]"`r`n")
        return [pscustomobject]@{
            ExitCode = $Capture.Process.ExitCode
            Output = (@($stdout, $stderr) | Where-Object { $_.Length -gt 0 }) -join "`n"
        }
    } finally {
        $Capture.Process.Dispose()
    }
}

$server = $null
$captureA = $null

try {
    Initialize-TestEnvironment -Name 'concurrency' -ExecutablePath $CupExecutablePath
    New-TestPackage -Component 'compiler' -Tool 'clang' -Version '23.1.0' `
        -Entries @('clang', 'clang++')

    $helper = Get-TestHelperPath -Name 'network-helper'
    $serverRoot = Join-Path $Script:CupTestRoot 'http-root'
    $ready = Join-Path $Script:CupTestRoot 'http-ready'
    $requestReady = Join-Path $Script:CupTestRoot 'http-request-ready'
    $serverLog = Join-Path $Script:CupTestRoot 'http-server.log'
    New-Item -ItemType Directory -Force -Path $serverRoot | Out-Null

    $archiveName = 'clang-23.1.0-windows-x64-windows-x64.zip'
    $archive = Join-Path $Script:CupTestRoot "artifacts\$archiveName"
    $sha256 = Get-Sha256Lower -Path $archive
    Copy-Item -LiteralPath $archive -Destination (Join-Path $serverRoot $archiveName) -Force
    Remove-Item -LiteralPath (Join-Path $Script:CupTestHome ".cup\cache\$sha256") `
        -Force -ErrorAction SilentlyContinue

    $serverArguments = @(
        'http-server', '--root', $serverRoot, '--port', '0',
        '--ready-file', $ready, '--request-file', $requestReady,
        '--delay-ms', '3000'
    )
    $server = Start-TestHelperProcess -FilePath $helper `
        -ArgumentList $serverArguments `
        -RedirectStandardOutput $serverLog -RedirectStandardError "$serverLog.err" `
        -Hidden

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $ready) -and
           [DateTime]::UtcNow -lt $deadline) {
        if ($server.HasExited) {
            $details = Get-Content -LiteralPath "$serverLog.err" -Raw `
                -ErrorAction SilentlyContinue
            Fail-Test "concurrency package server exited before becoming ready: $details"
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path -LiteralPath $ready)) {
        Fail-Test 'concurrency package server did not become ready'
    }

    $portText = (Get-Content -LiteralPath $ready -Raw).Trim()
    $port = 0
    if (-not [int]::TryParse($portText, [ref]$port) -or
        $port -lt 1 -or $port -gt 65535) {
        Fail-Test "concurrency package server reported invalid port: $portText"
    }

    $catalog = Join-Path $Script:CupTestDevRoot 'config\catalog.cfg'
    $runtimeCatalog = Join-Path $Script:CupTestHome '.cup\config\catalog.cfg'
    $catalogBackup = Join-Path $Script:CupTestRoot 'catalog.cfg.original'
    Copy-Item -LiteralPath $catalog -Destination $catalogBackup -Force
    Set-PackageCatalogArtifact `
        -Tool 'clang' `
        -Version '23.1.0' `
        -Format 'zip' `
        -Url "http://127.0.0.1:$port/$archiveName" `
        -Sha256 $sha256

    $env:CUP_INSTALL_ALLOW_INSECURE = '1'
    $env:NO_PROXY = '127.0.0.1'
    $env:no_proxy = '127.0.0.1'
    $captureA = Start-CupCapture -Arguments @('install', 'compiler', 'clang@23.1.0')
    $transaction = Join-Path $Script:CupTestHome '.cup\transaction.txt'
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $requestReady) -and
           [DateTime]::UtcNow -lt $deadline) {
        if ($captureA.Process.HasExited) {
            $early = Complete-CupCapture -Capture $captureA
            $captureA = $null
            Fail-Test ("first install exited before reaching the synchronized download`n" +
                "[$($early.ExitCode)] $($early.Output)")
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path -LiteralPath $requestReady)) {
        Fail-Test 'first install did not reach the synchronized download'
    }

    # A public read-only command shares the runtime lock boundary and cannot
    # observe a half-mutated root while the install owns the exclusive lock.
    $listBusy = Start-CupCapture -Arguments @('list')
    $listBusyResult = Complete-CupCapture -Capture $listBusy
    if ($listBusyResult.ExitCode -eq 0) {
        Fail-Test 'read-only list succeeded while a mutation held cup.lock'
    }
    Assert-Contains $listBusyResult.Output 'another cup operation is currently running'

    $doctorBusy = Start-CupCapture -Arguments @('doctor')
    $doctorBusyResult = Complete-CupCapture -Capture $doctorBusy
    if ($doctorBusyResult.ExitCode -eq 0) {
        Fail-Test 'doctor succeeded while a mutation held cup.lock'
    }
    Assert-Contains $doctorBusyResult.Output 'another cup operation is currently running'

    $captureB = Start-CupCapture -Arguments @('install', 'compiler', 'clang@23.1.0')
    $resultB = Complete-CupCapture -Capture $captureB
    if ($resultB.ExitCode -eq 0) {
        Fail-Test ("overlapping install was not blocked while the first operation was active`n" +
            "[$($resultB.ExitCode)] $($resultB.Output)")
    }

    $resultA = Complete-CupCapture -Capture $captureA
    $captureA = $null
    if ($resultA.ExitCode -ne 0) {
        Fail-Test ("first synchronized install failed`n" +
            "[$($resultA.ExitCode)] $($resultA.Output)")
    }

    Copy-Item -LiteralPath $catalogBackup -Destination $catalog -Force
    Copy-Item -LiteralPath $catalogBackup -Destination $runtimeCatalog -Force
    $listAfter = Start-CupCapture -Arguments @('list')
    $listAfterResult = Complete-CupCapture -Capture $listAfter
    if ($listAfterResult.ExitCode -ne 0) {
        Fail-Test "read-only list did not recover after the mutation completed: $($listAfterResult.Output)"
    }

    Assert-Contains $resultA.Output 'Installed compiler clang@23.1.0'
    if (-not ($resultB.Output.Contains('another cup operation is currently running') -or
              $resultB.Output.Contains('a package transaction is active or requires recovery'))) {
        Fail-Test (
            'overlapping install did not report the active operation or ' +
            "transaction: $($resultB.Output)")
    }
    Assert-NotContains $resultB.Output 'already installed'

    Assert-CupHealthy
    Assert-PathMissing $transaction
    $stagingItems = @(Get-ChildItem (Join-Path $Script:CupTestHome '.cup\staging') `
        -Force -ErrorAction SilentlyContinue)
    if ($stagingItems.Count -ne 0) {
        Fail-Test 'concurrent installs left temporary paths behind'
    }
    Assert-Contains (Invoke-Cup -CommandArgs @('info', 'compiler')) `
        'compiler [windows-x64]: clang@23.1.0 (stable)'
    Assert-Equals (Invoke-ManagedCommand -Name 'clang') `
        'clang-23.1.0-windows-x64:clang'

    Write-Host 'Windows concurrency tests passed.'
} finally {
    if ($null -ne $captureA) {
        try {
            if (-not $captureA.Process.HasExited) {
                Stop-TestProcessTree -Process $captureA.Process
            }
        } catch {
            # Cleanup is best effort.
        }
        try {
            $captureA.Process.Dispose()
        } catch {
            # Cleanup is best effort.
        }
    }
    if ($null -ne $server) {
        try {
            if (-not $server.HasExited) {
                Stop-TestProcessTree -Process $server
            }
        } catch {
            # Cleanup is best effort.
        }
        $server.Dispose()
    }
    Remove-TestEnvironment
}
