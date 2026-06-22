param([Parameter(Mandatory = $true)][string]$CupPath)
. (Join-Path $PSScriptRoot "common.ps1")

Resolve-Path $CupPath | Out-Null
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "cup native harness " + [guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
    $scriptPath = Join-Path $testRoot "expected failure.cmd"
    Set-Content -LiteralPath $scriptPath -Encoding ascii -NoNewline -Value (
        "@echo off`r`n" +
        "echo native stdout`r`n" +
        "echo native stderr 1>&2`r`n" +
        "exit /b 7`r`n")

    $result = Invoke-NativeProcess -FilePath $env:ComSpec `
        -Arguments @('/d', '/c', 'call', $scriptPath) `
        -WorkingDirectory $testRoot
    Assert-Equals ([string]$result.ExitCode) "7"
    Assert-Contains $result.Output "native stdout"
    Assert-Contains $result.Output "native stderr"

    Write-Host "Windows native process harness tests passed."
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
