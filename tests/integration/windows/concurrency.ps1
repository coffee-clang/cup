# Purpose: Exercises Windows process-lock contention and coherent concurrent installation.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

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
            try { $Capture.Process.Kill() } catch { }
            $Capture.Process.WaitForExit()
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

try {
    Initialize-TestEnvironment -Name "concurrency" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" `
        -Entries @("clang", "clang++")

    $captureA = Start-CupCapture -Arguments @("install", "compiler", "clang@stable")
    $captureB = Start-CupCapture -Arguments @("install", "compiler", "clang@stable")
    $resultA = Complete-CupCapture -Capture $captureA
    $resultB = Complete-CupCapture -Capture $captureB

    $successCount = @(@($resultA, $resultB) | Where-Object { $_.ExitCode -eq 0 }).Count
    if ($successCount -ne 1) {
        Fail-Test ("expected exactly one concurrent install to succeed`n" +
            "A [$($resultA.ExitCode)]: $($resultA.Output)`n" +
            "B [$($resultB.ExitCode)]: $($resultB.Output)")
    }
    $failed = if ($resultA.ExitCode -ne 0) { $resultA.Output } else { $resultB.Output }
    if (-not ($failed.Contains("already installed") -or
              $failed.Contains("another cup operation is currently running") -or
              $failed.Contains("interrupted package transaction must be repaired first"))) {
        Fail-Test "concurrent loser did not report a lock or installation conflict: $failed"
    }

    Assert-CupHealthy
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\transaction.txt")
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
    Remove-TestEnvironment
}
