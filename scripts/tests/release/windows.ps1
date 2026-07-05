param(
    [Parameter(Mandatory=$true)][string]$ReleaseDir,
    [Parameter(Mandatory=$true)][string]$Version
)

$ErrorActionPreference = "Stop"

Push-Location $ReleaseDir
try {
    sha256sum -c SHA256SUMS.common
    sha256sum -c SHA256SUMS.windows-x64
} finally {
    Pop-Location
}

$binary = (Resolve-Path (Join-Path $ReleaseDir "cup-windows-x64.exe")).Path
$actual = & $binary --version
if ($actual -ne "cup $Version") {
    throw "Unexpected version: $actual"
}

powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts/tests/integration/windows/run.ps1 -CupPath $binary

$port = 18080 + (Get-Random -Maximum 1000)
$root = (Resolve-Path $ReleaseDir).Path
$server = Start-Process -FilePath python -ArgumentList @('-m', 'http.server', "$port", '--directory', $root) -PassThru -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 50; $i++) {
        try {
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
    Remove-Item Env:CUP_INSTALL_NO_PATH_PROMPT -ErrorAction SilentlyContinue
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
