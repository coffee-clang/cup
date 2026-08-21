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
        Fail-Test "failed to start concurrent cup process"
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
            try {
                & taskkill.exe /PID $Capture.Process.Id /T /F 2>&1 | Out-Null
            } catch {
                # Cleanup is best effort.
            }
            [void]$Capture.Process.WaitForExit(10000)
            Fail-Test "concurrent cup process did not exit"
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
$originalAllowInsecure = Get-Item -LiteralPath Env:CUP_INSTALL_ALLOW_INSECURE `
    -ErrorAction SilentlyContinue

try {
    Initialize-TestEnvironment -Name "concurrency" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" `
        -Entries @("clang", "clang++")

    $configuration = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_CONFIGURATION)) {
        "development"
    } else {
        $env:CUP_TEST_CONFIGURATION
    }
    $helper = Join-Path $Script:CupTestBuildRoot `
        "windows-x64\$configuration\tests\helpers\network-helper.exe"
    Assert-PathExists $helper

    $port = 0
    $serverRoot = Join-Path $Script:CupTestRoot "http-root"
    $ready = Join-Path $Script:CupTestRoot "http-ready"
    $requestReady = Join-Path $Script:CupTestRoot "http-request-ready"
    $serverLog = Join-Path $Script:CupTestRoot "http-server.log"
    New-Item -ItemType Directory -Force -Path $serverRoot | Out-Null

    $cacheDir = Join-Path $Script:CupTestHome `
        ".cup\cache\compiler\clang\windows-x64\windows-x64\22.1.5"
    $archiveName = "clang-22.1.5-windows-x64-windows-x64.zip"
    Move-Item -LiteralPath (Join-Path $cacheDir $archiveName) `
        -Destination (Join-Path $serverRoot $archiveName)
    $checksumRoot = Join-Path $serverRoot "22.1.5\windows-x64\windows-x64"
    New-Item -ItemType Directory -Force -Path $checksumRoot | Out-Null
    Move-Item -LiteralPath (Join-Path $cacheDir "SHA256SUMS") `
        -Destination (Join-Path $checksumRoot "SHA256SUMS")
    Remove-Item -LiteralPath (Join-Path $Script:CupTestHome ".cup\cache\compiler\clang") `
        -Recurse -Force

    $serverArguments = @(
        "http-server", "--root", $serverRoot, "--port", "$port",
        "--ready-file", $ready, "--request-file", $requestReady,
        "--delay-ms", "3000"
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
        Fail-Test "concurrency package server did not become ready"
    }

    $portText = (Get-Content -LiteralPath $ready -Raw).Trim()
    $parsedPort = 0
    if (-not [int]::TryParse($portText, [ref]$parsedPort) -or
        $parsedPort -lt 1 -or $parsedPort -gt 65535) {
        Fail-Test "concurrency package server reported invalid port: $portText"
    }
    $port = $parsedPort

    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $catalogOriginal = @(Get-Content -LiteralPath $catalog)
    $key = "compiler.clang.windows-x64.windows-x64"
    $base = "http://127.0.0.1:$port"
    $changed = 0
    $updated = foreach ($line in Get-Content -LiteralPath $catalog) {
        if ($line.StartsWith("$key.url_template=", [StringComparison]::Ordinal)) {
            $changed++
            "$key.url_template=$base/clang-{version}-{host_platform}-{target_platform}.{format}"
        } elseif ($line.StartsWith("$key.checksum_url_template=", [StringComparison]::Ordinal)) {
            $changed++
            (
                "$key.checksum_url_template=$base/{version}/" +
                "{host_platform}/{target_platform}/SHA256SUMS")
        } else {
            $line
        }
    }
    if ($changed -ne 2) {
        Fail-Test "could not configure the concurrency package server"
    }
    Write-Utf8NoBom -Path $catalog -Lines $updated

    $env:CUP_INSTALL_ALLOW_INSECURE = "1"
    $captureA = Start-CupCapture -Arguments @("install", "compiler", "clang@stable")
    $transaction = Join-Path $Script:CupTestHome ".cup\transaction.txt"
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
        Fail-Test "first install did not reach the synchronized download"
    }

    $captureB = Start-CupCapture -Arguments @("install", "compiler", "clang@stable")
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
    Assert-Contains $resultA.Output "Installed compiler clang@22.1.5"
    if (-not ($resultB.Output.Contains("another cup operation is currently running") -or
              $resultB.Output.Contains("a package transaction is active or requires recovery"))) {
        Fail-Test (
            "overlapping install did not report the active operation or " +
            "transaction: $($resultB.Output)")
    }
    Assert-NotContains $resultB.Output "already installed"

    Write-Utf8NoBom -Path $catalog -Lines $catalogOriginal
    Assert-CupHealthy
    Assert-PathMissing $transaction
    $stagingItems = @(Get-ChildItem (Join-Path $Script:CupTestHome ".cup\staging") `
        -Force -ErrorAction SilentlyContinue)
    if ($stagingItems.Count -ne 0) {
        Fail-Test "concurrent installs left temporary paths behind"
    }
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) `
        "compiler [windows-x64]: clang@22.1.5 (stable)"
    Assert-Equals (Invoke-ManagedCommand -Name "clang") `
        "clang-22.1.5-windows-x64:clang"

    Write-Host "Windows concurrency tests passed."
} finally {
    if ($null -ne $captureA) {
        try {
            if (-not $captureA.Process.HasExited) {
                try {
                    & taskkill.exe /PID $captureA.Process.Id /T /F 2>&1 | Out-Null
                } catch {
                    # Cleanup is best effort.
                }
                [void]$captureA.Process.WaitForExit(10000)
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
                try {
                    & taskkill.exe /PID $server.Id /T /F 2>&1 | Out-Null
                } catch {
                    # Cleanup is best effort.
                }
                [void]$server.WaitForExit(10000)
            }
        } catch {
            # Cleanup is best effort.
        }
        $server.Dispose()
    }
    if ($null -eq $originalAllowInsecure) {
        Remove-Item -LiteralPath Env:CUP_INSTALL_ALLOW_INSECURE -ErrorAction SilentlyContinue
    } else {
        $env:CUP_INSTALL_ALLOW_INSECURE = $originalAllowInsecure.Value
    }
    Remove-TestEnvironment
}
