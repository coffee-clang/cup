# Purpose: Validates one completed Windows release candidate, native
# executable, and generated installer.

param(
    [Parameter(Mandatory=$true)][string]$ReleaseDir,
    [Parameter(Mandatory=$true)][string]$Version
)

$ErrorActionPreference = "Stop"

function Assert-ChecksumFile {
    param(
        [Parameter(Mandatory=$true)][string]$Directory,
        [Parameter(Mandatory=$true)][string]$ChecksumFile,
        [Parameter(Mandatory=$true)][string[]]$ExpectedNames
    )

    $checksumPath = Join-Path $Directory $ChecksumFile
    if (-not (Test-Path -LiteralPath $checksumPath)) {
        throw "Missing checksum file: $ChecksumFile"
    }

    $seen = @{}
    foreach ($line in Get-Content -LiteralPath $checksumPath) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^([0-9A-Fa-f]{64})\s+\*?(.+)$') {
            throw "Invalid checksum line in ${ChecksumFile}: $line"
        }

        $expectedHash = $Matches[1].ToLowerInvariant()
        $name = $Matches[2]
        if ($name.Contains('/') -or $name.Contains('\\') -or $name.Contains('..')) {
            throw "Unsafe checksum entry in ${ChecksumFile}: $name"
        }
        if ($ExpectedNames -notcontains $name) {
            throw "Unexpected checksum entry in ${ChecksumFile}: $name"
        }

        $path = Join-Path $Directory $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Checksum entry references missing file: $name"
        }

        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Checksum mismatch for ${name}: expected $expectedHash, got $actualHash"
        }
        $seen[$name] = $true
    }

    foreach ($name in $ExpectedNames) {
        if (-not $seen.ContainsKey($name)) {
            throw "Missing checksum entry in ${ChecksumFile}: $name"
        }
    }
}

Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.common" `
    -ExpectedNames @("packages.cfg", "install.cfg", "install.sh", "install.ps1")
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.windows-x64" `
    -ExpectedNames @("cup-windows-x64.exe", "uninstall.ps1", "release.txt")

$binary = (Resolve-Path (Join-Path $ReleaseDir "cup-windows-x64.exe")).Path
$actual = & $binary --version
if ($actual -ne "cup $Version") {
    throw "Unexpected version: $actual"
}

powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File tests/integration/windows/run.ps1 -CupPath $binary

$port = 18080 + (Get-Random -Maximum 1000)
$root = (Resolve-Path $ReleaseDir).Path
$configuration = if ($env:CUP_TEST_CONFIGURATION) { $env:CUP_TEST_CONFIGURATION } else { "development" }
$helper = Join-Path (Get-Location) "build\windows-x64\$configuration\tests\helpers\http-server.exe"
if (-not (Test-Path -LiteralPath $helper)) {
    throw "HTTP test helper is not built: $helper"
}
$ready = Join-Path $env:RUNNER_TEMP "cup-http-ready-$PID"
Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
$serverArgs = @('--root', $root, '--port', "$port", '--ready-file', $ready)
$server = Start-Process -FilePath $helper `
    -ArgumentList $serverArgs `
    -PassThru `
    -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 50; $i++) {
        try {
            if (-not (Test-Path -LiteralPath $ready)) { throw "not ready" }
            Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/release.txt" | Out-Null
            break
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    $testProfile = Join-Path $env:RUNNER_TEMP "cup-installer-profile"
    if (Test-Path -LiteralPath $testProfile) { Remove-Item -LiteralPath $testProfile -Recurse -Force }
    New-Item -ItemType Directory -Path $testProfile | Out-Null
    $env:USERPROFILE = $testProfile
    $env:CUP_INSTALL_ALLOW_INSECURE = "1"
    $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port"
    $env:CUP_INSTALL_NO_PATH_PROMPT = "1"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ReleaseDir "install.ps1")
    $installed = Join-Path $env:USERPROFILE ".cup\bin\cup.exe"
    $installedVersion = & $installed --version
    if ($installedVersion -ne "cup $Version") {
        throw "Unexpected installed version: $installedVersion"
    }
    & $installed doctor
} finally {
    Remove-Item Env:CUP_INSTALL_BASE_URL -ErrorAction SilentlyContinue
    Remove-Item Env:CUP_INSTALL_ALLOW_INSECURE -ErrorAction SilentlyContinue
    Remove-Item Env:CUP_INSTALL_NO_PATH_PROMPT -ErrorAction SilentlyContinue
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
}
