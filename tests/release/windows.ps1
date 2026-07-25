# Purpose: Validates one completed Windows release candidate, native
# executable, and generated installer.

param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseDir,
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$SourceSha
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Verify that each checksum file names exactly the expected immutable assets.
function Assert-ChecksumFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumFile,
        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedNames
    )

    $checksumPath = Join-Path $Directory $ChecksumFile
    if (-not (Test-Path -LiteralPath $checksumPath)) {
        throw "Missing checksum file: $ChecksumFile"
    }

    $seen = @{}
    foreach ($line in Get-Content -LiteralPath $checksumPath) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
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

# Validate the candidate checksums, metadata and native executable.
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.common" `
    -ExpectedNames @("packages.cfg", "install.cfg", "install.sh", "install.ps1")
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.windows-x64" `
    -ExpectedNames @("cup-windows-x64.exe", "uninstall.ps1", "release.txt")

$releaseMetadataPath = Join-Path $ReleaseDir "release.txt"
$releaseMetadata = @(Get-Content -LiteralPath $releaseMetadataPath)
$expectedMetadata = @(
    "format=1",
    "version=$Version",
    "commit=$SourceSha"
)
if ($releaseMetadata.Count -ne $expectedMetadata.Count) {
    throw "release.txt must contain exactly three lines"
}
for ($i = 0; $i -lt $expectedMetadata.Count; $i++) {
    if ($releaseMetadata[$i] -ne $expectedMetadata[$i]) {
        throw "Unexpected release.txt line $($i + 1): $($releaseMetadata[$i])"
    }
}

$binary = (Resolve-Path (Join-Path $ReleaseDir "cup-windows-x64.exe")).Path
$actual = & $binary --version
if ($LASTEXITCODE -ne 0) {
    throw "Release candidate --version failed with exit code $LASTEXITCODE"
}
if ($actual -ne "cup $Version") {
    throw "Unexpected version: $actual"
}

