# Purpose: Runs one Windows integration suite with consistent failure reporting and cleanup.

param([Parameter(Mandatory = $true)][string]$CupExecutablePath)
. (Join-Path $PSScriptRoot "common.ps1")

if ([string]::IsNullOrWhiteSpace($CupExecutablePath)) {
    Fail-Test "cup executable path is empty"
}
Resolve-Path -LiteralPath $CupExecutablePath | Out-Null
$harnessRoot = New-IsolatedTestRoot -Name "native harness with spaces"
try {
    $scriptPath = Join-Path $harnessRoot "expected failure.cmd"
    Set-Content -LiteralPath $scriptPath -Encoding ascii -NoNewline -Value (
        "@echo off`r`n" +
        "echo native stdout`r`n" +
        "echo native stderr 1>&2`r`n" +
        "exit /b 7`r`n")

    $result = Invoke-NativeProcess -FilePath (Get-CommandProcessor) `
        -Arguments @('/d', '/c', 'call', $scriptPath) `
        -WorkingDirectory $harnessRoot
    Assert-Equals ([string]$result.ExitCode) "7"
    Assert-Contains $result.Output "native stdout"
    Assert-Contains $result.Output "native stderr"

    Write-Host "Windows native process harness tests passed."
} finally {
    if (Test-Path -LiteralPath $harnessRoot) {
        Remove-Item -LiteralPath $harnessRoot -Recurse -Force
    }
}
