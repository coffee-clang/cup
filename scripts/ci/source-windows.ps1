# Purpose: Runs Windows source tests, the development build, repository
# contracts and the native integration suite against the canonical UCRT64
# dependency prefix prepared by `make deps`.

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

Invoke-CheckedNative "Windows unit tests" {
    & bash.exe -lc "PLATFORM=windows-x64 CUP_TEST_PLATFORM=windows-x64 make test-unit"
}
Invoke-CheckedNative "Windows development build" {
    make PLATFORM=windows-x64
}
Invoke-CheckedNative "Windows binary inspection" {
    make PLATFORM=windows-x64 check-binary
}
Invoke-CheckedNative "Windows repository contracts" {
    & bash.exe -lc "PLATFORM=windows-x64 CUP_TEST_PLATFORM=windows-x64 make test-repository"
}
Invoke-CheckedNative "Windows integration tests" {
    make test-windows
}
