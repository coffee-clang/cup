# Purpose: Runs the complete Windows source regression suite in UCRT64.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Command
    )

    & $Command
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode"
    }
}

Invoke-CheckedNative "Windows source tests" {
    make PLATFORM=windows-x64 test
}
Invoke-CheckedNative "Windows binary inspection" {
    make PLATFORM=windows-x64 check-binary
}