# Serve the candidate locally and smoke-test the generated installer in a fresh profile.
$port = 18080 + (Get-Random -Maximum 1000)
$root = (Resolve-Path $ReleaseDir).Path
if ($env:CUP_TEST_CONFIGURATION) {
    $configuration = $env:CUP_TEST_CONFIGURATION
} else {
    $configuration = "development"
}
$helper = Join-Path (Get-Location) "build\windows-x64\$configuration\tests\helpers\network-helper.exe"
if (-not (Test-Path -LiteralPath $helper)) {
    throw "HTTP test helper is not built: $helper"
}
$ready = Join-Path $env:RUNNER_TEMP "cup-http-ready-$PID"
Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
$serverArgs = @('http-server', '--root', $root, '--port', "$port", '--ready-file', $ready)
$server = Start-Process -FilePath $helper `
    -ArgumentList $serverArgs `
    -PassThru `
    -WindowStyle Hidden
$testProfile = Join-Path $env:RUNNER_TEMP "cup-installer-profile-$PID"
$originalEnvironment = @{}
foreach ($name in @(
    "USERPROFILE",
    "CUP_INSTALL_ALLOW_INSECURE",
    "CUP_INSTALL_BASE_URL",
    "CUP_INSTALL_NO_PATH_PROMPT"
)) {
    $item = Get-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    $originalEnvironment[$name] = if ($null -eq $item) { $null } else { $item.Value }
}

try {
    $serverReady = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            if (-not (Test-Path -LiteralPath $ready)) {
                throw "not ready"
            }
            Invoke-WebRequest -UseBasicParsing `
                -Uri "http://127.0.0.1:$port/release.txt" | Out-Null
            $serverReady = $true
            break
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $serverReady) {
        throw "HTTP test helper did not become ready"
    }

    if (Test-Path -LiteralPath $testProfile) {
        Remove-Item -LiteralPath $testProfile -Recurse -Force
    }
    New-Item -ItemType Directory -Path $testProfile | Out-Null

    $env:USERPROFILE = $testProfile
    $env:CUP_INSTALL_ALLOW_INSECURE = "1"
    $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port"
    $env:CUP_INSTALL_NO_PATH_PROMPT = "1"

    powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $ReleaseDir "install.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Windows installer failed with exit code $LASTEXITCODE"
    }
    $installed = Join-Path $env:USERPROFILE ".cup\bin\cup.exe"
    if (-not (Test-Path -LiteralPath $installed -PathType Leaf)) {
        throw "Windows installer did not create $installed"
    }
    $installedVersion = & $installed --version
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup --version failed with exit code $LASTEXITCODE"
    }
    if ($installedVersion -ne "cup $Version") {
        throw "Unexpected installed version: $installedVersion"
    }
    $candidateHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($installedHash -ne $candidateHash) {
        throw "Installed cup does not match the tested release candidate"
    }
    & $installed doctor
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup doctor failed with exit code $LASTEXITCODE"
    }

    # Repair may recreate mutable runtime paths, but it must preserve the installed
    # executable exactly as POSIX repair preserves ~/.cup/bin/cup.
    $binaryHashBeforeRepair = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    $stagingDirectory = Join-Path $env:USERPROFILE ".cup\staging"
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force -ErrorAction SilentlyContinue
    & $installed repair
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup repair failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $stagingDirectory -PathType Container)) {
        throw "cup repair did not recreate the staging directory"
    }
    $binaryHashAfterRepair = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($binaryHashAfterRepair -ne $binaryHashBeforeRepair) {
        throw "cup repair replaced the running executable"
    }
    $versionAfterRepair = & $installed --version
    if ($LASTEXITCODE -ne 0 -or $versionAfterRepair -ne "cup $Version") {
        throw "Installed cup was not usable after repair"
    }


    # A completion marker is accepted only for a complete installed generation. The failed
    # recovery must preserve both cup.exe and the staging evidence.
    $bootstrapStaging = Join-Path $env:USERPROFILE ".cup\.bootstrap"
    $committedMarker = Join-Path $bootstrapStaging "committed"
    $updateHelper = Join-Path $env:USERPROFILE ".cup\helpers\cup-update-helper.exe"
    $savedUpdateHelper = Join-Path $env:USERPROFILE "saved-cup-update-helper.exe"
    New-Item -ItemType Directory -Path $bootstrapStaging | Out-Null
    New-Item -ItemType File -Path $committedMarker | Out-Null
    Move-Item -LiteralPath $updateHelper -Destination $savedUpdateHelper
    $incompleteOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $ReleaseDir "install.ps1") 2>&1
    if ($LASTEXITCODE -eq 0) {
        throw "Incomplete committed Windows bootstrap staging unexpectedly succeeded"
    }
    if (($incompleteOutput -join "`n") -notlike
        "*completed bootstrap staging does not match a complete installed generation*") {
        throw "Incomplete committed Windows bootstrap failure was not explained"
    }
    if (-not (Test-Path -LiteralPath $committedMarker -PathType Leaf)) {
        throw "Incomplete committed Windows bootstrap staging was not preserved"
    }
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
        $binaryHashBeforeRepair) {
        throw "Incomplete committed Windows bootstrap recovery changed cup.exe"
    }
    Move-Item -LiteralPath $savedUpdateHelper -Destination $updateHelper

    # Reinstall the same tested candidate, complete cleanup and keep the executable valid.
    powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $ReleaseDir "install.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Windows reinstall failed with exit code $LASTEXITCODE"
    }
    if (Test-Path -LiteralPath $bootstrapStaging) {
        throw "Windows reinstall did not remove completed bootstrap staging"
    }
    $binaryHashAfterReinstall = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($binaryHashAfterReinstall -ne $candidateHash) {
        throw "Windows reinstall changed the tested release executable"
    }
    $versionAfterReinstall = & $installed --version
    if ($LASTEXITCODE -ne 0 -or $versionAfterReinstall -ne "cup $Version") {
        throw "Installed cup was not usable after reinstall"
    }

    # The assembled candidate performs its detached uninstall smoke test.
    $uninstallOutput = & $installed uninstall --yes 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup uninstall failed with exit code $LASTEXITCODE"
    }
    if (($uninstallOutput -join "`n") -notlike
        "*Uninstall started. The PATH entry was not removed.*") {
        throw "Installed cup uninstall did not report detached removal"
    }
    $cupRoot = Join-Path $env:USERPROFILE ".cup"
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ((Test-Path -LiteralPath $cupRoot) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $cupRoot) {
        throw "Release uninstall did not remove the cup root"
    }
    $residues = @(Get-ChildItem -LiteralPath $env:USERPROFILE -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall.*" })
    if ($residues.Count -ne 0) {
        throw "Release uninstall left staging behind: $($residues[0].FullName)"
    }
} finally {
    foreach ($name in $originalEnvironment.Keys) {
        $value = $originalEnvironment[$name]
        if ($null -eq $value) {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item -LiteralPath "Env:$name" -Value $value
        }
    }

    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $server.WaitForExit()
    }
    Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testProfile -Recurse -Force -ErrorAction SilentlyContinue
}
